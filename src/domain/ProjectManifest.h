#pragma once

#include "core/Result.h"
#include "core/Utc.h"
#include "domain/Identifiers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace creator::domain {

/// Colour space recorded in manifest.json. Mirrors the colorSpace enum in
/// schemas/project.schema.json, which currently permits exactly one value.
///
/// Separate from media::ColorSpace on purpose: one is a persisted format
/// detail, the other is a runtime frame property. Giving them the same name
/// invites a `using` that welds the two layers together.
enum class ManifestColorSpace {
    Rec709Sdr,
};

/// Returns the on-disk spelling from the schema ("rec709-sdr"), not the C++
/// enumerator name.
[[nodiscard]] std::string_view toString(ManifestColorSpace colorSpace) noexcept;

/// Fails with UnsupportedVersion for a spelling this build does not know: an
/// unrecognised colour space means the project came from a newer version, not
/// that the user passed a bad argument.
[[nodiscard]] core::Result<ManifestColorSpace> parseColorSpace(std::string_view text);

/// Canvas settings. Frame rate is a rational for the reason spelled out in
/// core/Timebase.h: 59.94 is 60000/1001 and a double drifts.
struct CanvasSettings final {
    std::int32_t width{1920};
    std::int32_t height{1080};
    std::int64_t frameRateNumerator{60};
    std::int64_t frameRateDenominator{1};
    ManifestColorSpace colorSpace{ManifestColorSpace::Rec709Sdr};

    friend bool operator==(const CanvasSettings&, const CanvasSettings&) = default;
};

/// Directory names inside the project package, relative to the .cstudio folder.
/// Stored rather than hardcoded so a project written by another version still
/// resolves its own layout.
struct ProjectDirectories final {
    std::string media{"media"};
    std::string audio{"audio"};
    std::string telemetry{"telemetry"};
    std::string proxies{"proxies"};
    std::string thumbnails{"thumbnails"};
    std::string autosave{"autosave"};
    std::string renders{"renders"};
    std::string logs{"logs"};

    friend bool operator==(const ProjectDirectories&, const ProjectDirectories&) = default;
};

/// In-memory form of manifest.json, mirroring schemas/project.schema.json.
///
/// A working project is a folder, not a zip: re-zipping gigabytes on every save
/// makes crash recovery impossible (ARCHITECTURE.md 7.1).
struct ProjectManifest final {
    static constexpr std::int32_t kCurrentSchemaVersion = 1;
    static constexpr std::string_view kDatabaseFileName = "project.db";

    std::int32_t schemaVersion{kCurrentSchemaVersion};
    ProjectId projectId;
    std::string name;
    core::Utc createdAt;
    core::Utc updatedAt;
    CanvasSettings canvas;
    std::string database{kDatabaseFileName};
    ProjectDirectories directories;
    std::vector<std::string> requiredFeatures;

    friend bool operator==(const ProjectManifest&, const ProjectManifest&) = default;
};

/// Field-level checks against the constraints in schemas/project.schema.json.
///
/// This is NOT JSON Schema validation. The instruction for this task is
/// "manifest 생성·읽기"; applying the schema document itself is R0-02's
/// "manifest JSON 스키마 검증" item. See
/// docs/superpowers/specs/2026-07-16-r0-01-bootstrap-design.md 5.
[[nodiscard]] core::Result<void> validate(const ProjectManifest& manifest);

}  // namespace creator::domain
