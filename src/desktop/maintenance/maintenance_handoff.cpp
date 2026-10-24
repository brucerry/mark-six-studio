#include "maintenance_handoff.hpp"
#include "durable_files.hpp"
#include "wire_codec.hpp"
#include "private_channel.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>

namespace marksix::maintenance {
namespace {
QString qpath(const fs::path& path) { return QString::fromStdU16String(path.u16string()); }
fs::path helperLocation() {
    const auto app = fs::path(QCoreApplication::applicationDirPath().toStdU16String());
#ifdef _WIN32
    return app / "mark-six-maintenance.exe";
#else
    return app.parent_path() / "Helpers/mark-six-maintenance";
#endif
}
}
MaintenanceHandoff::MaintenanceHandoff(fs::path root, WorkBusy workBusy, QObject* parent, fs::path helper, Launcher launcher, fs::path stagingBase)
    : QObject(parent), root_(std::move(root)), source_(helper.empty() ? helperLocation() : std::move(helper)),
      checksum_(source_.parent_path() / "maintenance-helper.sha256"), stagingBase_(std::move(stagingBase)), workBusy_(std::move(workBusy)), launcher_(std::move(launcher)) {
    if (!launcher_) launcher_ = [](const auto& program, const auto& arguments, const auto& directory, qint64* pid) {
        QProcess process;
        process.setProgram(program); process.setArguments(arguments); process.setWorkingDirectory(directory);
        auto environment = QProcessEnvironment::systemEnvironment();
#ifdef __APPLE__
        for (const auto& name : environment.keys()) if (name.startsWith(QStringLiteral("DYLD_"))) environment.remove(name);
        environment.remove(QStringLiteral("__CFBundleIdentifier"));
#endif
        process.setProcessEnvironment(environment);
        return process.startDetached(pid);
    };
#ifdef __APPLE__
    if (source_.parent_path().filename() == "Helpers" && source_.parent_path().parent_path().filename() == "Contents")
        checksum_ = source_.parent_path().parent_path() / "Resources/maintenance-helper.sha256";
#endif
    QString unavailable;
    try {
        expectedHash_ = readBoundedFile(checksum_, 80);
        if (expectedHash_.ends_with("\r\n")) expectedHash_.resize(expectedHash_.size() - 2);
        else if (expectedHash_.ends_with("\n")) expectedHash_.pop_back();
        if (expectedHash_.size() != 64 || expectedHash_.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("The helper checksum must contain exactly 64 hexadecimal digits");
        if (sha256File(source_) != expectedHash_) throw std::runtime_error("The native helper does not match its packaged checksum");
        available_ = true;
    } catch (const std::exception& error) { available_ = false; unavailable = QString::fromUtf8(error.what()); }
    status_ = available_ ? QStringLiteral("The app will close before a native review asks for final deletion approval.") :
        QStringLiteral("The verified native maintenance helper is unavailable. Reinstall a complete supported package before cleanup.\n") + unavailable;
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] { fail(QStringLiteral("The helper did not acknowledge the request. No cleanup was approved.")); });
    connect(&server_, &QLocalServer::newConnection, this, &MaintenanceHandoff::connected);
}
void MaintenanceHandoff::fail(const QString& reason) {
    timeout_.stop(); server_.close();
    if (socket_) { socket_->abort(); socket_->deleteLater(); socket_ = nullptr; }
    phase_ = Phase::Failed; status_ = reason;
    if (stage_ && !stage_->autoRemove()) status_ += QStringLiteral("\nTemporary helper/retry location: ") + stage_->path();
    else stage_.reset();
    emit changed();
}
void MaintenanceHandoff::start(int operation, bool recovery) {
    if (busy()) return;
    if (!available_ || operation < 0 || operation > 2 || (workBusy_ && workBusy_())) {
        status_ = !available_ ? QStringLiteral("The verified maintenance helper is unavailable.") :
            QStringLiteral("Finish or cancel analysis, import, history updates and app downloads before maintenance.");
        emit changed(); return;
    }
    try {
        if (identifyFile(source_).sha256 != expectedHash_) throw std::runtime_error("Maintenance helper changed; reinstall before continuing");
        const auto parent = processIdentity(currentProcessId());
        if (!parent || parent->uncertain) throw std::runtime_error("Cannot resolve initiating process identity");
        auto base = normalizePlatformPath(stagingBase_.empty() ? fs::path(QDir::tempPath().toStdU16String()) : stagingBase_);
        const auto relative = base.lexically_relative(root_);
        if (!relative.empty() && *relative.begin() != "..") base = normalizePlatformPath(fs::path(QDir::homePath().toStdU16String()));
        validateContainedPath(base / ".stage-probe", base);
        const auto safe = base.lexically_relative(root_);
        if (!safe.empty() && *safe.begin() != "..") throw std::runtime_error("No helper staging location exists outside this data root");
        stage_ = std::make_unique<QTemporaryDir>(qpath(base) + QStringLiteral("/marksix-maintenance-XXXXXX"));
        if (!stage_->isValid()) throw std::runtime_error("Cannot reserve private helper staging");
        const auto directory = normalizePlatformPath(fs::path(stage_->path().toStdU16String()));
        staged_ = directory / source_.filename();
        validateContainedPath(staged_, directory);
        if (!QFile::copy(qpath(source_), qpath(staged_)) || identifyFile(staged_).sha256 != expectedHash_)
            throw std::runtime_error("Staged maintenance helper failed integrity verification");
        QFile::setPermissions(qpath(staged_), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        nonce_ = randomToken(); buffer_.clear(); operation_ = operation; recovery_ = recovery;
        const auto name = QStringLiteral("marksix-maint-") + QString::fromStdString(nonce_.substr(0, 24));
        if (!server_.listen(name)) throw std::runtime_error("Cannot create a private maintenance channel");
        phase_ = Phase::Connecting;
        qint64 pid = 0;
        const QStringList arguments{QStringLiteral("--channel"), server_.fullServerName(), QString::fromStdString(nonce_),
            QString::number(parent->pid), QString::number(parent->started)};
        if (!launcher_(qpath(staged_), arguments, qpath(directory), &pid) || pid <= 0 || quint64(pid) > UINT32_MAX)
            throw std::runtime_error("Cannot launch the native maintenance helper");
        stage_->setAutoRemove(false);
        helper_ = processIdentity(std::uint32_t(pid));
        if (!helper_ || helper_->uncertain || helper_->account != parent->account || helper_->executable != staged_)
            throw std::runtime_error("Launched helper process identity mismatch");
        status_ = QStringLiteral("Preparing the native review. No inventoried files have been deleted.");
        timeout_.start(30000); emit changed();
    } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
}
void MaintenanceHandoff::connected() {
    while (server_.hasPendingConnections()) {
        auto* socket = server_.nextPendingConnection();
        try {
            if (socket_ || phase_ != Phase::Connecting || !helper_ ||
                channelPeer(socket->socketDescriptor(), true) != helper_->pid || !sameProcessAlive(*helper_) ||
                identifyFile(staged_).sha256 != expectedHash_) throw std::runtime_error("Unexpected maintenance peer");
            socket_ = socket; phase_ = Phase::Hello;
            connect(socket_, &QLocalSocket::readyRead, this, &MaintenanceHandoff::receive);
            connect(socket_, &QLocalSocket::disconnected, this, [this] {
                receive();
                if (phase_ != Phase::Closing) fail(QStringLiteral("The helper disconnected before shutdown approval. No cleanup was approved."));
            });
            receive();
        } catch (const std::exception&) { socket->abort(); socket->deleteLater(); }
    }
}
void MaintenanceHandoff::receive() {
    if (!socket_ || phase_ == Phase::Closing) return;
    try {
        buffer_ += socket_->readAll();
        if (buffer_.size() > 65536) throw std::runtime_error("Maintenance handshake exceeds limit");
        if (buffer_.size() < 8) return;
        Decoder header(std::string_view(buffer_.constData(), 8)); const auto size = header.number();
        if (size > 65528) throw std::runtime_error("Invalid maintenance handshake size");
        if (quint64(buffer_.size() - 8) < size) return;
        const std::string message(buffer_.constData() + 8, std::size_t(size)); buffer_.remove(0, qsizetype(size + 8));
        Decoder input(message); const auto type = input.text();
        if (input.text() != nonce_) throw std::runtime_error("Maintenance operation nonce mismatch");
        input.end();
        if (phase_ == Phase::Hello && type == "M6HELLO1") {
            Encoder request; request.text("M6USERREQUEST1"); request.number(std::uint64_t(operation_));
            request.path(root_); request.number(recovery_); request.path({}); request.text(currentAccount());
            if (operation_ == 2) {
                const auto scope = scopeProvider_ ? scopeProvider_() : UninstallScope{{{currentAccount(), root_, LocationKind::SavedData}}};
                request.text(encodeUninstallScope(scope)); request.text(nonce_);
            }
            Encoder body; body.text(nonce_); body.text(request.bytes);
            Encoder frame; frame.number(body.bytes.size()); frame.bytes += body.bytes;
            if (socket_->write(frame.bytes.data(), qint64(frame.bytes.size())) != qint64(frame.bytes.size())) throw std::runtime_error("Cannot send maintenance scope");
            socket_->flush(); phase_ = Phase::Ready;
        } else if (phase_ == Phase::Ready && type == "M6READY1") {
            timeout_.stop(); phase_ = Phase::Closing;
            status_ = QStringLiteral("Closing app workers and storage handles. The native review will appear after this app exits.");
            emit changed(); emit readyToExit();
        } else throw std::runtime_error("Unexpected maintenance handshake state");
        if (!buffer_.isEmpty() && phase_ != Phase::Closing) receive();
    } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
}
}
