#pragma once

#include <string_view>

namespace creator::cut_suggest {

/// Why a span was proposed as a cut. R2-05 produces two kinds of removable
/// spans, and the reason travels with the suggestion so the editor (DEFERRED,
/// Codex territory) can group, colour and explain them to a human reviewer.
///
///  - Silence: the recorded audio level stayed below a threshold long enough
///             that the span is very likely dead air.
///  - Filler:  a transcript word (or short adjacent run) matched the filler
///             lexicon ("um", "you know", ...).
enum class CutReason {
    Silence,
    Filler,
};

/// Stable lowercase wire form of a CutReason, used by the serializer and schema.
[[nodiscard]] std::string_view toString(CutReason reason) noexcept;

}  // namespace creator::cut_suggest
