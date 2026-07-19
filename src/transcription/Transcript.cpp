#include "transcription/Transcript.h"

#include "core/AppError.h"

#include <optional>
#include <utility>

namespace creator::transcription {

using core::AppError;
using core::ErrorCode;
using core::Result;

bool Transcript::isValidLanguageTag(const std::string& tag) noexcept {
    // BCP-47-shaped: ^[A-Za-z]{2,8}(-[A-Za-z0-9]{1,8})*$ . Validated by hand
    // rather than <regex> to avoid the header's locale/perf cost for one tag.
    const auto isAlpha = [](char c) noexcept {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    const auto isAlnum = [&](char c) noexcept {
        return isAlpha(c) || (c >= '0' && c <= '9');
    };

    std::size_t index = 0;
    const std::size_t size = tag.size();

    std::size_t primary = 0;
    while (index < size && isAlpha(tag[index])) {
        ++index;
        ++primary;
    }
    if (primary < 2 || primary > 8) return false;

    while (index < size) {
        if (tag[index] != '-') return false;
        ++index;
        std::size_t sub = 0;
        while (index < size && isAlnum(tag[index])) {
            ++index;
            ++sub;
        }
        if (sub < 1 || sub > 8) return false;
    }
    return true;
}

Result<Transcript> Transcript::create(std::vector<TranscriptSegment> segments,
                                      std::string languageTag, domain::SourceId sourceId) {
    if (!isValidLanguageTag(languageTag)) {
        return AppError{ErrorCode::InvalidArgument,
                        "transcript language tag must be a BCP-47-shaped tag"};
    }

    std::optional<core::TimestampNs> previousEnd;
    for (const TranscriptSegment& segment : segments) {
        if (previousEnd.has_value() && segment.range().start() < *previousEnd) {
            return AppError{ErrorCode::InvalidArgument,
                            "transcript segments must be ordered and non-overlapping"};
        }
        previousEnd = segment.range().end();
    }

    return Transcript{std::move(segments), std::move(languageTag), std::move(sourceId)};
}

const TranscriptSegment* Transcript::segmentAt(core::TimestampNs time) const noexcept {
    for (const TranscriptSegment& segment : segments_) {
        if (segment.range().start() <= time && time < segment.range().end()) {
            return &segment;
        }
    }
    return nullptr;
}

std::string Transcript::fullText() const {
    std::string text;
    for (const TranscriptSegment& segment : segments_) {
        if (!text.empty()) text += ' ';
        text += segment.text();
    }
    return text;
}

}  // namespace creator::transcription
