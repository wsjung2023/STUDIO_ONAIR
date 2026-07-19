#pragma once

#include "recorder/TrackSegmentPorts.h"

#include <cstdint>
#include <memory>

namespace creator::ffmpeg_adapter {

struct AudioEncoderOptions final {
    std::int64_t bitRate{192'000};
};

class FfmpegAudioSegmentEncoder final : public recorder::ITrackSegmentEncoder {
public:
    explicit FfmpegAudioSegmentEncoder(AudioEncoderOptions options = {});
    ~FfmpegAudioSegmentEncoder() override;

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
