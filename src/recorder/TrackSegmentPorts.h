#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Segment.h"
#include "media/MediaTypes.h"
#include "recorder/RecordingTrack.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace creator::recorder {

struct MappedVideoFrame final {
    core::TimestampNs timestamp{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::size_t rowBytes{0};
    const std::uint8_t* data{nullptr};
    std::shared_ptr<void> owner;
};

class IVideoFrameMapper {
public:
    virtual ~IVideoFrameMapper() = default;
    IVideoFrameMapper(const IVideoFrameMapper&) = delete;
    IVideoFrameMapper& operator=(const IVideoFrameMapper&) = delete;
    [[nodiscard]] virtual core::Result<MappedVideoFrame> map(
        const media::VideoFrame& frame) = 0;

protected:
    IVideoFrameMapper() = default;
};

struct SegmentEncodeConfig final {
    RecordingTrack track;
    std::filesystem::path partPath;
    core::TimestampNs startTime{};
    core::DurationNs targetDuration{};
};

struct EncodedSegment final {
    core::TimestampNs endTime{};
    std::uint64_t bytesWritten{0};
    std::string codecName;
};

class ITrackSegmentEncoder {
public:
    virtual ~ITrackSegmentEncoder() = default;
    ITrackSegmentEncoder(const ITrackSegmentEncoder&) = delete;
    ITrackSegmentEncoder& operator=(const ITrackSegmentEncoder&) = delete;
    [[nodiscard]] virtual core::Result<void> start(const SegmentEncodeConfig& config) = 0;
    [[nodiscard]] virtual core::Result<void> accept(const media::VideoFrame& frame) = 0;
    [[nodiscard]] virtual core::Result<void> accept(const media::AudioBlock& block) = 0;
    [[nodiscard]] virtual core::Result<EncodedSegment> finish(core::TimestampNs endTime) = 0;
    virtual void abort() noexcept = 0;
    [[nodiscard]] virtual SegmentContainer container() const noexcept {
        return SegmentContainer::Matroska;
    }

protected:
    ITrackSegmentEncoder() = default;
};

class ISegmentLifecycleSink {
public:
    virtual ~ISegmentLifecycleSink() = default;
    ISegmentLifecycleSink(const ISegmentLifecycleSink&) = delete;
    ISegmentLifecycleSink& operator=(const ISegmentLifecycleSink&) = delete;
    [[nodiscard]] virtual core::Result<void> begin(const domain::SegmentInfo& segment) = 0;
    [[nodiscard]] virtual core::Result<void> ready(const domain::SegmentInfo& segment) = 0;
    [[nodiscard]] virtual core::Result<void> failed(const domain::SourceId& sourceId,
                                                    std::uint64_t segmentIndex) = 0;

protected:
    ISegmentLifecycleSink() = default;
};

}  // namespace creator::recorder
