#include "transcription/TranscriptSegment.h"

#include "core/AppError.h"

#include <utility>

namespace creator::transcription {

using core::AppError;
using core::ErrorCode;
using core::Result;

Result<TranscriptSegment> TranscriptSegment::create(
    std::string text, domain::TimeRange range, std::vector<TranscriptWord> words,
    std::optional<std::string> speaker) {
    if (text.empty() || !domain::isValidUtf8(text)) {
        return AppError{ErrorCode::InvalidArgument,
                        "transcript segment text must be non-empty valid UTF-8"};
    }
    if (speaker.has_value() && (speaker->empty() || !domain::isValidUtf8(*speaker))) {
        return AppError{ErrorCode::InvalidArgument,
                        "transcript segment speaker label must be non-empty valid UTF-8"};
    }

    // Words must lie inside the segment and be strictly ordered without overlap.
    // Seeding the cursor with the segment start means the first word may begin
    // exactly at the segment start but never before it.
    core::TimestampNs cursor = range.start();
    for (const TranscriptWord& word : words) {
        if (word.range().start() < range.start() || word.range().end() > range.end()) {
            return AppError{ErrorCode::InvalidArgument,
                            "transcript word range falls outside its segment range"};
        }
        if (word.range().start() < cursor) {
            return AppError{ErrorCode::InvalidArgument,
                            "transcript words must be ordered and non-overlapping"};
        }
        cursor = word.range().end();
    }

    return TranscriptSegment{std::move(text), range, std::move(words), std::move(speaker)};
}

}  // namespace creator::transcription
