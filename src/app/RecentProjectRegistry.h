#pragma once

#include "core/Result.h"
#include "core/Utc.h"

#include <QString>

#include <filesystem>
#include <vector>

namespace creator::app {

struct RecentProject final {
    std::filesystem::path path;
    creator::core::Utc lastOpenedAt;
};

[[nodiscard]] std::filesystem::path pathFromQString(const QString& value);
[[nodiscard]] QString qStringFromPath(const std::filesystem::path& value);

class RecentProjectRegistry final {
public:
    explicit RecentProjectRegistry(std::filesystem::path filePath);
    [[nodiscard]] creator::core::Result<std::vector<RecentProject>> load() const;
    [[nodiscard]] creator::core::Result<void> remember(
        const std::filesystem::path& path, const creator::core::Utc& openedAt);
    [[nodiscard]] const std::filesystem::path& filePath() const noexcept { return filePath_; }

private:
    std::filesystem::path filePath_;
};

}  // namespace creator::app
