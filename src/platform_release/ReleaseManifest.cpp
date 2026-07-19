#include "platform_release/ReleaseManifest.h"

#include "core/AppError.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace creator::platform_release {
namespace {

bool hasTextToken(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::none_of(value.begin(), value.end(), [](unsigned char character) {
               return std::isspace(character) != 0 || std::iscntrl(character) != 0;
           });
}

bool isSha256(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isdigit(character) != 0 || (character >= 'a' && character <= 'f');
    });
}

bool isRelativeArtifactPath(std::string_view value) {
    if (value.empty() || value.find('\\') != std::string_view::npos) return false;
    const std::filesystem::path path{std::string{value}};
    if (path.is_absolute()) return false;
    for (const auto& part : path) {
        if (part == ".." || part == ".") return false;
    }
    return path.generic_string() == value;
}

}  // namespace

core::Result<ReleaseManifest> ReleaseManifest::create(
    std::string productVersion, std::string sourceRevision, std::string target,
    std::vector<ReleaseArtifact> artifacts) {
    if (!hasTextToken(productVersion) || !hasTextToken(sourceRevision) || !hasTextToken(target)) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "release manifest version, revision, and target must be non-empty tokens"};
    }
    if (artifacts.empty()) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "release manifest must contain at least one artifact"};
    }
    std::unordered_set<std::string> paths;
    for (const auto& artifact : artifacts) {
        if (!isRelativeArtifactPath(artifact.relativePath) || !isSha256(artifact.sha256)) {
            return core::AppError{core::ErrorCode::InvalidArgument,
                                  "release artifact path or SHA-256 is invalid"};
        }
        if (!paths.insert(artifact.relativePath).second) {
            return core::AppError{core::ErrorCode::AlreadyExists,
                                  "release artifact paths must be unique"};
        }
    }
    std::sort(artifacts.begin(), artifacts.end(), [](const ReleaseArtifact& left,
                                                     const ReleaseArtifact& right) {
        return left.relativePath < right.relativePath;
    });
    return ReleaseManifest{std::move(productVersion), std::move(sourceRevision),
                           std::move(target), std::move(artifacts)};
}

nlohmann::json ReleaseManifest::toJson() const {
    nlohmann::json artifacts = nlohmann::json::array();
    for (const auto& artifact : artifacts_) {
        artifacts.push_back({{"path", artifact.relativePath}, {"sha256", artifact.sha256}});
    }
    return {{"schemaVersion", 1}, {"productVersion", productVersion_},
            {"sourceRevision", sourceRevision_}, {"target", target_},
            {"artifacts", std::move(artifacts)}};
}

core::Result<ReleaseManifest> ReleaseManifest::fromJson(const nlohmann::json& document) {
    try {
        if (!document.is_object() || document.at("schemaVersion") != 1) {
            return core::AppError{core::ErrorCode::UnsupportedVersion,
                                  "release manifest schema version is unsupported"};
        }
        std::vector<ReleaseArtifact> artifacts;
        for (const auto& artifact : document.at("artifacts")) {
            artifacts.push_back({artifact.at("path").get<std::string>(),
                                 artifact.at("sha256").get<std::string>()});
        }
        return create(document.at("productVersion").get<std::string>(),
                      document.at("sourceRevision").get<std::string>(),
                      document.at("target").get<std::string>(), std::move(artifacts));
    } catch (const std::exception&) {
        return core::AppError{core::ErrorCode::ParseFailure,
                              "release manifest is not valid structured metadata"};
    }
}

}  // namespace creator::platform_release
