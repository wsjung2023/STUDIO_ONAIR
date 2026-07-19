#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Segment.h"
#include "recorder/RecordingTrack.h"
#include "recorder/TrackSegmentPorts.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace creator::recorder {

struct SegmentOutputPaths final {
    std::filesystem::path partPath;
    std::filesystem::path finalPath;
    std::filesystem::path relativeFinalPath;
};

class ISegmentFileOperations {
public:
    virtual ~ISegmentFileOperations() = default;
    ISegmentFileOperations(const ISegmentFileOperations&) = delete;
    ISegmentFileOperations& operator=(const ISegmentFileOperations&) = delete;
    [[nodiscard]] virtual core::Result<void> prepare(
        const std::filesystem::path& partPath,
        const std::filesystem::path& finalPath) = 0;
    [[nodiscard]] virtual core::Result<void> publish(
        const std::filesystem::path& partPath,
        const std::filesystem::path& finalPath) = 0;
    [[nodiscard]] virtual bool didPublishLastAttempt(
        const std::filesystem::path& partPath,
        const std::filesystem::path& finalPath) const noexcept = 0;

protected:
    ISegmentFileOperations() = default;
};

[[nodiscard]] std::unique_ptr<ISegmentFileOperations> makeSegmentFileOperations(
    std::filesystem::path packageRoot);

class DurableSegmentPublisher final {
public:
    DurableSegmentPublisher(std::filesystem::path packageRoot,
                            std::unique_ptr<ISegmentFileOperations> fileOperations,
                            std::unique_ptr<ISegmentLifecycleSink> lifecycleSink);

    [[nodiscard]] core::Result<SegmentOutputPaths> begin(
        const RecordingTrack& track, std::uint64_t index,
        core::TimestampNs startTime);
    [[nodiscard]] core::Result<SegmentOutputPaths> begin(
        const RecordingTrack& track, std::uint64_t index,
        core::TimestampNs startTime, SegmentContainer container);
    [[nodiscard]] core::Result<domain::SegmentInfo> publish(
        const EncodedSegment& encoded);
    [[nodiscard]] core::Result<void> fail();
    [[nodiscard]] bool hasPendingSegment() const noexcept { return pending_.has_value(); }

private:
    struct PendingSegment final {
        RecordingTrack track;
        std::uint64_t index{0};
        core::TimestampNs startTime{};
        SegmentOutputPaths paths;
    };

    std::filesystem::path packageRoot_;
    std::unique_ptr<ISegmentFileOperations> fileOperations_;
    std::unique_ptr<ISegmentLifecycleSink> lifecycleSink_;
    std::optional<PendingSegment> pending_;
};

}  // namespace creator::recorder
