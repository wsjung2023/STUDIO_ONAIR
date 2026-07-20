#pragma once

#include "app/android/AndroidMediaCodecSession.h"
#include "recorder/TrackSegmentPorts.h"

#include <cstdint>
#include <filesystem>

namespace creator::app::android {

/// Synchronous Android MediaCodec/MediaMuxer adapter for one durable source
/// track. The recorder worker owns this object; Java owns only opaque codec
/// handles and never publishes final package paths itself.
class AndroidMediaCodecSegmentEncoder final
    : public recorder::ITrackSegmentEncoder {
public:
    explicit AndroidMediaCodecSegmentEncoder(recorder::TrackMediaKind mediaKind);
    ~AndroidMediaCodecSegmentEncoder() override;

    [[nodiscard]] core::Result<void> start(
        const recorder::SegmentEncodeConfig& config) override;
    [[nodiscard]] core::Result<void> accept(
        const media::VideoFrame& frame) override;
    [[nodiscard]] core::Result<void> accept(
        const media::AudioBlock& block) override;
    [[nodiscard]] core::Result<recorder::EncodedSegment> finish(
        core::TimestampNs endTime) override;
    void abort() noexcept override;
    [[nodiscard]] recorder::SegmentContainer container() const noexcept override {
        return recorder::SegmentContainer::Mp4;
    }

private:
    [[nodiscard]] core::Result<void> ensureVideoCodec(
        std::uint32_t width, std::uint32_t height);
    [[nodiscard]] core::Result<void> ensureAudioCodec(
        std::uint32_t sampleRate, std::uint32_t channels);
    [[nodiscard]] std::int64_t presentationTimeUs(
        core::TimestampNs timestamp) noexcept;

    recorder::TrackMediaKind mediaKind_;
    AndroidMediaCodecSession session_;
    std::uint64_t generation_{};
    std::int64_t javaHandle_{};
    std::int64_t lastPresentationTimeUs_{-1};
    std::filesystem::path partPath_;
    core::TimestampNs startTime_{};
};

[[nodiscard]] core::Result<void> probeAndroidMediaCodec();

}  // namespace creator::app::android
