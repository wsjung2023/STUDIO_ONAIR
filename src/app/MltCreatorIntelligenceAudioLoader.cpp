#include "app/MltCreatorIntelligenceAudioLoader.h"

#include "core/AppError.h"
#include "core/Timebase.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace creator::app {
namespace {

constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::int64_t kMaximumAnalysisDurationNs =
    2LL * 60LL * 60LL * kNanosecondsPerSecond;

std::int64_t timelineDurationNs(const domain::Timeline& timeline) {
    std::int64_t duration = 0;
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            duration = std::max(
                duration,
                clip.timelineRange().end().time_since_epoch().count());
        }
    }
    return duration;
}

std::uint64_t samplesAtFrame(std::uint64_t frame, std::uint32_t frequency,
                             core::FrameRate rate) {
    const auto numerator = static_cast<std::uint64_t>(rate.numerator());
    const auto denominator = static_cast<std::uint64_t>(rate.denominator());
    const auto samplesPerNumerator = denominator * frequency;
    return (frame / numerator) * samplesPerNumerator +
           ((frame % numerator) * samplesPerNumerator) / numerator;
}

}  // namespace

CreatorIntelligenceController::AudioLoader
makeMltCreatorIntelligenceAudioLoader(
    std::filesystem::path runtimeRoot,
    mlt_adapter::MltEditEngineConfig::AudioProcessorFactory audioFactory) {
    return [runtimeRoot = std::move(runtimeRoot),
            audioFactory = std::move(audioFactory)](
               const edit_engine::TimelineSnapshot& snapshot,
               std::stop_token stop) -> core::Result<std::vector<float>> {
        const auto durationNs = timelineDurationNs(snapshot.timeline);
        if (durationNs <= 0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "project timeline has no audio duration"};
        }
        if (durationNs > kMaximumAnalysisDurationNs) {
            return core::AppError{
                core::ErrorCode::InvalidArgument,
                "local AI analysis is limited to two hours per project"};
        }
        const auto rate = snapshot.timeline.frameRate();
        const auto durationFrames =
            core::timestampToFrame(
                core::TimestampNs{core::DurationNs{durationNs - 1}}, rate) +
            1;
        const auto wholeSeconds = durationNs / kNanosecondsPerSecond;
        const auto remainderNs = durationNs % kNanosecondsPerSecond;
        const auto total48kSamples =
            static_cast<std::uint64_t>(wholeSeconds) * 48'000U +
            static_cast<std::uint64_t>(remainderNs) * 48'000U /
                static_cast<std::uint64_t>(kNanosecondsPerSecond);
        if (durationFrames <= 0 || total48kSamples < 3U) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "project audio is too short to analyze"};
        }

        mlt_adapter::MltEditEngine engine{{
            .runtimeRoot = runtimeRoot,
            .previewWidth = 16,
            .previewHeight = 16,
            .audioProcessingFactory = audioFactory}};
        auto loaded = engine.load(snapshot);
        if (!loaded.hasValue()) return loaded.error();

        std::vector<float> mono16k;
        mono16k.reserve(static_cast<std::size_t>(total48kSamples / 3U));
        double downsampleAccumulator = 0.0;
        int downsamplePhase = 0;
        for (std::int64_t frame = 0; frame < durationFrames; ++frame) {
            if (stop.stop_requested()) {
                return core::AppError{core::ErrorCode::InvalidState,
                                      "local AI audio loading was cancelled"};
            }
            const auto first = samplesAtFrame(
                static_cast<std::uint64_t>(frame), 48'000, rate);
            if (first >= total48kSamples) break;
            const auto next = std::min(
                samplesAtFrame(static_cast<std::uint64_t>(frame + 1), 48'000,
                               rate),
                total48kSamples);
            if (next <= first ||
                next - first >
                    static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                return core::AppError{
                    core::ErrorCode::InvalidState,
                    "local AI audio sample range is invalid"};
            }
            const auto requested = static_cast<int>(next - first);
            auto block = engine.requestMixedAudio(
                core::frameToTimestamp(frame, rate), 48'000, 2, requested);
            if (!block.hasValue()) return block.error();
            if (block.value().size() !=
                static_cast<std::size_t>(requested) * 2U) {
                return core::AppError{
                    core::ErrorCode::InvalidState,
                    "local AI audio loader received an incomplete block"};
            }
            for (std::size_t index = 0; index < block.value().size();
                 index += 2U) {
                downsampleAccumulator +=
                    (static_cast<double>(block.value()[index]) +
                     static_cast<double>(block.value()[index + 1U])) *
                    0.5;
                ++downsamplePhase;
                if (downsamplePhase == 3) {
                    mono16k.push_back(static_cast<float>(
                        downsampleAccumulator / 3.0));
                    downsampleAccumulator = 0.0;
                    downsamplePhase = 0;
                }
            }
        }
        if (mono16k.empty()) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "local AI audio loader produced no samples"};
        }
        return mono16k;
    };
}

}  // namespace creator::app
