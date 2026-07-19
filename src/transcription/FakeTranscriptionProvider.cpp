#include "transcription/FakeTranscriptionProvider.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/TimelineTypes.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptSegment.h"
#include "transcription/TranscriptWord.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace creator::transcription {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

// A fixed, ordered vocabulary. Cycled by word index so the output is scripted
// and reproducible; the fake never inspects sample VALUES to pick a word.
constexpr std::array<std::string_view, 8> kVocabulary{
    "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog"};

domain::TimeRange rangeNs(std::int64_t startNs, std::int64_t durationNs) {
    // Cadence is chosen so these always satisfy TimeRange's invariants; the
    // check is kept because a future cadence change must not silently produce an
    // invalid range.
    return domain::TimeRange::create(core::TimestampNs{core::DurationNs{startNs}},
                                     core::DurationNs{durationNs})
        .value();
}

}  // namespace

Result<Transcript> FakeTranscriptionProvider::transcribe(const AudioInput& audio,
                                                         const TranscriptionOptions& options) {
    if (audio.frameCount() == 0) {
        return AppError{ErrorCode::InvalidArgument, "cannot transcribe empty audio"};
    }
    if (!audio.hasOnlyFiniteSamples()) {
        return AppError{ErrorCode::InvalidArgument,
                        "cannot transcribe audio containing non-finite samples"};
    }

    const std::int64_t totalNs = audio.duration().count();
    const std::int64_t slotNs = kWordSlot.count();
    const std::int64_t spokenNs = kWordSpoken.count();
    const std::int64_t wordCount = totalNs / slotNs;

    std::vector<TranscriptSegment> segments;
    std::vector<TranscriptWord> current;
    std::string currentText;
    std::int64_t segmentFirstWord = 0;

    const auto finalizeSegment = [&](std::int64_t firstWord) -> Result<void> {
        if (current.empty()) return core::ok();
        const std::int64_t start = firstWord * slotNs;
        const std::int64_t end = current.back().range().end().time_since_epoch().count();
        auto segment = TranscriptSegment::create(currentText, rangeNs(start, end - start),
                                                 std::move(current));
        if (!segment.hasValue()) return segment.error();
        segments.push_back(std::move(segment).value());
        current.clear();
        currentText.clear();
        return core::ok();
    };

    for (std::int64_t index = 0; index < wordCount; ++index) {
        const std::int64_t start = index * slotNs;
        const std::string text{kVocabulary[static_cast<std::size_t>(index) % kVocabulary.size()]};
        // Deterministic confidence pattern in [0.6, 1.0]; no clock, no RNG.
        const double confidence = 1.0 - static_cast<double>(index % 5) * 0.1;
        auto word = TranscriptWord::create(text, rangeNs(start, spokenNs), confidence);
        if (!word.hasValue()) return word.error();

        if (!currentText.empty()) currentText += ' ';
        currentText += text;
        current.push_back(std::move(word).value());

        if (static_cast<int>(current.size()) == kWordsPerSegment) {
            auto flushed = finalizeSegment(segmentFirstWord);
            if (!flushed.hasValue()) return flushed.error();
            segmentFirstWord = index + 1;
        }
    }
    if (auto flushed = finalizeSegment(segmentFirstWord); !flushed.hasValue()) {
        return flushed.error();
    }

    std::string language =
        options.languageTag.empty() ? std::string{"en"} : options.languageTag;
    return Transcript::create(std::move(segments), std::move(language), options.sourceId);
}

}  // namespace creator::transcription
