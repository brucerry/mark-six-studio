#pragma once

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QStringList>

class QQuickWindow;
class StatisticsLoader;
class StatisticsModels;

int runDesktopDiagnostics(QGuiApplication& app, QQuickWindow* window, StatisticsLoader& loader,
                          StatisticsModels& models, const QStringList& arguments,
                          const QString& screenshotPath, QElapsedTimer& launchClock,
                          qint64 frontendStartupMs);
