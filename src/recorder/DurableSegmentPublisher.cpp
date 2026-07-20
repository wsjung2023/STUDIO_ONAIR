#include "recorder/DurableSegmentPublisher.h"

#include "core/AppError.h"

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <stdio.h>
#endif
#endif

namespace creator::recorder {
namespace {

core::AppError fileError(const char* operation, std::uint64_t code) {
    return {core::ErrorCode::IoFailure,
            std::string{"Segment file "} + operation + " failed (code " +
                std::to_string(code) + ")"};
}

bool pathStartsWith(const std::filesystem::path& path,
                    const std::filesystem::path& parent) {
    auto pathPart = path.begin();
    for (auto parentPart = parent.begin(); parentPart != parent.end();
         ++parentPart, ++pathPart) {
        if (pathPart == path.end() || *pathPart != *parentPart) return false;
    }
    return true;
}

class NativeSegmentFileOperations final : public ISegmentFileOperations {
public:
    explicit NativeSegmentFileOperations(std::filesystem::path packageRoot)
        : packageRoot_(std::move(packageRoot)) {}

    core::Result<void> prepare(const std::filesystem::path& partPath,
                               const std::filesystem::path& finalPath) override {
        std::error_code error;
        std::filesystem::create_directories(packageRoot_, error);
        if (error) return fileError("create package directory", error.value());
        std::filesystem::create_directories(partPath.parent_path(), error);
        if (error) return fileError("create temporary directory", error.value());
        std::filesystem::create_directories(finalPath.parent_path(), error);
        if (error) return fileError("create media directory", error.value());

        const auto canonicalRoot = std::filesystem::weakly_canonical(packageRoot_, error);
        if (error) return fileError("resolve package directory", error.value());
        const auto canonicalPartParent =
            std::filesystem::weakly_canonical(partPath.parent_path(), error);
        if (error) return fileError("resolve temporary directory", error.value());
        const auto canonicalFinalParent =
            std::filesystem::weakly_canonical(finalPath.parent_path(), error);
        if (error) return fileError("resolve media directory", error.value());
        if (!pathStartsWith(canonicalPartParent, canonicalRoot) ||
            !pathStartsWith(canonicalFinalParent, canonicalRoot)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "Segment path resolves outside the project package"};
        }

        const auto finalStatus = std::filesystem::symlink_status(finalPath, error);
        const bool finalMissing = error == std::errc::no_such_file_or_directory;
        if (finalMissing) error.clear();
        else if (error) return fileError("inspect final path", error.value());
        if (!finalMissing && finalStatus.type() != std::filesystem::file_type::not_found) {
            return core::AppError{core::ErrorCode::AlreadyExists,
                                  "Segment final path already exists"};
        }
        const auto partStatus = std::filesystem::symlink_status(partPath, error);
        const bool partMissing = error == std::errc::no_such_file_or_directory;
        if (partMissing) error.clear();
        else if (error) return fileError("inspect temporary path", error.value());
        if (!partMissing && partStatus.type() != std::filesystem::file_type::not_found) {
            return core::AppError{core::ErrorCode::AlreadyExists,
                                  "Segment temporary path already exists"};
        }
        return core::ok();
    }

    core::Result<void> publish(const std::filesystem::path& partPath,
                               const std::filesystem::path& finalPath) override {
        lastPartPath_ = partPath;
        lastFinalPath_ = finalPath;
        lastAttemptPublished_ = false;
#ifdef _WIN32
        HANDLE handle = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return fileError("open temporary", GetLastError());
        }
        if (!FlushFileBuffers(handle)) {
            const DWORD code = GetLastError();
            CloseHandle(handle);
            return fileError("flush temporary", code);
        }
        if (!CloseHandle(handle)) return fileError("close temporary", GetLastError());
        if (!MoveFileExW(partPath.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return fileError("publish without overwrite", GetLastError());
        }
        lastAttemptPublished_ = true;
#else
        const int descriptor = ::open(partPath.c_str(), O_RDONLY);
        if (descriptor < 0) {
            return fileError("open temporary", static_cast<std::uint64_t>(errno));
        }
        if (::fsync(descriptor) != 0) {
            const int code = errno;
            ::close(descriptor);
            return fileError("flush temporary", static_cast<std::uint64_t>(code));
        }
        if (::close(descriptor) != 0) {
            return fileError("close temporary", static_cast<std::uint64_t>(errno));
        }
#ifdef __APPLE__
        if (::renamex_np(partPath.c_str(), finalPath.c_str(), RENAME_EXCL) != 0) {
            return fileError("publish without overwrite", static_cast<std::uint64_t>(errno));
        }
        lastAttemptPublished_ = true;
#else
        if (::link(partPath.c_str(), finalPath.c_str()) != 0) {
            return fileError("publish without overwrite", static_cast<std::uint64_t>(errno));
        }
        lastAttemptPublished_ = true;
        if (::unlink(partPath.c_str()) != 0) {
            return fileError("remove published temporary", static_cast<std::uint64_t>(errno));
        }
#endif
        const int parent = ::open(finalPath.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (parent < 0) {
            return fileError("open media directory", static_cast<std::uint64_t>(errno));
        }
        if (::fsync(parent) != 0) {
            const int code = errno;
            ::close(parent);
            return fileError("flush media directory", static_cast<std::uint64_t>(code));
        }
        if (::close(parent) != 0) {
            return fileError("close media directory", static_cast<std::uint64_t>(errno));
        }
#endif
        return core::ok();
    }

    bool didPublishLastAttempt(const std::filesystem::path& partPath,
                               const std::filesystem::path& finalPath) const noexcept override {
        return lastAttemptPublished_ && partPath == lastPartPath_ &&
               finalPath == lastFinalPath_;
    }

private:
    std::filesystem::path packageRoot_;
    std::filesystem::path lastPartPath_;
    std::filesystem::path lastFinalPath_;
    bool lastAttemptPublished_{false};
};

}  // namespace

std::unique_ptr<ISegmentFileOperations> makeSegmentFileOperations(
    std::filesystem::path packageRoot) {
    return std::make_unique<NativeSegmentFileOperations>(std::move(packageRoot));
}

DurableSegmentPublisher::DurableSegmentPublisher(
    std::filesystem::path packageRoot,
    std::unique_ptr<ISegmentFileOperations> fileOperations,
    std::unique_ptr<ISegmentLifecycleSink> lifecycleSink)
    : DurableSegmentPublisher(std::move(packageRoot), std::move(fileOperations),
                              std::move(lifecycleSink), {}) {}

DurableSegmentPublisher::DurableSegmentPublisher(
    std::filesystem::path packageRoot,
    std::unique_ptr<ISegmentFileOperations> fileOperations,
    std::unique_ptr<ISegmentLifecycleSink> lifecycleSink,
    std::string segmentNamespace)
    : packageRoot_(std::move(packageRoot)),
      fileOperations_(std::move(fileOperations)),
      lifecycleSink_(std::move(lifecycleSink)),
      segmentNamespace_(std::move(segmentNamespace)) {}

core::Result<SegmentOutputPaths> DurableSegmentPublisher::begin(
    const RecordingTrack& track, std::uint64_t index,
    core::TimestampNs startTime) {
    return begin(track, index, startTime, SegmentContainer::Matroska);
}

core::Result<SegmentOutputPaths> DurableSegmentPublisher::begin(
    const RecordingTrack& track, std::uint64_t index,
    core::TimestampNs startTime, SegmentContainer container) {
    if (pending_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "A segment is already pending publication"};
    }
    if (!fileOperations_ || !lifecycleSink_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Segment publisher is not fully configured"};
    }

    auto relativeFinal = relativeSegmentPath(track, index, container);
    if (!segmentNamespace_.empty()) {
        relativeFinal = relativeFinal.parent_path() / segmentNamespace_ /
                        relativeFinal.filename();
    }
    auto relativePart = std::filesystem::path{".tmp"} / relativeFinal;
    relativePart += ".part";
    SegmentOutputPaths paths{
        .partPath = packageRoot_ / relativePart,
        .finalPath = packageRoot_ / relativeFinal,
        .relativeFinalPath = relativeFinal,
    };
    if (auto prepared = fileOperations_->prepare(paths.partPath, paths.finalPath);
        !prepared.hasValue()) {
        return prepared.error();
    }

    domain::SegmentInfo writing{
        .index = index,
        .sourceId = track.sourceId(),
        .startTime = startTime,
        .duration = core::DurationNs::zero(),
        .status = domain::SegmentStatus::Writing,
        .relativePath = paths.relativeFinalPath.generic_string(),
    };
    if (auto began = lifecycleSink_->begin(writing); !began.hasValue()) {
        return began.error();
    }
    pending_ = PendingSegment{track, index, startTime, paths};
    return paths;
}

core::Result<domain::SegmentInfo> DurableSegmentPublisher::publish(
    const EncodedSegment& encoded) {
    if (!pending_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "No segment is pending publication"};
    }
    if (encoded.endTime < pending_->startTime || encoded.bytesWritten == 0) {
        auto invalid = core::AppError{core::ErrorCode::InvalidArgument,
                                      "Encoded segment has invalid final metadata"};
        const auto failed = fail();
        if (!failed.hasValue()) {
            return core::AppError{failed.error().code(),
                                  invalid.message() + "; " + failed.error().message()};
        }
        return invalid;
    }

    if (auto published = fileOperations_->publish(pending_->paths.partPath,
                                                   pending_->paths.finalPath);
        !published.hasValue()) {
        const core::AppError original = published.error();
        if (fileOperations_->didPublishLastAttempt(pending_->paths.partPath,
                                                   pending_->paths.finalPath)) {
            pending_.reset();
            return original;
        }
        const auto failed = fail();
        if (!failed.hasValue()) {
            return core::AppError{failed.error().code(),
                                  original.message() + "; " + failed.error().message()};
        }
        return original;
    }

    domain::SegmentInfo ready{
        .index = pending_->index,
        .sourceId = pending_->track.sourceId(),
        .startTime = pending_->startTime,
        .duration = encoded.endTime - pending_->startTime,
        .status = domain::SegmentStatus::Ready,
        .relativePath = pending_->paths.relativeFinalPath.generic_string(),
    };
    pending_.reset();
    if (auto stored = lifecycleSink_->ready(ready); !stored.hasValue()) {
        return stored.error();
    }
    return ready;
}

core::Result<void> DurableSegmentPublisher::fail() {
    if (!pending_) return core::ok();
    const auto sourceId = pending_->track.sourceId();
    const auto index = pending_->index;
    pending_.reset();
    return lifecycleSink_->failed(sourceId, index);
}

}  // namespace creator::recorder
