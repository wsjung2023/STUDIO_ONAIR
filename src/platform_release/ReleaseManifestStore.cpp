#include "platform_release/ReleaseManifestStore.h"

#include "core/AppError.h"
#include "core/Sha256.h"
#include "core/Uuid.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>

namespace creator::platform_release {
namespace {

core::Result<void> verifyArtifacts(const std::filesystem::path& root,
                                   const ReleaseManifest& manifest) {
    for (const auto& artifact : manifest.artifacts()) {
        const auto hash = core::sha256File(root / artifact.relativePath);
        if (!hash.hasValue() || hash.value() != artifact.sha256) {
            return core::AppError{core::ErrorCode::IoFailure,
                                  "release artifact is missing or its SHA-256 does not match"};
        }
    }
    return core::ok();
}

}  // namespace

core::Result<void> ReleaseManifestStore::write(const std::filesystem::path& manifestPath,
                                                const std::filesystem::path& artifactRoot,
                                                const ReleaseManifest& manifest) const {
    const auto verified = verifyArtifacts(artifactRoot, manifest);
    if (!verified.hasValue()) return verified.error();

    std::error_code ec;
    std::filesystem::create_directories(manifestPath.parent_path(), ec);
    if (ec) return core::AppError{core::ErrorCode::IoFailure, "release manifest directory could not be created"};
    const auto temporary = manifestPath.parent_path() /
        ("." + manifestPath.filename().string() + ".part-" + core::generateUuidV4());
    {
        std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
        if (!output) return core::AppError{core::ErrorCode::IoFailure, "release manifest temporary file could not be opened"};
        output << manifest.toJson().dump(2) << '\n';
        output.flush();
        if (!output.good()) {
            output.close();
            std::filesystem::remove(temporary, ec);
            return core::AppError{core::ErrorCode::IoFailure, "release manifest temporary file could not be written"};
        }
    }
    std::filesystem::rename(temporary, manifestPath, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return core::AppError{core::ErrorCode::IoFailure, "release manifest could not be atomically published"};
    }
    return core::ok();
}

core::Result<ReleaseManifest> ReleaseManifestStore::read(const std::filesystem::path& manifestPath,
                                                          const std::filesystem::path& artifactRoot) const {
    std::ifstream input{manifestPath, std::ios::binary};
    if (!input) return core::AppError{core::ErrorCode::NotFound, "release manifest does not exist"};
    std::stringstream contents;
    contents << input.rdbuf();
    try {
        const auto manifest = ReleaseManifest::fromJson(nlohmann::json::parse(contents.str()));
        if (!manifest.hasValue()) return manifest.error();
        const auto verified = verifyArtifacts(artifactRoot, manifest.value());
        if (!verified.hasValue()) return verified.error();
        return manifest.value();
    } catch (const std::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure, "release manifest is not valid JSON"};
    }
}

}  // namespace creator::platform_release
