#include "rnnoise_adapter/RnnoiseRuntimeManifest.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::rnnoise_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError invalid(std::string message) {
    return AppError{ErrorCode::InvalidState, std::move(message)};
}

bool isReparsePoint(const std::filesystem::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
#endif
}

std::string keyFor(std::string path) {
#ifdef _WIN32
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return path;
}

std::string asciiLower(std::string path) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return path;
}

std::string fromUtf8String(const std::u8string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char8_t value : text) result.push_back(static_cast<char>(value));
    return result;
}

bool isLowerHexSha256(const std::string& text) {
    return text.size() == 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char value) {
               return std::isdigit(value) != 0 ||
                      (value >= static_cast<unsigned char>('a') &&
                       value <= static_cast<unsigned char>('f'));
           });
}

// A denoiser prefix carries only a static lib, a header and text evidence.
// Anything that looks like a loadable binary, or names GPL, must never appear.
bool isForbiddenArtifact(const std::string& path) {
    const std::string lower = asciiLower(path);
    return lower.ends_with(".exe") || lower.ends_with(".dll") ||
           lower.ends_with(".so") || lower.ends_with(".dylib") ||
           lower.find("gpl") != std::string::npos;
}

struct FileProvenance final {
    std::string_view component;
    std::string_view version;
    std::string_view sourceIdentity;
    std::string_view license;
};

std::optional<FileProvenance> expectedProvenance(const std::string& path) {
    const std::string lower = asciiLower(path);
    if (lower == "creator-studio-rnnoise-build.txt") {
        return FileProvenance{"Creator Studio", "1", "repository:R2-audio-dsp",
                              "LicenseRef-Creator-Studio-Proprietary"};
    }
    if (lower == "lib/rnnoise.lib" || lower == "include/rnnoise.h") {
        return FileProvenance{"RNNoise", CS_RNNOISE_EXPECTED_VERSION,
                              CS_RNNOISE_EXPECTED_COMMIT, "BSD-3-Clause"};
    }
    return std::nullopt;
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const unsigned char value : text) {
        encoded.push_back(static_cast<char8_t>(value));
    }
    return std::filesystem::path{encoded};
}

Result<void> validateRelativePath(const std::string& text) {
    if (text.empty() || text.find('\\') != std::string::npos) {
        return invalid("RNNoise manifest contains an invalid path");
    }
    const std::filesystem::path path = pathFromUtf8(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        fromUtf8String(path.lexically_normal().generic_u8string()) != text) {
        return invalid("RNNoise manifest contains a path outside its runtime root");
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            return invalid(
                "RNNoise manifest contains a path outside its runtime root");
        }
    }
    return core::ok();
}

}  // namespace

Result<void> verifyRnnoiseRuntimeManifest(
    const std::filesystem::path& runtimeRoot) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, error);
    const bool rootIsDirectory =
        !error && std::filesystem::is_directory(root, error);
    if (error || !rootIsDirectory || isReparsePoint(root)) {
        return invalid("RNNoise runtime root is missing or redirected");
    }
    const auto manifestPath = root / "rnnoise-runtime-manifest.json";
    const bool manifestIsFile =
        std::filesystem::is_regular_file(manifestPath, error);
    if (error || !manifestIsFile || isReparsePoint(manifestPath)) {
        return AppError{ErrorCode::NotFound, "RNNoise runtime manifest is missing"};
    }

    nlohmann::json manifest;
    try {
        std::ifstream input(manifestPath, std::ios::binary);
        manifest = nlohmann::json::parse(input);
        if (manifest.at("abi").get<int>() != 1 ||
            manifest.at("component").get<std::string>() != "RNNoise" ||
            manifest.at("version").get<std::string>() !=
                CS_RNNOISE_EXPECTED_VERSION ||
            manifest.at("source_commit").get<std::string>() !=
                CS_RNNOISE_EXPECTED_COMMIT ||
            manifest.at("source_archive_sha256").get<std::string>() !=
                CS_RNNOISE_EXPECTED_ARCHIVE_SHA256 ||
            manifest.at("linking").get<std::string>() != "static" ||
            manifest.at("license").get<std::string>() != "BSD-3-Clause" ||
            !manifest.at("files").is_array()) {
            return AppError{ErrorCode::UnsupportedVersion,
                            "RNNoise runtime identity is not approved"};
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "RNNoise runtime manifest is invalid"};
    }

    std::unordered_map<std::string, std::string> expected;
    const std::unordered_set<std::string> allowedRoles{"development", "evidence"};
    try {
        for (const auto& entry : manifest.at("files")) {
            const auto path = entry.at("path").get<std::string>();
            const auto hash = entry.at("sha256").get<std::string>();
            const auto role = entry.at("role").get<std::string>();
            const auto component = entry.at("component").get<std::string>();
            const auto version = entry.at("version").get<std::string>();
            const auto sourceIdentity =
                entry.at("source_identity").get<std::string>();
            const auto license = entry.at("license").get<std::string>();
            if (auto valid = validateRelativePath(path); !valid.hasValue()) {
                return valid;
            }
            if (!isLowerHexSha256(hash) || !allowedRoles.contains(role)) {
                return invalid("RNNoise manifest contains invalid file metadata");
            }
            const auto provenance = expectedProvenance(path);
            if (!provenance || component != provenance->component ||
                version != provenance->version ||
                sourceIdentity != provenance->sourceIdentity ||
                license != provenance->license) {
                return invalid(
                    "RNNoise manifest contains unapproved file provenance");
            }
            if (isForbiddenArtifact(path)) {
                return invalid("RNNoise manifest contains a forbidden artifact");
            }
            if (!expected.emplace(keyFor(path), hash).second) {
                return invalid("RNNoise manifest contains a duplicate path");
            }
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "RNNoise runtime manifest file list is invalid"};
    }

    for (const auto* required : {"lib/rnnoise.lib", "include/rnnoise.h"}) {
        if (!expected.contains(keyFor(required))) {
            return invalid(
                "RNNoise runtime is missing a required approved component");
        }
    }

    std::unordered_map<std::string, std::filesystem::path> actual;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::none, error};
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto path = iterator->path();
        if (isReparsePoint(path)) {
            return invalid("RNNoise runtime contains a redirected artifact");
        }
        if (iterator->is_regular_file(error) && !error && path != manifestPath) {
            const auto relative = fromUtf8String(
                path.lexically_relative(root).generic_u8string());
            if (auto valid = validateRelativePath(relative); !valid.hasValue()) {
                return valid;
            }
            if (isForbiddenArtifact(relative)) {
                return invalid("RNNoise runtime contains a forbidden artifact");
            }
            if (!actual.emplace(keyFor(relative), path).second) {
                return invalid("RNNoise runtime contains duplicate artifacts");
            }
        }
        iterator.increment(error);
    }
    if (error) return invalid("Could not enumerate the RNNoise runtime");
    if (actual.size() != expected.size()) {
        return invalid("RNNoise runtime file set does not match its manifest");
    }
    for (const auto& [relative, expectedHash] : expected) {
        const auto found = actual.find(relative);
        if (found == actual.end()) {
            return invalid("RNNoise runtime artifact is missing");
        }
        auto actualHash = core::sha256File(found->second);
        if (!actualHash.hasValue()) return actualHash.error();
        if (actualHash.value() != expectedHash) {
            return invalid(
                "RNNoise runtime artifact hash does not match its manifest");
        }
    }
    return core::ok();
}

}  // namespace creator::rnnoise_adapter
