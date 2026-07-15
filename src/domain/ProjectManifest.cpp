#include "domain/ProjectManifest.h"

#include "core/Uuid.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace creator::domain {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

// Bounds copied from schemas/project.schema.json.
constexpr std::int32_t kMinCanvasDimension = 16;
constexpr std::int32_t kMaxCanvasDimension = 16384;
constexpr std::size_t kMaxNameLength = 200;

Result<void> validateDirectory(std::string_view label, const std::string& value) {
    if (value.empty()) {
        return AppError{ErrorCode::InvalidArgument,
                        std::string{"directory '"} + std::string{label} + "' must not be empty"};
    }
    return core::ok();
}

}  // namespace

std::string_view toString(ManifestColorSpace colorSpace) noexcept {
    switch (colorSpace) {
        case ManifestColorSpace::Rec709Sdr:
            return "rec709-sdr";
    }
    return "rec709-sdr";
}

Result<ManifestColorSpace> parseColorSpace(std::string_view text) {
    if (text == "rec709-sdr") {
        return ManifestColorSpace::Rec709Sdr;
    }
    return AppError{ErrorCode::UnsupportedVersion,
                    "unknown colour space '" + std::string{text} + "'"};
}

Result<void> validate(const ProjectManifest& manifest) {
    if (manifest.schemaVersion < 1) {
        return AppError{ErrorCode::UnsupportedVersion, "schemaVersion must be at least 1"};
    }
    if (manifest.schemaVersion > ProjectManifest::kCurrentSchemaVersion) {
        return AppError{ErrorCode::UnsupportedVersion,
                        "project was written by a newer version of Creator Studio"};
    }

    if (!core::isUuidV4(manifest.projectId.value())) {
        return AppError{ErrorCode::InvalidArgument, "projectId must be a v4 UUID"};
    }

    if (manifest.name.empty()) {
        return AppError{ErrorCode::InvalidArgument, "project name must not be empty"};
    }
    if (manifest.name.size() > kMaxNameLength) {
        return AppError{ErrorCode::InvalidArgument, "project name must be at most 200 characters"};
    }

    if (manifest.database != ProjectManifest::kDatabaseFileName) {
        return AppError{ErrorCode::InvalidArgument, "database must be 'project.db'"};
    }

    if (manifest.canvas.width < kMinCanvasDimension || manifest.canvas.width > kMaxCanvasDimension ||
        manifest.canvas.height < kMinCanvasDimension ||
        manifest.canvas.height > kMaxCanvasDimension) {
        return AppError{ErrorCode::InvalidArgument,
                        "canvas dimensions must be between 16 and 16384"};
    }
    if (manifest.canvas.frameRateNumerator < 1 || manifest.canvas.frameRateDenominator < 1) {
        return AppError{ErrorCode::InvalidArgument,
                        "canvas frame rate numerator and denominator must be at least 1"};
    }

    // updatedAt < createdAt means someone edited the manifest by hand or a clock
    // went backwards. Either way the file is not trustworthy.
    if (manifest.updatedAt < manifest.createdAt) {
        return AppError{ErrorCode::InvalidArgument, "updatedAt must not precede createdAt"};
    }

    const std::array<std::pair<std::string_view, const std::string*>, 8> directories{{
        {"media", &manifest.directories.media},
        {"audio", &manifest.directories.audio},
        {"telemetry", &manifest.directories.telemetry},
        {"proxies", &manifest.directories.proxies},
        {"thumbnails", &manifest.directories.thumbnails},
        {"autosave", &manifest.directories.autosave},
        {"renders", &manifest.directories.renders},
        {"logs", &manifest.directories.logs},
    }};
    for (const auto& [label, value] : directories) {
        if (auto result = validateDirectory(label, *value); !result.hasValue()) {
            return result;
        }
    }

    // schema marks requiredFeatures uniqueItems.
    std::vector<std::string> sorted = manifest.requiredFeatures;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return AppError{ErrorCode::InvalidArgument, "requiredFeatures must not contain duplicates"};
    }

    return core::ok();
}

}  // namespace creator::domain
