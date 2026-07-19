#pragma once

#include "recorder/TrackSegmentPorts.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace creator::ffmpeg_adapter {

struct VideoEncoderOptions final {
    std::vector<std::string> preferredEncoderNames;
    std::int64_t frameRateNumerator{30};
    std::int64_t frameRateDenominator{1};
    std::int64_t bitRate{8'000'000};
};

class FfmpegVideoSegmentEncoder final : public recorder::ITrackSegmentEncoder {
public:
    explicit FfmpegVideoSegmentEncoder(
        std::unique_ptr<recorder::IVideoFrameMapper> mapper,
        VideoEncoderOptions options = {});
    ~FfmpegVideoSegmentEncoder() override;

    [[nodiscard]] core::Result<void> start(
        const recorder::SegmentEncodeConfig& config) override;
    [[nodiscard]] core::Result<void> accept(
        const media::VideoFrame& frame) override;
    [[nodiscard]] core::Result<void> accept(
        const media::AudioBlock& block) override;
    [[nodiscard]] core::Result<recorder::EncodedSegment> finish(
        core::TimestampNs endTime) override;
    void abort() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace creator::ffmpeg_adapter
