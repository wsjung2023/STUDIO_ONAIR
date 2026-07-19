#pragma once

#include "core/Result.h"
#include "cut_suggest/CutReason.h"
#include "domain/TimelineTypes.h"

#include <optional>
#include <string>

namespace creator::cut_suggest {

/// A single editable "you could remove this" suggestion produced by R2-05.
///
/// This is the R2-05 analogue of R2-03's ZoomCandidate: a pure, deterministic
/// proposal, never an applied edit. It carries:
///   - span:   a domain::TimeRange on the project timebase (CLAUDE.md 2.3)
///             covering the removable region. The actual non-destructive CUT
///             (ripple/gap handling, timeline mutation) is DEFERRED to the
///             editor + edit-engine (Codex R1 territory); this object only owns
///             the *where* and *why*.
///   - reason: Silence or Filler — which detector proposed it.
///   - score:  a confidence in [0, 1]. Higher means the heuristic is more sure
///             the span is safe to cut (deeper/longer silence, or a
///             higher-confidence filler match). The editor surfaces this so a
///             human can accept, tweak, or drop the suggestion; nothing
///             auto-applies (CLAUDE.md 2, 7).
///   - label:  an optional short human-readable tag, e.g. the matched filler
///             word "um" or phrase "you know". Absent for silence.
///
/// Constructed only through create(), so a suggestion with an out-of-range
/// score or an empty/invalid label cannot exist. The TimeRange is an
/// already-valid value object (non-negative start, positive duration).
class CutSuggestion final {
public:
    /// Fails with InvalidArgument if the score is not finite or falls outside
    /// the closed interval [0, 1], or if a label is present but empty or not
    /// valid UTF-8.
    [[nodiscard]] static core::Result<CutSuggestion> create(
        domain::TimeRange span, CutReason reason, double score,
        std::optional<std::string> label = std::nullopt);

    [[nodiscard]] const domain::TimeRange& span() const noexcept { return span_; }
    [[nodiscard]] CutReason reason() const noexcept { return reason_; }
    [[nodiscard]] double score() const noexcept { return score_; }
    [[nodiscard]] const std::optional<std::string>& label() const noexcept {
        return label_;
    }

    friend bool operator==(const CutSuggestion&, const CutSuggestion&) = default;

private:
    CutSuggestion(domain::TimeRange span, CutReason reason, double score,
                  std::optional<std::string> label)
        : span_(span),
          reason_(reason),
          score_(score),
          label_(std::move(label)) {}

    domain::TimeRange span_;
    CutReason reason_;
    double score_{};
    std::optional<std::string> label_;
};

}  // namespace creator::cut_suggest
