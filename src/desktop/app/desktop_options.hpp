#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <optional>

struct DesktopOptions {
    std::filesystem::path dataRoot;
    QString screenshotPath;
    QSize requestedSize;
    int requestedPage = 0;
    int requestedComparison = 0;
    bool showLearningProgress = false;
};

std::optional<DesktopOptions> parseDesktopOptions(const QStringList& arguments);
