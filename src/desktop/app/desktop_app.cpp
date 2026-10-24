#include "analysis.hpp"
#include "statistics_models.hpp"
#include "statistics_loader.hpp"
#include "desktop_options.hpp"
#include "desktop_diagnostics.hpp"
#include "update_controller.hpp"
#include "storage_controller.hpp"
#include "workspace_session.hpp"
#include "native_dialog.hpp"
#include "maintenance_handoff.hpp"
#include "workspace_registration.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QObject>
#include <QString>
#include <QQuickItem>
#include <QQuickWindow>
#include <QIcon>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QStyleHints>

#include <cmath>
#include <algorithm>
#include <iostream>
#include <string_view>

int runDesktopApp(int argc, char* argv[]) {
    std::unique_ptr<marksix::maintenance::FileLease> desktopLease;
    // A blocked launch must not initialize Qt graphics caches while another
    // account's full cleanup is verifying their absence.
    try {
        if (marksix::maintenance::readMachineGate()) throw std::runtime_error("System cleanup is active or interrupted. Finish its standalone native helper before opening Mark Six Studio.");
        desktopLease = marksix::maintenance::acquireCurrentDesktopLease();
        if (marksix::maintenance::readMachineGate()) throw std::runtime_error("System cleanup began during startup. Finish its native helper before opening Mark Six Studio.");
    } catch (const std::exception& error) {
        bool diagnostics = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            diagnostics = diagnostics || argument.starts_with("--smoke") || argument.ends_with("-audit");
        }
        std::cerr << "Workspace unavailable before Qt initialization: " << error.what() << '\n';
        if (!diagnostics) marksix::maintenance::nativeStartupBlocked(error.what());
        return 3;
    }
    QElapsedTimer launchClock;
    launchClock.start();
    // Keep the exact baseline linked into the independent statistics desktop.
    const auto fair = marksix::statistics::baseline(marksix::statistics::Scope::Main);
    if (std::abs(fair.inclusion - 6.0 / 49.0) > 1e-12) return 2;

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    app.styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
    app.setApplicationName(QStringLiteral("Mark Six Studio"));
    app.setApplicationDisplayName(QStringLiteral("Mark Six Studio"));
    app.setApplicationVersion(QStringLiteral(MARKSIX_APP_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/icon.png")));
    const auto arguments = app.arguments();
    const auto options = parseDesktopOptions(arguments);
    if (!options) return 2;
    const auto& dataRoot = options->dataRoot;
    const auto& screenshotPath = options->screenshotPath;
    const auto& requestedSize = options->requestedSize;
    const auto requestedPage = options->requestedPage;
    const auto requestedComparison = options->requestedComparison;
    std::unique_ptr<marksix::maintenance::WorkspaceSession> session;
    try { session = std::make_unique<marksix::maintenance::WorkspaceSession>(dataRoot); }
    catch (const std::exception& error) {
        const bool diagnostics = std::any_of(arguments.begin(), arguments.end(), [](const auto& argument) {
            return argument.startsWith(QStringLiteral("--smoke")) || argument.endsWith(QStringLiteral("-audit"));
        });
        std::cerr << "Workspace unavailable: " << error.what() << '\n';
        bool pendingReset = false;
        try { pendingReset = marksix::maintenance::resetPending(dataRoot); } catch (const std::exception&) {}
        if (!diagnostics && pendingReset) {
            marksix::maintenance::MaintenanceHandoff recovery(dataRoot, [] { return false; });
            if (recovery.available() && marksix::maintenance::nativeReview("Interrupted reset", std::string(error.what()) +
                "\nThe workspace stays closed until recovery restores the old generation or finishes the approved purge.", "Recover reset", "Close")) {
                QObject::connect(&recovery, &marksix::maintenance::MaintenanceHandoff::readyToExit, &app, &QCoreApplication::quit);
                QObject::connect(&recovery, &marksix::maintenance::MaintenanceHandoff::changed, &app, [&] {
                    if (!recovery.busy()) { marksix::maintenance::nativeReview("Recovery could not start", recovery.status().toStdString()); app.quit(); }
                });
                recovery.start(1, true);
                if (recovery.busy()) return app.exec();
                return 3;
            }
        }
        if (!diagnostics) marksix::maintenance::nativeReview("Workspace unavailable", error.what());
        return 3;
    }
    QString registrationNotice;
    try { marksix::maintenance::rememberCurrentWorkspace(dataRoot); }
    catch (const std::exception& error) { registrationNotice = QStringLiteral("This workspace was not registered for future all-user cleanup: ") + QString::fromUtf8(error.what()); }
    StatisticsModels models;
    StatisticsLoader loader(dataRoot, models);
    marksix::updates::UpdateController updater(app.applicationVersion(),
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("updates")),
        [](const QUrl& url) { return QDesktopServices::openUrl(url); });
    updater.setInstallerLauncher(marksix::maintenance::launchNativeInstaller);
    marksix::maintenance::StorageController storage(dataRoot, marksix::maintenance::discoverUsers,
        [](const QUrl& url) { return QDesktopServices::openUrl(url); });
    storage.setRegistrationNotice(registrationNotice);
    marksix::maintenance::MaintenanceHandoff maintenance(dataRoot, [&] {
        return loader.busy() || loader.updateBusy() || loader.sourceWorkBusy() || loader.controlBusy() || updater.busy() || storage.busy();
    });
    maintenance.setUninstallScopeProvider([&] { return storage.uninstallScope(); });
    QObject::connect(&maintenance, &marksix::maintenance::MaintenanceHandoff::readyToExit, &app, [&] {
        QTimer::singleShot(0, &app, [&] { updater.preserveDownloadForMaintenance(); loader.close(); app.quit(); });
    });
    QObject::connect(&updater, &marksix::updates::UpdateController::installerStarted,
                     &app, &QCoreApplication::quit);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("statisticsModels"), &models);
    engine.rootContext()->setContextProperty(QStringLiteral("statisticsLoader"), &loader);
    engine.rootContext()->setContextProperty(QStringLiteral("appUpdater"), &updater);
    engine.rootContext()->setContextProperty(QStringLiteral("appStorage"), &storage);
    engine.rootContext()->setContextProperty(QStringLiteral("appMaintenance"), &maintenance);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("MarkSixStatisticsDesktop", "Main");
    const auto frontendStartupMs = launchClock.elapsed();
    if (engine.rootObjects().empty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (!window) return 1;
    if (requestedSize.isValid()) window->resize(requestedSize);
    window->setProperty("pageIndex", requestedPage);
    window->setProperty("comparisonIndex", requestedComparison);
    if (options->showLearningProgress) {
        if (auto* learning = window->findChild<QQuickItem*>(QStringLiteral("learningPage")))
            learning->setProperty("progressView", true);
    }
    return runDesktopDiagnostics(app, window, loader, models, arguments, screenshotPath, launchClock, frontendStartupMs);
}
