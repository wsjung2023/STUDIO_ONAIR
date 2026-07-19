#include "whisper_adapter/WhisperTranscriptionProvider.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/TimelineTypes.h"
#include "transcription/AudioInput.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptSegment.h"
#include "transcription/TranscriptWord.h"
#include "whisper_adapter/WhisperRuntimeManifest.h"

#include <whisper.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace creator::whisper_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr int kWhisperSampleRate = WHISPER_SAMPLE_RATE;  // 16 kHz, whisper's fixed input rate
constexpr std::int64_t kNanosPerCentisecond = 10'000'000;  // whisper t0/t1 are 10 ms units

// Interleaved-average downmix to mono followed by linear resampling to 16 kHz.
// Both steps are exact and deterministic; no wall clock, no RNG.
std::vector<float> resampleToWhisperMono(const transcription::AudioInput& audio) {
    const std::span<const float> samples = audio.samples();
    const auto channels = static_cast<std::size_t>(audio.channelCount());
    const std::int64_t frames = audio.frameCount();

    std::vector<float> mono(static_cast<std::size_t>(frames));
    for (std::int64_t frame = 0; frame < frames; ++frame) {
        double sum = 0.0;
        const auto base = static_cast<std::size_t>(frame) * channels;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            sum += static_cast<double>(samples[base + channel]);
        }
        mono[static_cast<std::size_t>(frame)] =
            static_cast<float>(sum / static_cast<double>(channels));
    }

    if (audio.sampleRateHz() == kWhisperSampleRate || frames <= 1) {
        return mono;
    }

    const double ratio =
        static_cast<double>(kWhisperSampleRate) / static_cast<double>(audio.sampleRateHz());
    const auto outCount =
        static_cast<std::size_t>(static_cast<double>(frames) * ratio);
    std::vector<float> resampled(outCount);
    for (std::size_t index = 0; index < outCount; ++index) {
        const double sourcePosition = static_cast<double>(index) / ratio;
        const auto left = static_cast<std::size_t>(sourcePosition);
        const std::size_t right = std::min(left + 1, mono.size() - 1);
        const double fraction = sourcePosition - static_cast<double>(left);
        resampled[index] = static_cast<float>(
            (1.0 - fraction) * mono[left] + fraction * mono[right]);
    }
    return resampled;
}

// whisper token text carries special markers (e.g. "[_BEG_]", "<|...|>") and
// leading spaces; only real word text becomes a TranscriptWord.
std::optional<std::string> normalizeTokenText(const char* raw) {
    if (raw == nullptr) return std::nullopt;
    std::string text{raw};
    std::size_t start = 0;
    while (start < text.size() && text[start] == ' ') ++start;
    text = text.substr(start);
    if (text.empty()) return std::nullopt;
    if (text.front() == '[' || text.front() == '<') return std::nullopt;
    return text;
}

std::optional<std::string> normalizeSegmentText(const char* raw) {
    if (raw == nullptr) return std::nullopt;
    std::string text{raw};
    std::size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return std::nullopt;
    std::size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

double clampConfidence(double value) {
    if (!std::isfinite(value)) return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

}  // namespace

// Owns the whisper_context and serializes access to it.
class WhisperTranscriptionProvider::Impl final {
public:
    Impl(whisper_context* context, int threadCount)
        : context_(context), threadCount_(threadCount) {}
    ~Impl() {
        if (context_ != nullptr) whisper_free(context_);
    }

    Result<transcription::Transcript> transcribe(
        const transcription::AudioInput& audio,
        const transcription::TranscriptionOptions& options) {
        if (audio.frameCount() == 0) {
            return AppError{ErrorCode::InvalidArgument, "cannot transcribe empty audio"};
        }
        if (!audio.hasOnlyFiniteSamples()) {
            return AppError{ErrorCode::InvalidArgument,
                            "cannot transcribe audio containing non-finite samples"};
        }

        std::string language =
            options.languageTag.empty() ? std::string{"en"} : options.languageTag;

        const std::vector<float> mono = resampleToWhisperMono(audio);
        if (mono.empty()) {
            return AppError{ErrorCode::InvalidArgument,
                            "audio produced no samples after resampling"};
        }

        std::vector<transcription::TranscriptSegment> segments;
        {
            std::lock_guard lock(mutex_);
            whisper_full_params params =
                whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
            params.print_progress = false;
            params.print_realtime = false;
            params.print_timestamps = false;
            params.print_special = false;
            params.translate = false;
            params.no_timestamps = false;
            params.token_timestamps = true;
            params.n_threads = threadCount_;
            params.language = language.c_str();

            const int status = whisper_full(context_, params, mono.data(),
                                            static_cast<int>(mono.size()));
            if (status != 0) {
                return AppError{ErrorCode::InvalidState,
                                "whisper inference failed with code " +
                                    std::to_string(status)};
            }

            auto mapped = mapSegments();
            if (!mapped.hasValue()) return mapped.error();
            segments = std::move(mapped).value();
        }

        return transcription::Transcript::create(std::move(segments),
                                                 std::move(language), options.sourceId);
    }

private:
    // Maps whisper's segments/tokens onto strictly ordered, non-overlapping
    // project-timebase value objects. Defensive against messy token timings:
    // ranges are clamped inside their segment and forced monotonic; a word that
    // still cannot form a valid range is dropped (word-level detail is optional),
    // and a segment whose words cannot validate falls back to no word-level
    // timing rather than failing the whole transcript.
    Result<std::vector<transcription::TranscriptSegment>> mapSegments() {
        std::vector<transcription::TranscriptSegment> segments;
        const int segmentCount = whisper_full_n_segments(context_);
        core::TimestampNs previousSegmentEnd{core::DurationNs{0}};

        for (int i = 0; i < segmentCount; ++i) {
            auto text = normalizeSegmentText(whisper_full_get_segment_text(context_, i));
            if (!text.has_value() || !domain::isValidUtf8(*text)) continue;

            std::int64_t startNs =
                whisper_full_get_segment_t0(context_, i) * kNanosPerCentisecond;
            std::int64_t endNs =
                whisper_full_get_segment_t1(context_, i) * kNanosPerCentisecond;

            const std::int64_t previousNs =
                previousSegmentEnd.time_since_epoch().count();
            startNs = std::max(startNs, previousNs);
            if (endNs <= startNs) endNs = startNs + kNanosPerCentisecond;

            auto range = domain::TimeRange::create(
                core::TimestampNs{core::DurationNs{startNs}},
                core::DurationNs{endNs - startNs});
            if (!range.hasValue()) continue;

            auto words = mapWords(i, range.value());

            auto segment = transcription::TranscriptSegment::create(
                *text, range.value(), std::move(words));
            if (!segment.hasValue()) {
                // Retry without word-level timing before giving up on the text.
                segment = transcription::TranscriptSegment::create(
                    *text, range.value(), {});
                if (!segment.hasValue()) continue;
            }
            previousSegmentEnd = range.value().end();
            segments.push_back(std::move(segment).value());
        }
        return segments;
    }

    std::vector<transcription::TranscriptWord> mapWords(int segmentIndex,
                                                        const domain::TimeRange& segment) {
        std::vector<transcription::TranscriptWord> words;
        const int tokenCount = whisper_full_n_tokens(context_, segmentIndex);
        core::TimestampNs cursor = segment.start();
        const std::int64_t segEndNs = segment.end().time_since_epoch().count();

        for (int t = 0; t < tokenCount; ++t) {
            auto text = normalizeTokenText(
                whisper_full_get_token_text(context_, segmentIndex, t));
            if (!text.has_value() || !domain::isValidUtf8(*text)) continue;

            const whisper_token_data data =
                whisper_full_get_token_data(context_, segmentIndex, t);
            std::int64_t startNs = data.t0 * kNanosPerCentisecond;
            std::int64_t endNs = data.t1 * kNanosPerCentisecond;

            const std::int64_t cursorNs = cursor.time_since_epoch().count();
            startNs = std::clamp(startNs, cursorNs, segEndNs);
            endNs = std::clamp(endNs, startNs, segEndNs);
            if (endNs <= startNs) continue;  // no positive duration -> drop word detail

            auto range = domain::TimeRange::create(
                core::TimestampNs{core::DurationNs{startNs}},
                core::DurationNs{endNs - startNs});
            if (!range.hasValue()) continue;

            auto word = transcription::TranscriptWord::create(
                *text, range.value(), clampConfidence(static_cast<double>(data.p)));
            if (!word.hasValue()) continue;
            cursor = range.value().end();
            words.push_back(std::move(word).value());
        }
        return words;
    }

    std::mutex mutex_;
    whisper_context* context_;
    int threadCount_;
};

WhisperTranscriptionProvider::WhisperTranscriptionProvider(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

WhisperTranscriptionProvider::~WhisperTranscriptionProvider() = default;

Result<std::unique_ptr<WhisperTranscriptionProvider>>
WhisperTranscriptionProvider::create(WhisperProviderConfig config) {
    auto verified = verifyWhisperRuntimeManifest(config.runtimeRoot);
    if (!verified.hasValue()) return verified.error();

    int threads = config.threadCount;
    if (threads <= 0) {
        const unsigned int hardware = std::thread::hardware_concurrency();
        threads = hardware == 0 ? 1 : static_cast<int>(std::min(hardware, 8u));
    }

    // whisper.cpp uses C callbacks and returns error codes, but ggml can still
    // throw std::bad_alloc; keep every path Result-shaped across the boundary.
    try {
        whisper_context_params contextParams = whisper_context_default_params();
        contextParams.use_gpu = false;  // audited build is CPU-only and reproducible

        const std::string modelPath =
            verified.value().modelPath.generic_string();
        whisper_context* context = whisper_init_from_file_with_params(
            modelPath.c_str(), contextParams);
        if (context == nullptr) {
            return AppError{ErrorCode::InvalidState,
                            "audited whisper model could not be loaded"};
        }
        auto impl = std::make_unique<Impl>(context, threads);
        return std::unique_ptr<WhisperTranscriptionProvider>(
            new WhisperTranscriptionProvider(std::move(impl)));
    } catch (const std::exception& error) {
        return AppError{ErrorCode::InvalidState,
                        std::string{"whisper initialization failed: "} + error.what()};
    } catch (...) {
        return AppError{ErrorCode::Unknown, "whisper initialization failed"};
    }
}

Result<transcription::Transcript> WhisperTranscriptionProvider::transcribe(
    const transcription::AudioInput& audio,
    const transcription::TranscriptionOptions& options) {
    try {
        return impl_->transcribe(audio, options);
    } catch (const std::exception& error) {
        return AppError{ErrorCode::InvalidState,
                        std::string{"whisper transcription failed: "} + error.what()};
    } catch (...) {
        return AppError{ErrorCode::Unknown, "whisper transcription failed"};
    }
}

}  // namespace creator::whisper_adapter
