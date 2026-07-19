#pragma once

#include "avatar/ExpressionParameters.h"
#include "core/Result.h"

#include <string>
#include <vector>

namespace creator::avatar {

/// Provider-neutral expression channel that a model exposes under an
/// Inochi2D (or other renderer) parameter name.
enum class AvatarParameterSource {
    EyeOpenLeft,
    EyeOpenRight,
    BrowUpLeft,
    BrowUpRight,
    MouthOpen,
    MouthWide,
    HeadYaw,
    HeadPitch,
    HeadRoll,
};

struct AvatarParameterBinding final {
    std::string modelParameter;
    AvatarParameterSource source;
    float scale{1.0F};
    float offset{0.0F};
    float minimum{0.0F};
    float maximum{1.0F};
};

struct AvatarParameterValue final {
    std::string modelParameter;
    float value{0.0F};

    friend bool operator==(const AvatarParameterValue&,
                           const AvatarParameterValue&) = default;
};

/// Validates and applies a model's explicit parameter mapping.
///
/// The renderer adapter owns the model-specific names and ranges; this class
/// only turns the nine canonical tracking channels into deterministic values.
/// It therefore remains independent of Inochi2D headers and model files while
/// preserving clamping, scaling, and offset semantics at one boundary.
class AvatarParameterMapper final {
public:
    [[nodiscard]] static core::Result<AvatarParameterMapper> create(
        std::vector<AvatarParameterBinding> bindings);

    AvatarParameterMapper(AvatarParameterMapper&&) noexcept = default;
    AvatarParameterMapper& operator=(AvatarParameterMapper&&) noexcept = default;
    AvatarParameterMapper(const AvatarParameterMapper&) = default;
    AvatarParameterMapper& operator=(const AvatarParameterMapper&) = default;

    [[nodiscard]] const std::vector<AvatarParameterBinding>& bindings() const noexcept {
        return bindings_;
    }
    [[nodiscard]] core::Result<std::vector<AvatarParameterValue>> map(
        const ExpressionParameters& parameters) const;

private:
    explicit AvatarParameterMapper(std::vector<AvatarParameterBinding> bindings)
        : bindings_(std::move(bindings)) {}

    std::vector<AvatarParameterBinding> bindings_;
};

}  // namespace creator::avatar
