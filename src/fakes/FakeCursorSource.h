#pragma once

#include "cursor/ICursorSource.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace creator::fakes {

/// A deterministic, scripted ICursorSource for tests.
///
/// Deliberately has no thread, no clock, and no RNG: it replays a fixed vector
/// of raw samples that the test hands it, one per poll(), in order, and returns
/// nullopt once the script is exhausted. Two runs of the same script therefore
/// produce byte-identical output, which is what CLAUDE.md §8's determinism
/// requirement needs and what makes the whole cursor pipeline reproducible
/// before the real Windows Raw Input backend exists.
///
/// It emits RAW samples (physical pixels + source dimensions); turning those
/// into normalized, schema-valid events is the job of CursorNormalizer and the
/// serializer downstream, exactly as the real backend's output would be.
class FakeCursorSource final : public creator::cursor::ICursorSource {
public:
    explicit FakeCursorSource(std::vector<creator::cursor::RawCursorSample> script);

    [[nodiscard]] std::optional<creator::cursor::RawCursorSample> poll() override;

    /// True once every scripted sample has been polled.
    [[nodiscard]] bool exhausted() const noexcept;

private:
    std::vector<creator::cursor::RawCursorSample> script_;
    std::size_t next_{0};
};

}  // namespace creator::fakes
