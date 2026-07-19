#pragma once

#include "core/AppError.h"
#include "core/Result.h"
#include "domain/TimelineTypes.h"

#include <cmath>
#include <string>
#include <utility>

namespace creator::transcription {

/// One recognized word inside a transcript segment.
///
/// Responsibility: bind a piece of transcribed text to a span on the project
/// timebase (CLAUDE.md 2.3) and the recognizer's confidence for it. A value
/// object: it is either fully valid (constructed via create()) or it does not
/// exist. The private constructor and validated create() make an empty-text or
/// out-of-range-confidence word unrepresentable.
class TranscriptWord final {
public:
    /// Fails with InvalidArgument if text is empty or is not valid UTF-8, or if
    /// confidence is not a finite number in [0, 1]. The time range is already an
    /// invariant of domain::TimeRange (non-negative start, positive duration).
    [[nodiscard]] static core::Result<TranscriptWord> create(
        std::string text, domain::TimeRange range, double confidence) {
        if (text.empty()) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "transcript word text must not be empty"};
        }
        if (!domain::isValidUtf8(text)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "transcript word text must be valid UTF-8"};
        }
        if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "transcript word confidence must be a finite value in [0, 1]"};
        }
        return TranscriptWord{std::move(text), range, confidence};
    }

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const domain::TimeRange& range() const noexcept { return range_; }
    [[nodiscard]] double confidence() const noexcept { return confidence_; }

    friend bool operator==(const TranscriptWord&, const TranscriptWord&) = default;

private:
    TranscriptWord(std::string text, domain::TimeRange range, double confidence)
        : text_(std::move(text)), range_(range), confidence_(confidence) {}

    std::string text_;
    domain::TimeRange range_;
    double confidence_;
};

}  // namespace creator::transcription
