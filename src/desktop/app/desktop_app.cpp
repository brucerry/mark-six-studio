#include "analysis.hpp"
#include "statistics_models.hpp"
#include "statistics_loader.hpp"
#include "desktop_options.hpp"
#include "desktop_diagnostics.hpp"

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

#include <cmath>

int runDesktopApp(int argc, char* argv[]) {
    QElapsedTimer launchClock;
    launchClock.start();
    // Keep the exact baseline linked into the independent statistics desktop.
    const auto fair = marksix::statistics::baseline(marksix::statistics::Scope::Main);
    if (std::abs(fair.inclusion - 6.0 / 49.0) > 1e-12) return 2;

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Mark Six Studio"));
    app.setApplicationDisplayName(QStringLiteral("Mark Six Studio"));
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
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("statisticsModels"), &models);
    engine.rootContext()->setContextProperty(QStringLiteral("statisticsLoader"), &loader);
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
