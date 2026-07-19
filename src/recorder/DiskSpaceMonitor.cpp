#include "recorder/DiskSpaceMonitor.h"

#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace creator::recorder {

core::Result<DiskSpaceValues> FilesystemDiskSpaceProbe::query(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto values = std::filesystem::space(path, error);
    if (error) {
        return core::AppError{core::ErrorCode::IoFailure,
                              "Could not query recording disk space: " + error.message()};
    }
    return DiskSpaceValues{
        .capacityBytes = static_cast<std::uint64_t>(values.capacity),
        .freeBytes = static_cast<std::uint64_t>(values.free),
        .availableBytes = static_cast<std::uint64_t>(values.available),
    };
}

DiskSpaceMonitor::DiskSpaceMonitor(std::unique_ptr<IDiskSpaceProbe> probe,
                                   std::uint64_t reserveBytes)
    : probe_(std::move(probe)), reserveBytes_(reserveBytes) {}

core::Result<DiskSpaceSnapshot> DiskSpaceMonitor::check(
    const std::filesystem::path& path, std::uint64_t nextSegmentBytes) {
    if (!probe_) {
        lastError_ = core::AppError{core::ErrorCode::InvalidState,
                                    "Disk space monitor has no filesystem probe"};
        return *lastError_;
    }
    if (nextSegmentBytes > std::numeric_limits<std::uint64_t>::max() - reserveBytes_) {
        lastSnapshot_.reset();
        lastError_ = core::AppError{core::ErrorCode::InvalidArgument,
                                    "Recording disk reserve calculation overflowed"};
        return *lastError_;
    }

    auto values = probe_->query(path);
    if (!values.hasValue()) {
        lastSnapshot_.reset();
        lastError_ = values.error();
        return *lastError_;
    }

    const auto required = reserveBytes_ + nextSegmentBytes;
    lastSnapshot_ = DiskSpaceSnapshot{
        .availableBytes = values.value().availableBytes,
        .reserveBytes = reserveBytes_,
        .nextSegmentBytes = nextSegmentBytes,
        .requiredBytes = required,
        .sufficient = values.value().availableBytes >= required,
    };
    if (!lastSnapshot_->sufficient) {
        lastError_ = core::AppError{core::ErrorCode::InsufficientStorage,
                                    "Recording stopped because available disk space is below the reserve"};
        return *lastError_;
    }
    lastError_.reset();
    return *lastSnapshot_;
}

}  // namespace creator::recorder
