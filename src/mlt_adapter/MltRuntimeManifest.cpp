#include "mlt_adapter/MltRuntimeManifest.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace creator::mlt_adapter {
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

bool isForbiddenArtifact(const std::string& path) {
    std::string lower = asciiLower(path);
    const auto slash = lower.find_last_of('/');
    const std::string name = lower.substr(slash == std::string::npos ? 0 : slash + 1);
    return name.ends_with(".exe") || name == "melt" || name == "melt.exe" ||
           name.find("plusgpl") != std::string::npos ||
           name.find("rubberband") != std::string::npos ||
           name.find("vid.stab") != std::string::npos ||
           name.find("xine") != std::string::npos;
}

struct FileProvenance final {
    std::string_view component;
    std::string_view version;
    std::string_view sourceIdentity;
    std::string_view license;
};

constexpr std::string_view kVcpkgIdentity =
    "vcpkg:43643e1f5cf73db40d0d4bd610183348eb09b24e";
constexpr std::string_view kFfmpegIdentity =
    "sha256:464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c";

std::optional<FileProvenance> expectedProvenance(const std::string& path) {
    const auto lower = asciiLower(path);
    const auto nameOffset = lower.find_last_of('/');
    const auto name = lower.substr(
        nameOffset == std::string::npos ? 0 : nameOffset + 1);
    if (name.starts_with("avcodec-") || name.starts_with("avfilter-") ||
        name.starts_with("avformat-") || name.starts_with("avutil-") ||
        name.starts_with("swresample-") || name.starts_with("swscale-")) {
        return FileProvenance{"FFmpeg", "8.1.2", kFfmpegIdentity,
                              "LGPL-2.1-or-later"};
    }
    if (name == "z.dll") {
        return FileProvenance{"zlib", "1.3.2", kVcpkgIdentity, "Zlib"};
    }
    if (name.starts_with("pthread") ||
        lower.starts_with("include/mlt-deps/")) {
        return FileProvenance{"PThreads4W", "3.0.0", kVcpkgIdentity,
                              "Apache-2.0"};
    }
    if (name == "iconv-2.dll" || name == "libiconv-2.dll") {
        return FileProvenance{"GNU libiconv", "1.19", kVcpkgIdentity,
                              "LGPL-2.1-or-later"};
    }
    if (name == "dl.dll") {
        return FileProvenance{"dlfcn-win32", "1.4.2", kVcpkgIdentity,
                              "MIT"};
    }
    if (lower == "creator-studio-mlt-build.txt") {
        return FileProvenance{"Creator Studio", "1", "repository:R1-03",
                              "LicenseRef-Creator-Studio-Proprietary"};
    }
    if (lower.starts_with("bin/mlt") || lower.starts_with("lib/") ||
        lower.starts_with("share/") || lower.starts_with("include/mlt-7/")) {
        return FileProvenance{"MLT Framework", CS_MLT_EXPECTED_VERSION,
                              CS_MLT_EXPECTED_COMMIT, "LGPL-2.1-or-later"};
    }
    return std::nullopt;
}

nlohmann::json expectedDependencies() {
    return nlohmann::json::array({
        {{"component", "FFmpeg"},
         {"version", "8.1.2"},
         {"source_identity", kFfmpegIdentity},
         {"license", "LGPL-2.1-or-later"}},
        {{"component", "zlib"},
         {"version", "1.3.2"},
         {"source_identity", kVcpkgIdentity},
         {"license", "Zlib"}},
        {{"component", "PThreads4W"},
         {"version", "3.0.0"},
         {"source_identity", kVcpkgIdentity},
         {"license", "Apache-2.0"}},
        {{"component", "GNU libiconv"},
         {"version", "1.19"},
         {"source_identity", kVcpkgIdentity},
         {"license", "LGPL-2.1-or-later"}},
        {{"component", "dlfcn-win32"},
         {"version", "1.4.2"},
         {"source_identity", kVcpkgIdentity},
         {"license", "MIT"}},
    });
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
        return invalid("MLT manifest contains an invalid path");
    }
    const std::filesystem::path path = pathFromUtf8(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        fromUtf8String(path.lexically_normal().generic_u8string()) != text) {
        return invalid("MLT manifest contains a path outside its runtime root");
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            return invalid("MLT manifest contains a path outside its runtime root");
        }
    }
    return core::ok();
}

}  // namespace

Result<void> verifyMltRuntimeManifest(
    const std::filesystem::path& runtimeRoot) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, error);
    const bool rootIsDirectory =
        !error && std::filesystem::is_directory(root, error);
    if (error || !rootIsDirectory || isReparsePoint(root)) {
        return invalid("MLT runtime root is missing or redirected");
    }
    const auto manifestPath = root / "mlt-runtime-manifest.json";
    const bool manifestIsFile =
        std::filesystem::is_regular_file(manifestPath, error);
    if (error || !manifestIsFile ||
        isReparsePoint(manifestPath)) {
        return AppError{ErrorCode::NotFound, "MLT runtime manifest is missing"};
    }

    nlohmann::json manifest;
    try {
        std::ifstream input(manifestPath, std::ios::binary);
        manifest = nlohmann::json::parse(input);
        if (manifest.at("abi").get<int>() != 1 ||
            manifest.at("component").get<std::string>() != "MLT Framework" ||
            manifest.at("version").get<std::string>() != CS_MLT_EXPECTED_VERSION ||
            manifest.at("source_commit").get<std::string>() !=
                CS_MLT_EXPECTED_COMMIT ||
            manifest.at("linking").get<std::string>() != "dynamic" ||
            manifest.at("allowed_modules") !=
                nlohmann::json::array({"core", "avformat"}) ||
            manifest.at("dependencies") != expectedDependencies() ||
            !manifest.at("files").is_array()) {
            return AppError{ErrorCode::UnsupportedVersion,
                            "MLT runtime identity is not approved"};
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "MLT runtime manifest is invalid"};
    }

    std::unordered_map<std::string, std::string> expected;
    const std::unordered_set<std::string> allowedRoles{
        "runtime-library", "runtime-module", "runtime-data", "development",
        "evidence"};
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
                return invalid("MLT manifest contains invalid file metadata");
            }
            const auto provenance = expectedProvenance(path);
            if (!provenance || component != provenance->component ||
                version != provenance->version ||
                sourceIdentity != provenance->sourceIdentity ||
                license != provenance->license) {
                return invalid(
                    "MLT manifest contains unapproved file provenance");
            }
            if (isForbiddenArtifact(path)) {
                return invalid("MLT manifest contains a forbidden artifact");
            }
            if (!expected.emplace(keyFor(path), hash).second) {
                return invalid("MLT manifest contains a duplicate path");
            }
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "MLT runtime manifest file list is invalid"};
    }

    for (const auto* required : {"bin/mlt-7.dll", "bin/mlt++-7.dll",
                                 "bin/z.dll",
                                 "lib/mlt-7/mltcore.dll",
                                 "lib/mlt-7/mltavformat.dll"}) {
        if (!expected.contains(keyFor(required))) {
            return invalid("MLT runtime is missing a required approved component");
        }
    }

    std::unordered_map<std::string, std::filesystem::path> actual;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::none, error};
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto path = iterator->path();
        if (isReparsePoint(path)) {
            return invalid("MLT runtime contains a redirected artifact");
        }
        if (iterator->is_regular_file(error) && !error && path != manifestPath) {
            const auto relative = fromUtf8String(
                path.lexically_relative(root).generic_u8string());
            if (auto valid = validateRelativePath(relative); !valid.hasValue()) {
                return valid;
            }
            if (isForbiddenArtifact(relative)) {
                return invalid("MLT runtime contains a forbidden artifact");
            }
            if (!actual.emplace(keyFor(relative), path).second) {
                return invalid("MLT runtime contains duplicate artifacts");
            }
        }
        iterator.increment(error);
    }
    if (error) return invalid("Could not enumerate the MLT runtime");
    if (actual.size() != expected.size()) {
        return invalid("MLT runtime file set does not match its manifest");
    }
    for (const auto& [relative, expectedHash] : expected) {
        const auto found = actual.find(relative);
        if (found == actual.end()) {
            return invalid("MLT runtime artifact is missing");
        }
        auto actualHash = core::sha256File(found->second);
        if (!actualHash.hasValue()) return actualHash.error();
        if (actualHash.value() != expectedHash) {
            return invalid("MLT runtime artifact hash does not match its manifest");
        }
    }
    return core::ok();
}

}  // namespace creator::mlt_adapter
