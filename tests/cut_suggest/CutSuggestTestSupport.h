#pragma once

// Shared, header-only builders for the cut-suggestion tests: typed time ranges,
// transcript words, and synthetic PCM. Deterministic and Qt-free.

#include "core/Timebase.h"
#include "domain/TimelineTypes.h"
#include "transcription/TranscriptWord.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cut_suggest_test {

inline creator::domain::TimeRange range(std::int64_t startNs, std::int64_t durNs) {
    return creator::domain::TimeRange::create(
               creator::core::TimestampNs{creator::core::DurationNs{startNs}},
               creator::core::DurationNs{durNs})
        .value();
}

inline creator::transcription::TranscriptWord word(std::string text,
                                                   std::int64_t startNs,
                                                   std::int64_t durNs,
                                                   double confidence) {
    return creator::transcription::TranscriptWord::create(
               std::move(text), range(startNs, durNs), confidence)
        .value();
}

// Appends `frames` frames of a constant-amplitude interleaved mono signal.
inline void appendLevel(std::vector<float>& out, std::size_t frames, float amplitude) {
    for (std::size_t i = 0; i < frames; ++i) {
        // Alternate sign so the block reads as an AC signal, not DC; |value| is
        // the amplitude, so its RMS equals `amplitude`.
        out.push_back((i % 2 == 0) ? amplitude : -amplitude);
    }
}

}  // namespace cut_suggest_test
