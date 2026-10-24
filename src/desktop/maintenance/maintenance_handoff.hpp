#pragma once
#include "native_runtime.hpp"
#include "uninstall_inventory.hpp"
#include <QObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <functional>

namespace marksix::maintenance {
class MaintenanceHandoff final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    using WorkBusy = std::function<bool()>;
    using Launcher = std::function<bool(const QString&, const QStringList&, const QString&, qint64*)>;
    MaintenanceHandoff(fs::path root, WorkBusy workBusy, QObject* parent = nullptr,
        fs::path helper = {}, Launcher launcher = {}, fs::path stagingBase = {});
    bool busy() const { return phase_ != Phase::Idle && phase_ != Phase::Failed; }
    bool available() const { return available_; }
    QString status() const { return status_; }
    void setUninstallScopeProvider(std::function<UninstallScope()> provider) { scopeProvider_ = std::move(provider); }
    Q_INVOKABLE void start(int operation, bool recovery = false);
signals:
    void changed();
    void readyToExit(); // Only after authenticated helper acknowledges durable request.
private:
    enum class Phase { Idle, Connecting, Hello, Ready, Closing, Failed };
    void connected();
    void receive();
    void fail(const QString& reason);
    fs::path root_, source_, checksum_, staged_, stagingBase_;
    WorkBusy workBusy_;
    Launcher launcher_;
    std::function<UninstallScope()> scopeProvider_;
    QLocalServer server_;
    QLocalSocket* socket_ = nullptr;
    QTimer timeout_;
    std::unique_ptr<QTemporaryDir> stage_;
    std::optional<ProcessIdentity> helper_;
    QByteArray buffer_;
    std::string nonce_, expectedHash_;
    int operation_ = 0;
    bool recovery_ = false, available_ = false;
    Phase phase_ = Phase::Idle;
    QString status_;
};
}
