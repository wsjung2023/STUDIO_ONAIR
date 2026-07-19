#pragma once

namespace creator::avatar {

/// Normalized facial-expression parameters, provider-neutral.
///
/// A fixed-field struct rather than a name->value map: "매직 문자열 대신 typed
/// ID/value object 사용" (CLAUDE.md 4) applies to expression channels just as
/// much as to identifiers. Any tracking engine (MediaPipe, OpenSeeFace, a
/// deterministic fake) must reduce its own output to exactly these fields
/// before it crosses the tracking-port boundary.
///
/// eyeOpenLeft, eyeOpenRight, browUpLeft, browUpRight, mouthOpen, mouthWide are
/// normalized to [0, 1] (0 = fully closed/neutral, 1 = fully open/raised/wide).
/// headYaw, headPitch, headRoll are normalized to [-1, 1] (0 = facing camera
/// straight on).
struct ExpressionParameters final {
    float eyeOpenLeft{0.0F};
    float eyeOpenRight{0.0F};
    float browUpLeft{0.0F};
    float browUpRight{0.0F};
    float mouthOpen{0.0F};
    float mouthWide{0.0F};
    float headYaw{0.0F};
    float headPitch{0.0F};
    float headRoll{0.0F};

    friend bool operator==(const ExpressionParameters&, const ExpressionParameters&) = default;

    /// The face-at-rest baseline: every field at its documented zero point
    /// (eyes/brows/mouth closed, head facing the camera straight on). A member
    /// function rather than an in-class static data member of this same type:
    /// the class is still incomplete at the point its own data members are
    /// declared, so a `static constexpr ExpressionParameters` data member
    /// cannot be initialized in-class here, but member function bodies are a
    /// complete-class context and can construct one freely.
    static constexpr ExpressionParameters neutral() noexcept { return ExpressionParameters{}; }
};

}  // namespace creator::avatar
