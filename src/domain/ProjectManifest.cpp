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

/// Counts Unicode code points in UTF-8 text.
///
/// JSON Schema measures maxLength in code points, but std::string::size()
/// counts bytes. Using size() would reject names the schema allows, and the
/// gap is not academic for this product: Hangul is three bytes per character
/// in UTF-8, so a 67-character Korean project name already exceeds 200 bytes
/// while sitting nowhere near the schema's 200-character limit. ASCII fixtures
/// hide this completely - bytes and code points coincide there.
///
/// Continuation bytes are 10xxxxxx; every other byte begins a code point.
[[nodiscard]] std::size_t utf8Length(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const unsigned char byte : text) {
        if ((byte & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

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

    // Stricter than the schema on purpose. project.schema.json says
    // {"format": "uuid"}, which admits any RFC 4122 version, but we only ever
    // generate v4 (core/Uuid.h) and a non-v4 id would mean the manifest came
    // from something that is not this product. Rejecting it here is a
    // compatibility decision, not an oversight: if we ever need to open
    // projects written by another tool, this is the line to revisit.
    if (!core::isUuidV4(manifest.projectId.value())) {
        return AppError{ErrorCode::InvalidArgument, "projectId must be a v4 UUID"};
    }

    if (manifest.name.empty()) {
        return AppError{ErrorCode::InvalidArgument, "project name must not be empty"};
    }
    // Code points, not bytes - the schema says characters. See utf8Length.
    if (utf8Length(manifest.name) > kMaxNameLength) {
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
