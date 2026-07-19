#pragma once

#include "avatar/AvatarMotionSample.h"
#include "core/Result.h"

#include <filesystem>
#include <vector>

namespace creator::avatar {

/// Loads recorded avatar.motion NDJSON and samples it on project time.
class AvatarMotionPlayback final {
public:
    [[nodiscard]] static core::Result<AvatarMotionPlayback> load(
        const std::filesystem::path& telemetryPath);

    [[nodiscard]] const std::vector<AvatarMotionSample>& samples() const noexcept {
        return samples_;
    }
    [[nodiscard]] core::Result<AvatarMotionSample> sampleAt(
        core::TimestampNs timestamp) const;

private:
    explicit AvatarMotionPlayback(std::vector<AvatarMotionSample> samples)
        : samples_(std::move(samples)) {}

    std::vector<AvatarMotionSample> samples_;
};

}  // namespace creator::avatar
