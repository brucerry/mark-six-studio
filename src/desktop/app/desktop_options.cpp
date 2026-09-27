#include "desktop_options.hpp"
#include "statistics_data_root.hpp"

std::optional<DesktopOptions> parseDesktopOptions(const QStringList& arguments) {
    DesktopOptions options;
    for (int index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == QStringLiteral("--data-dir")) {
            if (++index >= arguments.size() || arguments[index].isEmpty()) return std::nullopt;
            options.dataRoot = std::filesystem::path(arguments[index].toStdWString());
        } else if (argument == QStringLiteral("--screenshot")) {
            if (++index >= arguments.size() || arguments[index].isEmpty()) return std::nullopt;
            options.screenshotPath = arguments[index];
        } else if (argument == QStringLiteral("--size")) {
            if (++index >= arguments.size()) return std::nullopt;
            const auto parts = arguments[index].split('x');
            if (parts.size() != 2) return std::nullopt;
            bool widthOk = false, heightOk = false;
            const int width = parts[0].toInt(&widthOk);
            const int height = parts[1].toInt(&heightOk);
            if (!widthOk || !heightOk || width < 800 || height < 600) return std::nullopt;
            options.requestedSize = QSize(width, height);
        } else if (argument == QStringLiteral("--page")) {
            if (++index >= arguments.size()) return std::nullopt;
            bool valid = false;
            options.requestedPage = arguments[index].toInt(&valid);
            if (!valid || options.requestedPage < 0 || options.requestedPage > 4) return std::nullopt;
        } else if (argument == QStringLiteral("--comparison-tab")) {
            if (++index >= arguments.size()) return std::nullopt;
            bool valid = false;
            options.requestedComparison = arguments[index].toInt(&valid);
            if (!valid || options.requestedComparison < 0 || options.requestedComparison > 4) return std::nullopt;
        } else if (argument == QStringLiteral("--learning-progress")) {
            options.showLearningProgress = true;
        }
    }
    if (options.dataRoot.empty()) options.dataRoot = defaultStatisticsDataRoot();
    return options;
}
