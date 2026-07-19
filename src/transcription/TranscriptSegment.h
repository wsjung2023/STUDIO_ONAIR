#pragma once

#include "core/Result.h"
#include "domain/TimelineTypes.h"
#include "transcription/TranscriptWord.h"

#include <optional>
#include <string>
#include <vector>

namespace creator::transcription {

/// A contiguous run of transcribed speech: a phrase or utterance.
///
/// Responsibility: hold a piece of text, its span on the project timebase
/// (CLAUDE.md 2.3), an ordered list of the words it is made of, and an optional
/// speaker label. Constructed only via the validated create(); the invariants
/// (words in range, monotonic, non-overlapping) are checked once at
/// construction so every consumer can trust them.
class TranscriptSegment final {
public:
    /// Fails with InvalidArgument if:
    ///  - text is empty or not valid UTF-8;
    ///  - a speaker label is present but empty or not valid UTF-8;
    ///  - any word's range falls outside the segment's range; or
    ///  - the words are not strictly ordered and non-overlapping by time.
    /// An empty word list is allowed (a segment carrying no word-level timing).
    [[nodiscard]] static core::Result<TranscriptSegment> create(
        std::string text, domain::TimeRange range, std::vector<TranscriptWord> words,
        std::optional<std::string> speaker = std::nullopt);

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const domain::TimeRange& range() const noexcept { return range_; }
    [[nodiscard]] const std::vector<TranscriptWord>& words() const noexcept { return words_; }
    [[nodiscard]] const std::optional<std::string>& speaker() const noexcept {
        return speaker_;
    }

    friend bool operator==(const TranscriptSegment&, const TranscriptSegment&) = default;

private:
    TranscriptSegment(std::string text, domain::TimeRange range,
                      std::vector<TranscriptWord> words,
                      std::optional<std::string> speaker)
        : text_(std::move(text)),
          range_(range),
          words_(std::move(words)),
          speaker_(std::move(speaker)) {}

    std::string text_;
    domain::TimeRange range_;
    std::vector<TranscriptWord> words_;
    std::optional<std::string> speaker_;
};

}  // namespace creator::transcription
