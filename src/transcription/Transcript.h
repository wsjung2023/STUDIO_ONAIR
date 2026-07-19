#pragma once

#include "core/Result.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "transcription/TranscriptSegment.h"

#include <string>
#include <vector>

namespace creator::transcription {

/// The full result of transcribing one audio source: an ordered list of
/// segments, the language they are in, and the id of the source they came from.
///
/// Responsibility: be the module's aggregate value object. Constructed only via
/// the validated create(); segments are guaranteed strictly ordered and
/// non-overlapping on the project timebase (CLAUDE.md 2.3), and the language tag
/// is guaranteed to be a BCP-47-shaped string. Query helpers answer "which
/// segment is playing at time t" and "what is the whole text" without exposing
/// the internal vector to mutation.
class Transcript final {
public:
    /// Fails with InvalidArgument if:
    ///  - languageTag is not a BCP-47-shaped tag (e.g. "en", "ko", "en-US"); or
    ///  - the segments are not strictly ordered and non-overlapping by time.
    /// An empty segment list is allowed (silence produced no speech).
    [[nodiscard]] static core::Result<Transcript> create(
        std::vector<TranscriptSegment> segments, std::string languageTag,
        domain::SourceId sourceId);

    [[nodiscard]] const std::vector<TranscriptSegment>& segments() const noexcept {
        return segments_;
    }
    [[nodiscard]] const std::string& languageTag() const noexcept { return languageTag_; }
    [[nodiscard]] const domain::SourceId& sourceId() const noexcept { return sourceId_; }

    /// The segment whose range contains `time`, or nullptr if none does. The
    /// returned pointer is owned by this Transcript and is valid for its
    /// lifetime.
    [[nodiscard]] const TranscriptSegment* segmentAt(core::TimestampNs time) const noexcept;

    /// All segment texts joined with a single space, in order. Empty transcript
    /// yields an empty string.
    [[nodiscard]] std::string fullText() const;

    /// True if `tag` has the shape of a BCP-47 language tag: a 2-8 letter primary
    /// subtag optionally followed by '-' separated 1-8 alphanumeric subtags.
    /// Deliberately lenient ("BCP-47-ish"): it validates shape, not the IANA
    /// registry.
    [[nodiscard]] static bool isValidLanguageTag(const std::string& tag) noexcept;

    friend bool operator==(const Transcript&, const Transcript&) = default;

private:
    Transcript(std::vector<TranscriptSegment> segments, std::string languageTag,
               domain::SourceId sourceId)
        : segments_(std::move(segments)),
          languageTag_(std::move(languageTag)),
          sourceId_(std::move(sourceId)) {}

    std::vector<TranscriptSegment> segments_;
    std::string languageTag_;
    domain::SourceId sourceId_;
};

}  // namespace creator::transcription
