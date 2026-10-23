#include "analysis.hpp"
#include "statistics_models.hpp"
#include "statistics_loader.hpp"
#include "desktop_options.hpp"
#include "desktop_diagnostics.hpp"
#include "update_controller.hpp"

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

int runDesktopApp(int argc, char* argv[]) {
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
    StatisticsModels models;
    StatisticsLoader loader(dataRoot, models);
    marksix::updates::UpdateController updater(app.applicationVersion(),
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("updates")),
        [](const QUrl& url) { return QDesktopServices::openUrl(url); });
    QObject::connect(&updater, &marksix::updates::UpdateController::installerStarted,
                     &app, &QCoreApplication::quit);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("statisticsModels"), &models);
    engine.rootContext()->setContextProperty(QStringLiteral("statisticsLoader"), &loader);
    engine.rootContext()->setContextProperty(QStringLiteral("appUpdater"), &updater);
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
