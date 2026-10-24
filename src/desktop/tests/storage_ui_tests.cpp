#include "storage_controller.hpp"
#include "native_runtime.hpp"
#include "workspace_session.hpp"
#include "private_channel.hpp"
#include "wire_codec.hpp"
#include "maintenance_handoff.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QLocalServer>
#include <QLocalSocket>
#include <fstream>
#include <iostream>
#include <algorithm>

using namespace marksix::maintenance;
namespace {
int checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void ready(StorageController& controller) {
    QElapsedTimer elapsed; elapsed.start();
    while (controller.busy() && elapsed.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(2); }
    check(!controller.busy(), "Asynchronous inspection finishes");
}
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary); file << text;
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir temporary; check(temporary.isValid(), "Private fixture exists");
        const auto base = fs::canonical(fs::path(temporary.path().toStdU16String()));
        UserStorage first{currentAccount(), base / "home", base / "home/data", base / "home/cache", base / "home/Downloads", base / "home/maintenance/ownership.bin", {}, {}, true};
        auto second = first; second.account = "other-account"; second.home = base / "other";
        second.dataRoot = second.home / "data"; second.cacheRoot = second.home / "cache"; second.downloads = second.home / "Downloads";
        second.manifestPath = second.home / "maintenance/ownership.bin";
        const auto old = first.dataRoot / "preserved.MSX", other = second.dataRoot / "older.msx";
        write(old, "opaque legacy"); write(other, "other legacy"); write(first.cacheRoot / "regenerable", "cache");
        const auto before = sha256File(old);
        bool opened = false; bool fail = false; int catalogFetches = 0;
        write(first.downloads / "downloaded-legacy.msx", "old downloaded recording");
        StorageController controller(first.dataRoot, [&](bool all, std::vector<InventoryIssue>& issues) {
            if (fail) throw std::runtime_error("profile inspection denied");
            if (all) { issues.push_back({base / "offline", "offline-account", "Home is offline", true}); return std::vector<UserStorage>{first, second}; }
            return std::vector<UserStorage>{first};
        }, [&](const QUrl& url) { opened = url.isLocalFile(); return opened; }, nullptr,
        [](const auto&, auto&) { return std::vector<InstalledApplication>{}; },
        [&] { ++catalogFetches; return ReleaseCatalog{}; });
        controller.refresh(); ready(controller);
        check(controller.preservedFiles().size() == 1, "Current-user list is scoped correctly");
        const auto row = controller.preservedFiles().front().toMap();
        check(row.value("path").toString() == QString::fromStdU16String(old.u16string()) &&
            row.value("account").toString() == QString::fromStdString(currentAccount()) && !row.value("reason").toString().isEmpty(),
            "UI provides full preserved path, account and reason");
        check(controller.preservedReport().contains("Preserved legacy files (1)"), "Copyable report has a clear label");
        check(controller.openFolder(QString::fromStdU16String(first.dataRoot.u16string())) && opened, "Reviewed accessible folder can be opened");
        check(!controller.openFolder(QString::fromStdU16String(base.u16string())), "Unreviewed folder cannot be opened through controller");
#ifdef __APPLE__
        const auto nativeCache = first.home / "Library/Caches/com.brucerry.marksixstudio";
        write(nativeCache / "graphics.bin", "native cache fixture"); controller.refresh(); ready(controller);
        check(controller.openFolder(QString::fromStdU16String(nativeCache.u16string())), "Native bundle cache location is visible and can be opened from the reviewed page");
#endif
        controller.refresh(true); ready(controller);
        check(controller.preservedFiles().size() == 2 && controller.preservedReport().contains("other-account"), "All-user review clearly lists each account");
        check(controller.coverage().contains("offline-account") && controller.status().contains("unresolved"), "Inaccessible profiles are disclosed rather than counted empty");
        const auto custom = first.home / "custom"; write(custom / "another.msx", "legacy custom");
        controller.addLocation(QUrl::fromLocalFile(QString::fromStdU16String(custom.u16string())), false); ready(controller);
        check(controller.preservedFiles().size() == 3, "Explicit additional data folder is inspected");
        controller.addLocation(QUrl::fromLocalFile(QString::fromStdU16String(first.home.u16string())), false);
        check(!controller.busy() && controller.status() != "Inspecting local files without changing saved records…", "Home-directory selection is rejected");
        check(sha256File(old) == before, "Every review preserves original recording bytes");
        check(catalogFetches == 0, "Opening or refreshing storage performs no automatic release request");
        controller.verifyDownloads(); ready(controller);
        check(catalogFetches == 1 && controller.preservedFiles().size() == 4, "Explicit download verification also lists preserved downloaded recordings");
        controller.refresh(); ready(controller);
        check(catalogFetches == 1, "Subsequent local inspection reuses verified identities without a network request");
        const auto otherCustom = second.home / "older-custom-root"; write(otherCustom / "other-custom.msx", "other account opaque recording");
        controller.addLocation(QUrl::fromLocalFile(QString::fromStdU16String(otherCustom.u16string())), false, QStringLiteral("other-account")); ready(controller);
        const auto scope = controller.uninstallScope();
        check(std::any_of(scope.additional.begin(), scope.additional.end(), [&](const auto& location) { return location.account == "other-account" && location.path == otherCustom; }),
            "Selected additional folders retain their real native account in the administrator handoff");
        check(controller.preservedReport().contains(QStringLiteral("other-custom.msx")) && controller.accounts().size() == 2, "Another account's selected root and preserved legacy files appear in the review");
        fail = true; controller.refresh(); ready(controller);
        check(controller.status().contains("Inspection failed"), "Discovery failure is visible");
        const auto gated = base / "gated"; fs::create_directories(resetGatePath(gated));
        bool rejected = false; try { WorkspaceSession session(gated); } catch (const std::exception&) { rejected = true; }
        check(rejected, "Incomplete reset prevents workspace startup");
        fs::remove(resetGatePath(gated));
        MachineGate machine{randomToken(), base / "protected-native-stage", *processIdentity(currentProcessId())};
        const auto startup = base / "blocked-system-workspace";
        rejected = false;
        try { WorkspaceSession blocked(startup, [&] { return std::optional<MachineGate>(machine); }); } catch (const std::exception&) { rejected = true; }
        check(rejected && !fs::exists(startup), "System cleanup blocks new workspaces before creating a directory, model or database");
        int gateReads = 0; rejected = false;
        try { WorkspaceSession raced(startup, [&]() -> std::optional<MachineGate> { return ++gateReads == 1 ? std::nullopt : std::optional<MachineGate>(machine); }); }
        catch (const std::exception&) { rejected = true; }
        check(rejected && gateReads == 2 && bool(FileLease::acquire(sessionLeasePath(startup), FileLease::Mode::Exclusive)),
            "A system gate published during startup closes the race and releases the rejected workspace lease");
        WorkspaceSession session(gated);
        check(!FileLease::acquire(sessionLeasePath(gated), FileLease::Mode::Exclusive), "Live workspace prevents exclusive cleanup");
        QLocalServer server; server.setSocketOptions(QLocalServer::UserAccessOption);
        check(server.listen(QStringLiteral("m6test-") + QString::fromStdString(randomToken().substr(0, 24))), "Private IPC fixture listens");
        const auto parent = *processIdentity(currentProcessId());
        auto client = std::async(std::launch::async, [&] {
            PrivateChannel channel(server.fullServerName().toStdString(), parent);
            channel.send("nonce-bound hello"); return channel.receive();
        });
        check(server.waitForNewConnection(3000), "Native helper connects to private channel");
        std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
        check(channelPeer(socket->socketDescriptor(), true) == currentProcessId(), "Native peer PID is authenticated by OS");
        QByteArray message;
        while (message.size() < 25 && socket->waitForReadyRead(3000)) message += socket->readAll();
        check(message.size() == 25, "Length-prefixed native message arrives intact");
        Decoder frame(std::string_view(message.constData(), std::size_t(message.size())));
        check(frame.number() == 17 && frame.bytes == "nonce-bound hello", "IPC content uses bounded binary framing");
        Encoder response; response.number(8); response.bytes += "approved";
        socket->write(response.bytes.data(), qint64(response.bytes.size())); socket->flush();
        check(client.get() == "approved", "Native helper reads framed acknowledgement");
        socket.reset();
        auto staleParent = parent; ++staleParent.started;
        auto wrongPeer = std::async(std::launch::async, [&] {
            try { PrivateChannel channel(server.fullServerName().toStdString(), staleParent); return false; }
            catch (const std::exception&) { return true; }
        });
        check(server.waitForNewConnection(3000), "Changed-peer fixture connects");
        std::unique_ptr<QLocalSocket> rejectedSocket(server.nextPendingConnection());
        check(wrongPeer.get(), "A reused server PID is rejected before request transfer");
        const auto helperFile = base / "native/mark-six-maintenance";
        write(helperFile, "isolated fake helper; never executed");
        write(helperFile.parent_path() / "maintenance-helper.sha256", sha256File(helperFile) + "\n");
        write(helperFile.parent_path() / "maintenance-helper.sha256", sha256File(helperFile) + "\r\n");
        MaintenanceHandoff nativeNewline(first.dataRoot, [] { return false; }, nullptr, helperFile, {}, base);
        check(nativeNewline.available(), "A native Windows checksum newline retains exact SHA-256 verification");
        write(helperFile.parent_path() / "maintenance-helper.sha256", sha256File(helperFile) + "\n");
        int launches = 0; bool working = true;
        MaintenanceHandoff handoff(first.dataRoot, [&] { return working; }, nullptr, helperFile,
            [&](const auto&, const auto&, const auto&, qint64*) { ++launches; return false; }, base);
        check(handoff.available(), "Helper hash is verified before enabling a handoff");
        handoff.start(0);
        check(launches == 0 && !handoff.busy(), "Live application work blocks handoff before staging or launch");
        working = false; handoff.start(0);
        check(launches == 1 && !handoff.busy() && handoff.status().contains("Cannot launch"), "Launch failure leaves application open and reports no cleanup approval");
        write(helperFile, "substituted after initial inspection"); handoff.start(1);
        check(launches == 1 && handoff.status().contains("changed"), "Substituted helper cannot start under an earlier integrity check");
        MaintenanceHandoff altered(first.dataRoot, [] { return false; }, nullptr, helperFile, {}, base);
        check(!altered.available(), "Checksum mismatch keeps maintenance unavailable");
        check(sha256File(old) == before, "Failed handoffs leave saved legacy bytes untouched");
        const auto appRoot = first.home / "reviewed-product-copy";
        const auto renamed = appRoot / "retained-recording.dat";
        write(renamed, "opaque renamed recording");
        OwnershipManifest manifest; manifest.recordings.push_back({renamed, std::string(64, '0')}); writeManifest(first.manifestPath, manifest);
        StorageController payloadPreview(first.dataRoot, [&](bool, auto&) { return std::vector<UserStorage>{first}; }, [](const auto&) { return false; }, nullptr,
            [&](const auto&, auto&) { return std::vector<InstalledApplication>{{appRoot, appRoot / "unverified-fixture-executable", first.account, {}}}; },
            [] { return ReleaseCatalog{}; });
        payloadPreview.refresh(); ready(payloadPreview);
        check(payloadPreview.preservedReport().contains(QString::fromStdString(quotedReportField(pathUtf8(renamed)))) &&
            payloadPreview.preservedReport().contains(QString::fromStdString(first.account)),
            "Storage preview lists renamed legacy payload hints with full paths and native accounts before payload inspection");
#ifdef __APPLE__
        const auto bundle = first.home / "Applications/Mark 六 Studio.app";
        write(bundle / "Contents/Info.plist", "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
            "<key>CFBundleIdentifier</key><string>com.brucerry.marksixstudio</string>"
            "<key>CFBundlePackageType</key><string>APPL</string>"
            "<key>CFBundleExecutable</key><string>mark-six-studio</string></dict></plist>");
        write(bundle / "Contents/MacOS/mark-six-studio", "private fixture; never executed");
        QUrl openedUrl; int folderCalls = 0; bool allowOpen = true;
        StorageController bundlePreview(first.dataRoot, [&](bool, auto&) { return std::vector<UserStorage>{first}; },
            [&](const QUrl& url) { openedUrl = url; ++folderCalls; return allowOpen; }, nullptr,
            [&](const auto&, auto&) { return std::vector<InstalledApplication>{{bundle, bundle / "Contents/MacOS/mark-six-studio", {}, {}}}; });
        bundlePreview.refresh(); ready(bundlePreview);
        const auto bundlePath = QString::fromStdU16String(bundle.u16string());
        const auto parentPath = QString::fromStdU16String(bundle.parent_path().u16string());
        const auto applicationRow = bundlePreview.locations().back().toMap();
        check(applicationRow.value("path").toString() == bundlePath &&
            applicationRow.value("openLabel").toString() == "Open containing folder" &&
            applicationRow.value("openPath").toString() == parentPath,
            "Application row keeps the reviewed bundle path and labels its actual folder action");
        check(bundlePreview.openFolder(bundlePath) && folderCalls == 1 && openedUrl.isLocalFile() && openedUrl.toLocalFile() == parentPath,
            "Opening a reviewed app location sends its containing folder to the OS, never the app bundle");
        const auto dataPath = QString::fromStdU16String(first.dataRoot.u16string());
        check(bundlePreview.openFolder(dataPath) && openedUrl.toLocalFile() == dataPath,
            "Ordinary reviewed storage folders still open directly");
        allowOpen = false;
        check(!bundlePreview.openFolder(bundlePath) && bundlePreview.status().contains("Could not open folder") &&
            bundlePreview.status().contains(parentPath), "Failed OS folder opening has visible feedback");
        fs::remove_all(bundle); const auto callsBefore = folderCalls;
        check(!bundlePreview.openFolder(bundlePath) && folderCalls == callsBefore && bundlePreview.status().contains("unavailable"),
            "A disappeared application is reported without opening a stale location");
#endif
        std::cout << checks << " storage controller assertions passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
