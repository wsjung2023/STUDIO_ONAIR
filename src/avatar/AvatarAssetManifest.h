#pragma once

#include "avatar/AvatarLicense.h"
#include "avatar/AvatarTypes.h"
#include "core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace creator::avatar {

struct AvatarAssetDependency final {
    AvatarAssetId assetId;
    std::string version;

    friend bool operator==(const AvatarAssetDependency&, const AvatarAssetDependency&) = default;
};

struct AvatarPayloadHash final {
    std::string path;
    std::string sha256;

    friend bool operator==(const AvatarPayloadHash&, const AvatarPayloadHash&) = default;
};

struct AvatarPerformanceMetadata final {
    std::uint64_t payloadBytes{};
    std::uint64_t textureBytes{};
    std::uint32_t textureCount{};
    std::uint32_t maxTextureDimension{};
    std::uint32_t vertexCount{};
    std::uint32_t triangleCount{};
    std::uint32_t drawCallCount{};
    std::uint32_t drawPartCount{};
    std::uint32_t boneCount{};

    friend bool operator==(const AvatarPerformanceMetadata&,
                           const AvatarPerformanceMetadata&) = default;
};

struct AvatarAssetManifestDraft final {
    AvatarPackageId packageId;
    std::string packageVersion;
    AvatarAssetId assetId;
    std::string assetVersion;
    std::string displayName;
    std::string vendor;
    std::vector<AvatarRepresentation> supportedRepresentations;
    std::vector<RigFamily> supportedRigFamilies;
    std::vector<AvatarSlot> allowedSlots;
    std::vector<AvatarAssetDependency> dependencies;
    std::vector<AvatarPayloadHash> payloads;
    AvatarPerformanceMetadata performance;
    std::string sourceUri;
    std::string licenseId;
    std::string licenseVersion;
    std::vector<LicenseGrant> grants;
    std::string attributionText;
    std::vector<std::string> regionAllowList;
    core::Utc validFrom;
    std::optional<core::Utc> validUntil;
};

/// Validated immutable identity, compatibility, payload, and rights manifest.
class AvatarAssetManifest final {
public:
    static constexpr std::int32_t kCurrentSchemaVersion = 1;

    [[nodiscard]] static core::Result<AvatarAssetManifest> create(
        AvatarAssetManifestDraft draft);

    [[nodiscard]] std::int32_t schemaVersion() const noexcept;
    [[nodiscard]] const AvatarPackageId& packageId() const noexcept;
    [[nodiscard]] const AvatarAssetId& assetId() const noexcept;
    [[nodiscard]] const std::vector<LicenseGrant>& grants() const noexcept;
    [[nodiscard]] const std::string& attributionText() const noexcept;
    [[nodiscard]] const std::vector<std::string>& regionAllowList() const noexcept;
    [[nodiscard]] core::Utc validFrom() const noexcept;
    [[nodiscard]] const std::optional<core::Utc>& validUntil() const noexcept;
    [[nodiscard]] const AvatarAssetManifestDraft& values() const noexcept;

private:
    explicit AvatarAssetManifest(AvatarAssetManifestDraft values);

    AvatarAssetManifestDraft values_;
};

}  // namespace creator::avatar
