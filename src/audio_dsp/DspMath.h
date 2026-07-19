#pragma once

#include <cmath>

namespace creator::audio_dsp {

/// Denormal (subnormal) floats in a decaying IIR feedback path cause severe
/// x86 CPU stalls — a real-time hazard that shows up as audio dropouts
/// (CLAUDE.md §9). These helpers flush a tiny magnitude to exactly zero so the
/// persisted feedback state can never linger in the subnormal region. The
/// threshold (1e-20) sits far below anything audible (~-400 dBFS) yet well
/// above the float/double subnormal region, so it never touches a real signal
/// but forces settled state to a clean zero. This is a purely local flush of a
/// stored value — it deliberately avoids `_mm_setcsr`/MXCSR, which is a global,
/// non-portable side effect.
[[nodiscard]] inline float flushDenorm(float x) noexcept {
    return std::abs(x) < 1.0e-20F ? 0.0F : x;
}

/// Double overload for state carried in double precision (envelope followers,
/// biquad state). Same threshold and rationale as the float overload.
[[nodiscard]] inline double flushDenorm(double x) noexcept {
    return std::abs(x) < 1.0e-20 ? 0.0 : x;
}

}  // namespace creator::audio_dsp
