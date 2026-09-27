#include "desktop_diagnostics.hpp"
#include "statistics_loader.hpp"
#include "statistics_models.hpp"

#include <QAccessible>
#include <QDir>
#include <QImage>
#include <QKeyEvent>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace {
double processCpuMs() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return double(k.QuadPart + u.QuadPart) / 10000.0;
}
size_t peakWorkingSet() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
        sizeof(memory)) ? memory.PeakWorkingSetSize : 0;
}
}

int runDesktopDiagnostics(QGuiApplication& app, QQuickWindow* window, StatisticsLoader& loader,
                          StatisticsModels& models, const QStringList& arguments,
                          const QString& screenshotPath, QElapsedTimer& launchClock,
                          qint64 frontendStartupMs) {
    const auto capture = [&]() {
        if (screenshotPath.isEmpty()) return true;
        return window->grabWindow().save(screenshotPath);
    };
    const bool smoke = arguments.contains(QStringLiteral("--smoke"));
    const bool layoutAudit = arguments.contains(QStringLiteral("--layout-audit"));
    if (!smoke && !layoutAudit && !arguments.contains(QStringLiteral("--resize-audit")) &&
        !arguments.contains(QStringLiteral("--accessibility-audit"))) loader.reload();
    if (smoke) QTimer::singleShot(900, &app, [&] { QCoreApplication::exit(capture() ? 0 : 5); });
    if (arguments.contains(QStringLiteral("--loading-audit"))) {
        QTimer::singleShot(700, &app, [&] {
            auto* cover = window->findChild<QQuickItem*>(QStringLiteral("busyCover"));
            const bool okay = cover && cover->isVisible() &&
                !window->findChild<QQuickItem*>(QStringLiteral("loadingEstimate"));
            QCoreApplication::exit(okay && capture() ? 0 : 16);
        });
    }
    if (layoutAudit) QTimer::singleShot(250, &app, [&] {
        auto* grid = window->findChild<QQuickItem*>(QStringLiteral("predictionTiles"));
        auto* content = window->findChild<QQuickItem*>(QStringLiteral("content"));
        if (!grid || !content) {
            std::cerr << "LAYOUT_OBJECTS grid=" << bool(grid) << " content=" << bool(content) << '\n';
            QCoreApplication::exit(6); return;
        }
        bool valid = true;
        for (const auto& size : {QSize(800, 600), QSize(960, 640), QSize(1280, 800)}) {
            window->resize(size);
            QCoreApplication::processEvents();
            QList<QQuickItem*> tiles;
            for (auto* child : grid->childItems())
                if (child->objectName() == QStringLiteral("predictionTile")) tiles.push_back(child);
            if (tiles.size() != 6 || grid->width() > content->width()) valid = false;
            for (auto* tile : tiles) {
                const QPointF pos = tile->mapToItem(grid, QPointF(0, 0));
                if (tile->width() < 100 || tile->height() < 90 || pos.x() < -1 ||
                    pos.x() + tile->width() > grid->width() + 1) valid = false;
            }
            std::cout << "LAYOUT " << size.width() << 'x' << size.height() << " tiles=" << tiles.size()
                      << " grid=" << grid->width() << " viewport=" << content->width() << '\n';
        }
        QCoreApplication::exit(valid && capture() ? 0 : 6);
    });
    if (arguments.contains(QStringLiteral("--navigation-audit"))) {
        QTimer::singleShot(20, &app, [&] {
            for (int page = 0; page < 5; ++page) window->setProperty("pageIndex", page);
            window->setProperty("pageIndex", 0);
            if (window->property("pageIndex").toInt() != 0) QCoreApplication::exit(7);
            else QTimer::singleShot(20, &app, &QCoreApplication::quit);
        });
    }
    if (arguments.contains(QStringLiteral("--interaction-audit"))) {
        auto* cover = window->findChild<QQuickItem*>(QStringLiteral("busyCover"));
        const bool coveredDuringLoad = loader.busy() && cover && cover->isVisible();
        auto* check = new QTimer(&app);
        check->setInterval(50);
        QObject::connect(check, &QTimer::timeout, &app, [&] {
            if (loader.busy()) return;
            check->stop();
            window->setProperty("pageIndex", 3);
            QCoreApplication::processEvents();
            auto* outer = window->findChild<QQuickItem*>(QStringLiteral("content"));
            auto* draw = window->findChild<QQuickItem*>(QStringLiteral("drawTable"));
            auto* table = draw ? draw->findChild<QQuickItem*>(QStringLiteral("innerTable")) : nullptr;
            bool okay = coveredDuringLoad && cover && !cover->isVisible() &&
                !app.windowIcon().isNull() && outer && draw && table && models.archive()->rowCount() > 100;
            if (okay) {
                outer->setProperty("contentY", 40.0);
                table->setProperty("contentY", 80.0);
                QCoreApplication::processEvents();
                const QPointF initial = table->mapToScene(QPointF(table->width() / 2, table->height() / 2));
                outer->setProperty("contentY", outer->property("contentY").toDouble() +
                    initial.y() - window->height() / 2.0);
                QCoreApplication::processEvents();
                const double outerBefore = outer->property("contentY").toDouble();
                const double innerBefore = table->property("contentY").toDouble();
                const QPointF pos = table->mapToScene(QPointF(table->width() / 2, table->height() / 2));
                QWheelEvent wheel(pos, window->mapToGlobal(pos.toPoint()), QPoint(), QPoint(0, -120),
                                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
                QCoreApplication::sendEvent(window, &wheel);
                QCoreApplication::processEvents();
                const double outerAfter = outer->property("contentY").toDouble();
                const double innerAfter = table->property("contentY").toDouble();
                const bool resized = QMetaObject::invokeMethod(draw, "resizeColumn",
                    Q_ARG(QVariant, QVariant(0)), Q_ARG(QVariant, QVariant(240.0)));
                const double width = draw->property("columnWidths").toMap().value(QStringLiteral("0")).toDouble();
                std::cout << "INTERACTION_SCROLL pos=" << pos.x() << ',' << pos.y()
                          << " outer=" << outerBefore << "->" << outerAfter
                          << " inner=" << innerBefore << "->" << innerAfter
                          << " wheel_events=" << draw->property("wheelEvents").toInt()
                          << " resize=" << resized << " width=" << width << '\n';
                okay = std::abs(outerAfter - outerBefore) < 0.1 && innerAfter > innerBefore &&
                    resized && std::abs(width - 240.0) < 0.1;
            }
            std::cout << "INTERACTION_AUDIT=" << (okay ? "PASS" : "FAIL") << '\n';
            QCoreApplication::exit(okay ? 0 : 15);
        });
        check->start();
        QTimer::singleShot(120000, &app, [] { QCoreApplication::exit(15); });
    }
    if (arguments.contains(QStringLiteral("--accessibility-audit"))) {
        QTimer::singleShot(350, &app, [&] {
            auto* license = window->findChild<QQuickItem*>(QStringLiteral("licenseButton"));
            auto* update = window->findChild<QQuickItem*>(QStringLiteral("updateButton"));
            auto* dialog = window->findChild<QObject*>(QStringLiteral("licenseDialog"));
            bool okay = license && update && dialog;
            std::cout << "ACCESSIBILITY_OBJECTS license=" << bool(license) << " update=" << bool(update)
                      << " dialog=" << bool(dialog) << '\n';
            if (okay) {
                auto* licenseAccess = QAccessible::queryAccessibleInterface(license);
                auto* updateAccess = QAccessible::queryAccessibleInterface(update);
                std::cout << "ACCESSIBILITY_NAMES license=" << (licenseAccess ? licenseAccess->text(QAccessible::Name).toStdString() : "<none>")
                          << " update=" << (updateAccess ? updateAccess->text(QAccessible::Name).toStdString() : "<none>") << '\n';
                okay = licenseAccess && updateAccess &&
                    licenseAccess->text(QAccessible::Name) == QStringLiteral("License") &&
                    updateAccess->text(QAccessible::Name) == QStringLiteral("Update history");
                license->forceActiveFocus(Qt::TabFocusReason);
                std::cout << "ACCESSIBILITY_FOCUS=" << license->hasActiveFocus() << '\n';
                okay = okay && license->hasActiveFocus();
                QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
                QCoreApplication::sendEvent(license, &press);
                QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
                QCoreApplication::sendEvent(license, &release);
                QCoreApplication::processEvents();
                std::cout << "ACCESSIBILITY_DIALOG_VISIBLE=" << dialog->property("visible").toBool() << '\n';
                okay = okay && dialog->property("visible").toBool();
                if (!screenshotPath.isEmpty()) okay = capture() && okay;
                auto* website = window->findChild<QObject*>(QStringLiteral("websiteDialog"));
                const bool linkInvoked = QMetaObject::invokeMethod(window, "openWebsite",
                    Q_ARG(QVariant, QVariant(QStringLiteral("https://bet.hkjc.com/ch/marksix/home"))));
                QCoreApplication::processEvents();
                std::cout << "LINK_POPUP=" << (linkInvoked && website && website->property("visible").toBool()) << '\n';
                okay = okay && linkInvoked && website && website->property("visible").toBool();
                window->setProperty("reducedMotion", true);
                okay = okay && window->property("reducedMotion").toBool();
            }
            std::cout << "ACCESSIBILITY_AUDIT=" << (okay ? "PASS" : "FAIL") << '\n';
            QCoreApplication::exit(okay ? 0 : 11);
        });
    }
    if (arguments.contains(QStringLiteral("--renderer-audit"))) {
        QTimer::singleShot(400, &app, [&] {
            const auto image = window->grabWindow();
            const auto api = window->rendererInterface()->graphicsApi();
            std::cout << "RENDERER_API=" << int(api) << " FRAME=" << image.width() << 'x' << image.height() << '\n';
            QCoreApplication::exit(image.isNull() ? 13 : 0);
        });
    }
    if (arguments.contains(QStringLiteral("--resize-audit"))) {
        const auto sizes = std::make_shared<const std::array<QSize, 6>>(std::array<QSize, 6>{
            QSize(800, 600), QSize(1280, 800), QSize(960, 640),
            QSize(800, 600), QSize(1280, 800), QSize(800, 600)});
        const auto step = std::make_shared<int>(0);
        const auto first = std::make_shared<QImage>();
        const auto next = std::make_shared<std::function<void()>>();
        *next = [&app, window, sizes, step, first, next] {
            if (*step == int(sizes->size()) * 5) {
                std::cout << "RESIZE_AUDIT=PASS captures=" << *step << "\n";
                QCoreApplication::exit(0);
                return;
            }
            const int sizeIndex = *step / 5, pageIndex = *step % 5;
            window->resize((*sizes)[size_t(sizeIndex)]);
            window->setProperty("pageIndex", pageIndex);
            QTimer::singleShot(100, &app, [&app, window, sizes, step, first, next] {
                const QImage image = window->grabWindow();
                auto* sidebar = window->findChild<QQuickItem*>(QStringLiteral("sidebar"));
                auto* license = window->findChild<QQuickItem*>(QStringLiteral("licenseButton"));
                auto* content = window->findChild<QQuickItem*>(QStringLiteral("content"));
                const bool geometry = sidebar && license && content &&
                    license->mapToScene(QPointF(license->width(), 0)).x() <= window->width() &&
                    content->width() > 400 && sidebar->y() == 0 && content->y() == 0;
                if (image.isNull() || !geometry) {
                    std::cerr << "RESIZE_AUDIT=FAIL step=" << *step << "\n";
                    QCoreApplication::exit(12); return;
                }
                if (*step == 0) *first = image;
                if (*step == int(sizes->size()) * 5 - 5 && *first != image) {
                    size_t changed = 0;
                    if (first->size() == image.size())
                        for (int y = 0; y < image.height(); ++y)
                            for (int x = 0; x < image.width(); ++x)
                                if (first->pixel(x, y) != image.pixel(x, y)) ++changed;
                    first->save(QDir::tempPath() + QStringLiteral("/marksix-resize-first.png"));
                    image.save(QDir::tempPath() + QStringLiteral("/marksix-resize-return.png"));
                    std::cout << "RESIZE_RETURN_CHANGED_PIXELS=" << changed << '\n';
                    if (first->size() != image.size() ||
                        changed > size_t(image.width() * image.height()) / 1000) {
                        std::cerr << "RESIZE_AUDIT=FAIL return paint differs\n";
                        QCoreApplication::exit(12); return;
                    }
                }
                ++*step;
                (*next)();
            });
        };
        QTimer::singleShot(100, &app, [next] { (*next)(); });
    }
    if (arguments.contains(QStringLiteral("--performance-audit"))) {
        auto* navigation = new QTimer(&app);
        navigation->setInterval(30);
        auto* check = new QTimer(&app);
        check->setInterval(50);
        auto ticks = std::make_shared<int>(0);
        auto frames = std::make_shared<int>(0);
        auto maxGap = std::make_shared<qint64>(0);
        auto lastTick = std::make_shared<qint64>(launchClock.elapsed());
        QObject::connect(window, &QQuickWindow::frameSwapped, &app, [frames] { ++*frames; });
        QObject::connect(navigation, &QTimer::timeout, &app, [=, &launchClock] {
            window->setProperty("pageIndex", *ticks % 5);
            ++*ticks;
            const auto now = launchClock.elapsed();
            *maxGap = std::max(*maxGap, now - *lastTick);
            *lastTick = now;
        });
        navigation->start();
        QObject::connect(check, &QTimer::timeout, &app, [=, &app, &loader, &launchClock] {
            if (loader.busy()) return;
            check->stop(); navigation->stop();
            window->setProperty("pageIndex", 0);
            const auto readyMs = launchClock.elapsed();
            QTimer::singleShot(2000, &app, [=, &app, &loader] {
                const auto idleCpu = processCpuMs();
                const auto idleFrames = *frames;
                QTimer::singleShot(5000, &app, [=, &loader] {
                    const auto cpuDelta = processCpuMs() - idleCpu;
                    const auto frameDelta = *frames - idleFrames;
                    std::cout << "PERFORMANCE frontend_startup_ms=" << frontendStartupMs
                              << " local_ready_ms=" << readyMs << " navigation_ticks=" << *ticks
                              << " max_tick_gap_ms=" << *maxGap << " idle_cpu_ms_per_5s=" << cpuDelta
                              << " idle_frames_per_5s=" << frameDelta
                              << " peak_working_set_mib=" << peakWorkingSet() / 1048576.0 << '\n';
                    QCoreApplication::exit(loader.error().isEmpty() && *ticks > 5 && *maxGap < 500 ? 0 : 14);
                });
            });
        });
        check->start();
        QTimer::singleShot(120000, &app, [] { QCoreApplication::exit(14); });
    }
    if (arguments.contains(QStringLiteral("--smoke-load"))) {
        auto* check = new QTimer(&app);
        int learningAuditStage = 0;
        int analysisAuditStage = 0;
        int unfilteredArchiveRows = 0;
        QObject::connect(check, &QTimer::timeout, &app, [&] {
            if (loader.busy()) return;
            if (!loader.error().isEmpty()) {
                std::cerr << "STATISTICS_LOAD_ERROR=" << loader.error().toStdString() << '\n';
                QCoreApplication::exit(3);
            } else {
                if (arguments.contains(QStringLiteral("--date-picker-audit"))) {
                    auto* from = window->findChild<QQuickItem*>(QStringLiteral("fromDatePicker"));
                    auto* through = window->findChild<QQuickItem*>(QStringLiteral("throughDatePicker"));
                    const auto first = loader.eraFirstDates().value(0);
                    const auto last = loader.eraLastDates().value(0);
                    bool okay = from && through && !first.isEmpty() && !last.isEmpty();
                    if (okay) {
                        okay = QMetaObject::invokeMethod(from, "chooseIso",
                            Q_ARG(QVariant, QVariant(first)));
                        okay = okay && from->property("selectedDate").toString() == first;
                        QMetaObject::invokeMethod(through, "chooseIso",
                            Q_ARG(QVariant, QVariant(QStringLiteral("1900-01-01"))));
                        okay = okay && through->property("selectedDate").toString().isEmpty();
                        QMetaObject::invokeMethod(through, "chooseIso",
                            Q_ARG(QVariant, QVariant(last)));
                        okay = okay && through->property("selectedDate").toString() == last;
                        QMetaObject::invokeMethod(from, "chooseIso",
                            Q_ARG(QVariant, QVariant(QStringLiteral("2999-01-01"))));
                        okay = okay && from->property("selectedDate").toString() == first;
                        auto* calendar = from->findChild<QObject*>(QStringLiteral("dateCalendar"));
                        okay = okay && calendar && QMetaObject::invokeMethod(calendar, "open");
                        QCoreApplication::processEvents();
                        okay = okay && calendar->property("visible").toBool();
                    }
                    std::cout << "DATE_PICKER_AUDIT=" << (okay ? "PASS" : "FAIL") << '\n';
                    if (!okay) {
                        QCoreApplication::exit(17);
                        check->stop();
                        return;
                    }
                }
                if (arguments.contains(QStringLiteral("--analysis-audit"))) {
                    if (analysisAuditStage == 0) {
                        if (models.archive()->rowCount() == 0 || models.analysis()->rowCount() != 49 ||
                            models.pairs()->rowCount() != 1176) {
                            std::cerr << "ANALYSIS_AUDIT=default mismatch\n";
                            QCoreApplication::exit(10); check->stop(); return;
                        }
                        unfilteredArchiveRows = models.archive()->rowCount();
                        loader.setAnalysisSettings(1, 0, {}, {}, 1, 1);
                        analysisAuditStage = 1;
                        return;
                    }
                    if (analysisAuditStage == 1) {
                        const double fairExtra = models.analysis()->data(models.analysis()->index(0, 4),
                            StatisticsTableModel::NumericRole).toDouble();
                        if (models.pairs()->rowCount() != 0 || models.archive()->rowCount() == 0 ||
                            std::abs(fairExtra - 1.0 / 49.0) > 1e-12) {
                            std::cerr << "ANALYSIS_AUDIT=extra/era mismatch\n";
                            QCoreApplication::exit(10); check->stop(); return;
                        }
                        loader.setAnalysisSettings(0, 50, {}, {}, 1, 0);
                        analysisAuditStage = 2;
                        return;
                    }
                    if (analysisAuditStage == 2) {
                        int total = 0;
                        for (int row = 0; row < 49; ++row)
                            total += models.analysis()->data(models.analysis()->index(row, 1),
                                StatisticsTableModel::NumericRole).toInt();
                        if (total != 300 || models.rolling()->rowCount() != 50 ||
                            models.pairs()->rowCount() != 1176 ||
                            loader.setAnalysisSettings(0, 0, QStringLiteral("bad-date"), {}, 1, 0)) {
                            std::cerr << "ANALYSIS_AUDIT=window/validation mismatch\n";
                            QCoreApplication::exit(10); check->stop(); return;
                        }
                        const auto firstDate = loader.eraFirstDates().value(0);
                        if (firstDate.isEmpty() ||
                            !loader.setAnalysisSettings(0, 0, firstDate, firstDate, 1, 0)) {
                            std::cerr << "ANALYSIS_AUDIT=date selection failed\n";
                            QCoreApplication::exit(10);
                            check->stop();
                            return;
                        }
                        analysisAuditStage = 3;
                        return;
                    }
                    if (analysisAuditStage == 3) {
                        const auto firstDate = loader.eraFirstDates().value(0);
                        const bool filtered = models.archive()->rowCount() == 1 &&
                            models.archive()->data(models.archive()->index(0, 1),
                                StatisticsTableModel::DisplayRole).toString() == firstDate;
                        if (!filtered) {
                            std::cerr << "ANALYSIS_AUDIT=history date range mismatch\n";
                            QCoreApplication::exit(10);
                            check->stop();
                            return;
                        }
                        auto* drawPage = window->findChild<QQuickItem*>(
                            QStringLiteral("drawHistoryPage"));
                        auto* fromPicker = window->findChild<QQuickItem*>(
                            QStringLiteral("fromDatePicker"));
                        auto* throughPicker = window->findChild<QQuickItem*>(
                            QStringLiteral("throughDatePicker"));
                        auto* clearButton = window->findChild<QQuickItem*>(
                            QStringLiteral("clearDateFiltersButton"));
                        const bool controlsReady = drawPage && fromPicker && throughPicker &&
                            clearButton &&
                            QMetaObject::invokeMethod(fromPicker, "chooseIso",
                                Q_ARG(QVariant, QVariant(firstDate))) &&
                            QMetaObject::invokeMethod(throughPicker, "chooseIso",
                                Q_ARG(QVariant, QVariant(firstDate))) &&
                            clearButton->property("enabled").toBool() &&
                            QMetaObject::invokeMethod(drawPage, "clearDateFilters");
                        if (!controlsReady) {
                            std::cerr << "ANALYSIS_AUDIT=clear controls unavailable\n";
                            QCoreApplication::exit(10);
                            check->stop();
                            return;
                        }
                        analysisAuditStage = 4;
                        return;
                    }
                    if (analysisAuditStage == 4) {
                        auto* fromPicker = window->findChild<QQuickItem*>(
                            QStringLiteral("fromDatePicker"));
                        auto* throughPicker = window->findChild<QQuickItem*>(
                            QStringLiteral("throughDatePicker"));
                        const bool cleared = models.archive()->rowCount() == unfilteredArchiveRows &&
                            fromPicker && throughPicker &&
                            fromPicker->property("selectedDate").toString().isEmpty() &&
                            throughPicker->property("selectedDate").toString().isEmpty();
                        if (!cleared) {
                            std::cerr << "ANALYSIS_AUDIT=clear did not reapply unfiltered range\n";
                            QCoreApplication::exit(10);
                            check->stop();
                            return;
                        }
                        std::cout << "ANALYSIS_AUDIT=PASS clear_reapplied=" << unfilteredArchiveRows
                                  << " history rows\n";
                    }
                }
                if (arguments.contains(QStringLiteral("--learning-audit"))) {
                    const int count = loader.learningCount();
                    if (count <= 100 || models.learning()->rowCount() != 100) {
                        std::cerr << "LEARNING_AUDIT=missing fixture or page\n";
                        QCoreApplication::exit(9); check->stop(); return;
                    }
                    if (learningAuditStage == 0) {
                        loader.pageLearning(3);
                        learningAuditStage = 1;
                        return;
                    }
                    if (learningAuditStage == 1) {
                        const auto first = models.learning()->data(models.learning()->index(0, 0),
                            StatisticsTableModel::NumericRole).toInt();
                        if (loader.learningOffset() != count - 100 || first != count - 99) {
                            std::cerr << "LEARNING_AUDIT=last-page mismatch\n";
                            QCoreApplication::exit(9); check->stop(); return;
                        }
                        loader.setProgressView(0, 1);
                        learningAuditStage = 2;
                        return;
                    }
                    if (learningAuditStage == 2) {
                        const auto values = models.progress()->chartValues();
                        const auto zero = models.progress()->chartZero();
                        if (values.isEmpty() || !zero || models.progress()->rowCount() == 0) {
                            std::cerr << "LEARNING_AUDIT=progress missing\n";
                            QCoreApplication::exit(9); check->stop(); return;
                        }
                        const auto negatives = std::count_if(values.begin(), values.end(),
                            [](const QVariant& value) { return value.toDouble() < 0; });
                        std::cout << "LEARNING_AUDIT replay_points=" << values.size()
                                  << " negative_points=" << negatives << '\n';
                        loader.setProgressView(1, 1);
                        learningAuditStage = 3;
                        return;
                    }
                    if (learningAuditStage == 3) {
                        const bool separate = models.progress()->summary().contains(
                            QStringLiteral("Saved before local fetch"));
                        if (!separate) {
                            std::cerr << "LEARNING_AUDIT=evidence groups mixed\n";
                            QCoreApplication::exit(9); check->stop(); return;
                        }
                        std::cout << "LEARNING_AUDIT=PASS last_offset=" << loader.learningOffset()
                                  << " prefetch_rows=" << models.progress()->rowCount() << '\n';
                    }
                }
                if (arguments.contains(QStringLiteral("--model-audit"))) {
                    const bool okay = models.labProbabilities()->rowCount() == 49 &&
                        models.adaptive()->rowCount() == 49 &&
                        models.fixed()->rowCount() == 49 &&
                        models.exact()->rowCount() == 49 &&
                        models.calibration()->rowCount() == 60 && loader.controlEligible() &&
                        loader.adaptiveExtra() >= 1 && loader.fixedExtra() >= 1 &&
                        !loader.adaptiveMain().contains(loader.adaptiveExtra()) &&
                        !loader.fixedMain().contains(loader.fixedExtra()) &&
                        std::abs(models.labProbabilities()->data(
                            models.labProbabilities()->index(0, 2), StatisticsTableModel::NumericRole).toDouble()
                            - 6.0 / 49.0) < 1e-12;
                    std::cout << "MODEL_AUDIT=" << (okay ? "PASS" : "FAIL") << '\n';
                    if (!okay) { QCoreApplication::exit(8); check->stop(); return; }
                }
                std::cout << "ARCHIVE_ROWS=" << models.archive()->rowCount()
                          << " ANALYSIS_ROWS=" << models.analysis()->rowCount()
                          << " LAB_ROWS=" << models.forecastLab()->rowCount()
                          << " LEARNING_ROWS=" << models.learning()->rowCount() << '\n';
                QTimer::singleShot(120, &app, [&] { QCoreApplication::exit(capture() ? 0 : 5); });
                check->stop();
            }
        });
        check->start(50);
        QTimer::singleShot(180000, &app, [] { QCoreApplication::exit(4); });
    }
    return app.exec();
}
