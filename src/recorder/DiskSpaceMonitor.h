#pragma once

#include "core/AppError.h"
#include "core/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace creator::recorder {

struct DiskSpaceValues final {
    std::uint64_t capacityBytes{0};
    std::uint64_t freeBytes{0};
    std::uint64_t availableBytes{0};
};

struct DiskSpaceSnapshot final {
    std::uint64_t availableBytes{0};
    std::uint64_t reserveBytes{0};
    std::uint64_t nextSegmentBytes{0};
    std::uint64_t requiredBytes{0};
    bool sufficient{false};
};

class IDiskSpaceProbe {
public:
    virtual ~IDiskSpaceProbe() = default;
    IDiskSpaceProbe(const IDiskSpaceProbe&) = delete;
    IDiskSpaceProbe& operator=(const IDiskSpaceProbe&) = delete;
    [[nodiscard]] virtual core::Result<DiskSpaceValues> query(
        const std::filesystem::path& path) = 0;

protected:
    IDiskSpaceProbe() = default;
};

class FilesystemDiskSpaceProbe final : public IDiskSpaceProbe {
public:
    [[nodiscard]] core::Result<DiskSpaceValues> query(
        const std::filesystem::path& path) override;
};

class DiskSpaceMonitor final {
public:
    static constexpr std::uint64_t defaultReserveBytes = 1ULL << 30U;

    explicit DiskSpaceMonitor(
        std::unique_ptr<IDiskSpaceProbe> probe = std::make_unique<FilesystemDiskSpaceProbe>(),
        std::uint64_t reserveBytes = defaultReserveBytes);

    [[nodiscard]] core::Result<DiskSpaceSnapshot> check(
        const std::filesystem::path& path, std::uint64_t nextSegmentBytes);
    [[nodiscard]] const std::optional<DiskSpaceSnapshot>& lastSnapshot() const noexcept {
        return lastSnapshot_;
    }
    [[nodiscard]] const std::optional<core::AppError>& lastError() const noexcept {
        return lastError_;
    }

private:
    std::unique_ptr<IDiskSpaceProbe> probe_;
    std::uint64_t reserveBytes_;
    std::optional<DiskSpaceSnapshot> lastSnapshot_;
    std::optional<core::AppError> lastError_;
};

}  // namespace creator::recorder
