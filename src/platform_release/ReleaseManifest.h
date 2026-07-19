#pragma once

#include "core/Result.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace creator::platform_release {

struct ReleaseArtifact final {
    std::string relativePath;
    std::string sha256;

    friend bool operator==(const ReleaseArtifact&, const ReleaseArtifact&) = default;
};

/// Immutable release evidence. It contains only final artifact paths relative
/// to a release directory and their SHA-256 values; credentials, endpoints and
/// user content are intentionally not representable here.
class ReleaseManifest final {
public:
    [[nodiscard]] static core::Result<ReleaseManifest> create(
        std::string productVersion, std::string sourceRevision,
        std::string target, std::vector<ReleaseArtifact> artifacts);
    [[nodiscard]] static core::Result<ReleaseManifest> fromJson(const nlohmann::json& document);

    [[nodiscard]] nlohmann::json toJson() const;
    [[nodiscard]] const std::string& productVersion() const noexcept { return productVersion_; }
    [[nodiscard]] const std::string& sourceRevision() const noexcept { return sourceRevision_; }
    [[nodiscard]] const std::string& target() const noexcept { return target_; }
    [[nodiscard]] const std::vector<ReleaseArtifact>& artifacts() const noexcept { return artifacts_; }

    friend bool operator==(const ReleaseManifest&, const ReleaseManifest&) = default;

private:
    ReleaseManifest(std::string productVersion, std::string sourceRevision,
                    std::string target, std::vector<ReleaseArtifact> artifacts)
        : productVersion_(std::move(productVersion)),
          sourceRevision_(std::move(sourceRevision)), target_(std::move(target)),
          artifacts_(std::move(artifacts)) {}

    std::string productVersion_;
    std::string sourceRevision_;
    std::string target_;
    std::vector<ReleaseArtifact> artifacts_;
};

}  // namespace creator::platform_release
