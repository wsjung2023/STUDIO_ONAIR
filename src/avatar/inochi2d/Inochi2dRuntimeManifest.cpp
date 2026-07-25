#include "avatar/inochi2d/Inochi2dRuntimeManifest.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace creator::avatar::inochi2d {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::string_view kVersion = "0.8.7-nightly+66fa768";
constexpr std::string_view kSourceCommit =
    "66fa76834b28037db0c871c656563422f697879e";
constexpr std::string_view kArchiveSha256 =
    "79f1f51641380ac992b5ecca2ab49245f111517ca4185ca832ffb0460f6cd4fb";
constexpr std::string_view kNoticeSha256 =
    "f79f6e26fa823e5c1881490bfee86627de43fc461ddeab4d80dc7af87cfc1743";

constexpr std::array<std::string_view, 16> kRequiredSymbols{
    "in_puppet_load",
    "in_puppet_free",
    "in_puppet_get_parameters",
    "in_parameter_get_name",
    "in_parameter_get_dimensions",
    "in_parameter_set_value",
    "in_puppet_update",
    "in_puppet_draw",
    "in_puppet_get_drawlist",
    "in_drawlist_get_commands",
    "in_drawlist_get_vertex_data",
    "in_drawlist_get_index_data",
    "in_texture_get_width",
    "in_texture_get_height",
    "in_texture_get_channels",
    "in_texture_get_pixels",
};

struct DependencyIdentity final {
    std::string_view name;
    std::string_view version;
    std::string_view archiveSha256;
};

constexpr std::array<DependencyIdentity, 6>
    kPinnedDependencies{{
        {"imagefmt", "2.1.2",
         "10f4182efc4fc3846561ca702b3207493f736639498b2dc61a3adcee2bb18736"},
        {"inmath", "1.3.0",
         "865fa85d6c07c5f23207cdf9987207d95547e8303009cd0a028b8e7aa9d5aeae"},
        {"intel-intrinsics", "1.12.1",
         "4e056612b6ebe819fef2e45c19d78427b7e67b3c8650445e7379ed2b30f61519"},
        {"nulib", "0.3.5",
         "e4b56c28cd3264c72ba18e21889b9dddd1927b83828d92ebe6b49d559b22e597"},
        {"numem", "1.3.2",
         "771688ea0ac4990e8576de4cdcdb381449d78d9edf7a6a7d55adeccfe46d94cc"},
        {"silly", "1.1.1",
         "ffb78e740db5ab36c216c349ec36548a91c66fd1b69b980c1fd3e912ce8ae73b"},
    }};

struct PlatformIdentity final {
    std::string_view target;
    std::string_view triple;
    std::string_view minimumPlatform;
    std::string_view libraryPath;
};

constexpr PlatformIdentity currentPlatform() {
#if defined(_WIN32) && defined(_M_X64)
    return {"windows-x64", "x86_64-pc-windows-msvc", "windows-10-1809",
            "bin/inochi2d.dll"};
#elif defined(__APPLE__) && defined(__aarch64__)
    return {"macos-arm64", "arm64-apple-darwin", "macos-13.0",
            "lib/libinochi2d.dylib"};
#elif defined(__ANDROID__) && defined(__aarch64__)
    return {"android-arm64", "aarch64-linux-android26", "android-api-26",
            "lib/libinochi2d.so"};
#else
    return {};
#endif
}

AppError invalid(std::string message) {
    return AppError{ErrorCode::InvalidState, std::move(message)};
}

AppError unsupported(std::string message) {
    return AppError{ErrorCode::UnsupportedVersion, std::move(message)};
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

std::string fromUtf8String(const std::u8string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char8_t value : text) result.push_back(static_cast<char>(value));
    return result;
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const unsigned char value : text) {
        encoded.push_back(static_cast<char8_t>(value));
    }
    return std::filesystem::path{encoded};
}

std::string keyFor(std::string value) {
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    return value;
}

bool isLowerHexSha256(const std::string& value) {
    return value.size() == 64U &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}

Result<void> validateRelativePath(const std::string& text) {
    if (text.empty() || text.find('\\') != std::string::npos) {
        return invalid("Inochi2D manifest contains an invalid path");
    }
    const auto path = pathFromUtf8(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory() ||
        fromUtf8String(path.lexically_normal().generic_u8string()) != text) {
        return invalid("Inochi2D manifest path escapes its runtime root");
    }
    for (const auto& component : path) {
        if (component == "." || component == "..") {
            return invalid("Inochi2D manifest path escapes its runtime root");
        }
    }
    return core::ok();
}

Result<std::vector<std::uint8_t>> readPrefix(
    const std::filesystem::path& path, std::size_t bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return AppError{ErrorCode::IoFailure,
                        "Could not read the Inochi2D runtime library"};
    }
    std::vector<std::uint8_t> result(bytes);
    input.read(reinterpret_cast<char*>(result.data()),
               static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(input.gcount()));
    return result;
}

bool isExpectedArchitecture(const std::filesystem::path& library,
                            std::string_view target) {
    auto prefixResult = readPrefix(library, 512U);
    if (!prefixResult.hasValue()) return false;
    const auto& bytes = prefixResult.value();
    if (target == "windows-x64") {
        if (bytes.size() < 0x46U || bytes[0] != 'M' || bytes[1] != 'Z') {
            return false;
        }
        const auto peOffset =
            static_cast<std::uint32_t>(bytes[0x3c]) |
            (static_cast<std::uint32_t>(bytes[0x3d]) << 8U) |
            (static_cast<std::uint32_t>(bytes[0x3e]) << 16U) |
            (static_cast<std::uint32_t>(bytes[0x3f]) << 24U);
        if (peOffset > bytes.size() - 6U) return false;
        return bytes[peOffset] == 'P' && bytes[peOffset + 1U] == 'E' &&
               bytes[peOffset + 2U] == 0 && bytes[peOffset + 3U] == 0 &&
               bytes[peOffset + 4U] == 0x64 && bytes[peOffset + 5U] == 0x86;
    }
    if (target == "macos-arm64") {
        return bytes.size() >= 8U && bytes[0] == 0xcf && bytes[1] == 0xfa &&
               bytes[2] == 0xed && bytes[3] == 0xfe && bytes[4] == 0x0c &&
               bytes[5] == 0x00 && bytes[6] == 0x00 && bytes[7] == 0x01;
    }
    if (target == "android-arm64") {
        return bytes.size() >= 20U && bytes[0] == 0x7f && bytes[1] == 'E' &&
               bytes[2] == 'L' && bytes[3] == 'F' && bytes[4] == 2 &&
               bytes[5] == 1 && bytes[18] == 0xb7 && bytes[19] == 0;
    }
    return false;
}

}  // namespace

Result<Inochi2dRuntimeInfo> Inochi2dRuntimeManifest::loadAndVerify(
    const std::filesystem::path& runtimeRoot) {
    const auto platform = currentPlatform();
    if (platform.target.empty()) {
        return unsupported(
            "This process target has no audited Inochi2D runtime");
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, error);
    const bool rootIsDirectory =
        !error && std::filesystem::is_directory(root, error);
    if (error || !rootIsDirectory || isReparsePoint(root)) {
        return invalid("Inochi2D runtime root is missing or redirected");
    }
    const auto manifestPath = root / "runtime-manifest.json";
    const bool manifestIsFile =
        std::filesystem::is_regular_file(manifestPath, error);
    if (error || !manifestIsFile || isReparsePoint(manifestPath)) {
        return AppError{ErrorCode::NotFound,
                        "Inochi2D runtime manifest is missing"};
    }

    std::string libraryRelative;
    std::string librarySha256;
    std::string noticeRelative;
    std::string noticeSha256;
    std::string thirdPartyNoticesRelative;
    std::string thirdPartyNoticesSha256;
    std::vector<std::pair<std::string, std::string>> runtimeDependencies;
    Inochi2dRuntimeInfo info;
    try {
        std::ifstream input(manifestPath, std::ios::binary);
        const auto manifest = nlohmann::json::parse(input);
        if (manifest.at("schema_version").get<int>() != 1 ||
            manifest.at("component").get<std::string>() != "Inochi2D C-FFI" ||
            manifest.at("version").get<std::string>() != kVersion ||
            manifest.at("source_commit").get<std::string>() != kSourceCommit ||
            manifest.at("source_archive_sha256").get<std::string>() !=
                kArchiveSha256 ||
            manifest.at("license").get<std::string>() != "BSD-2-Clause" ||
            manifest.at("linking").get<std::string>() != "dynamic" ||
            manifest.at("target").get<std::string>() != platform.target ||
            manifest.at("target_triple").get<std::string>() != platform.triple ||
            manifest.at("minimum_platform").get<std::string>() !=
                platform.minimumPlatform ||
            manifest.at("abi_mode").get<std::string>() != "IN_VEC2_POSITION") {
            return unsupported("Inochi2D runtime identity is not approved");
        }

        info.version = manifest.at("version").get<std::string>();
        info.sourceCommit = manifest.at("source_commit").get<std::string>();
        info.target = manifest.at("target").get<std::string>();
        info.targetTriple = manifest.at("target_triple").get<std::string>();
        info.minimumPlatform =
            manifest.at("minimum_platform").get<std::string>();
        info.compilerIdentity = manifest.at("compiler").get<std::string>();
        info.sdkIdentity = manifest.at("sdk").get<std::string>();
        if (info.compilerIdentity.empty() || info.sdkIdentity.empty()) {
            return invalid(
                "Inochi2D runtime omits compiler or SDK identity");
        }
        const auto& dependencies = manifest.at("dependencies");
        if (!dependencies.is_object() ||
            dependencies.size() != kPinnedDependencies.size()) {
            return unsupported(
                "Inochi2D runtime dependency set is not approved");
        }
        for (const auto& dependency : kPinnedDependencies) {
            const auto& value = dependencies.at(dependency.name);
            if (!value.is_object() || value.size() != 2U ||
                value.at("version").get<std::string>() != dependency.version ||
                value.at("archive_sha256").get<std::string>() !=
                    dependency.archiveSha256) {
                return unsupported(
                    "Inochi2D runtime dependency set is not approved");
            }
        }
        const auto& runtimeDependencyJson =
            manifest.at("runtime_dependencies");
        if (!runtimeDependencyJson.is_array()) {
            return unsupported(
                "Inochi2D runtime dependency closure is not approved");
        }
        if (platform.target == "windows-x64") {
            const std::unordered_set<std::string> expectedRuntimeDependencies{
                "bin/druntime-ldc-shared.dll",
                "bin/phobos2-ldc-shared.dll"};
            if (runtimeDependencyJson.size() !=
                expectedRuntimeDependencies.size()) {
                return unsupported(
                    "Inochi2D runtime dependency closure is not approved");
            }
            std::unordered_set<std::string> seen;
            for (const auto& value : runtimeDependencyJson) {
                if (!value.is_object() || value.size() != 3U ||
                    value.at("component").get<std::string>() !=
                        "LDC 1.40.0 BSL-1.0 runtime") {
                    return unsupported(
                        "Inochi2D runtime dependency closure is not approved");
                }
                auto relative = value.at("path").get<std::string>();
                auto sha256 = value.at("sha256").get<std::string>();
                if (!expectedRuntimeDependencies.contains(relative) ||
                    !seen.emplace(relative).second ||
                    !isLowerHexSha256(sha256)) {
                    return unsupported(
                        "Inochi2D runtime dependency closure is not approved");
                }
                if (auto valid = validateRelativePath(relative);
                    !valid.hasValue()) {
                    return valid.error();
                }
                runtimeDependencies.emplace_back(
                    std::move(relative), std::move(sha256));
            }
        } else if (!runtimeDependencyJson.empty()) {
            return unsupported(
                "Inochi2D runtime dependency closure is not approved");
        }

        libraryRelative = manifest.at("library").at("path").get<std::string>();
        librarySha256 =
            manifest.at("library").at("sha256").get<std::string>();
        noticeRelative = manifest.at("notice").at("path").get<std::string>();
        noticeSha256 = manifest.at("notice").at("sha256").get<std::string>();
        thirdPartyNoticesRelative =
            manifest.at("third_party_notices").at("path").get<std::string>();
        thirdPartyNoticesSha256 =
            manifest.at("third_party_notices").at("sha256").get<std::string>();
        if (libraryRelative != platform.libraryPath ||
            noticeRelative != "LICENSE" || !isLowerHexSha256(librarySha256) ||
            noticeSha256 != kNoticeSha256 ||
            thirdPartyNoticesRelative != "THIRD_PARTY_NOTICES.txt" ||
            !isLowerHexSha256(thirdPartyNoticesSha256)) {
            return unsupported(
                "Inochi2D runtime artifact identity is not approved");
        }
        if (auto valid = validateRelativePath(libraryRelative);
            !valid.hasValue()) {
            return valid.error();
        }
        if (auto valid = validateRelativePath(noticeRelative);
            !valid.hasValue()) {
            return valid.error();
        }
        if (auto valid = validateRelativePath(thirdPartyNoticesRelative);
            !valid.hasValue()) {
            return valid.error();
        }

        if (!manifest.at("symbols").is_array()) {
            return unsupported(
                "Inochi2D runtime symbol list is not approved");
        }
        std::unordered_set<std::string> symbols;
        for (const auto& entry : manifest.at("symbols")) {
            if (!symbols.emplace(entry.get<std::string>()).second) {
                return unsupported(
                    "Inochi2D runtime symbol list is not approved");
            }
        }
        if (symbols.size() != kRequiredSymbols.size()) {
            return unsupported(
                "Inochi2D runtime symbol list is not approved");
        }
        for (const auto required : kRequiredSymbols) {
            if (!symbols.contains(std::string{required})) {
                return unsupported(
                    "Inochi2D runtime is missing a required C FFI symbol");
            }
            info.requiredSymbols.emplace_back(required);
        }
    } catch (const nlohmann::json::exception&) {
        return AppError{ErrorCode::ParseFailure,
                        "Inochi2D runtime manifest is invalid"};
    }

    const auto library = root / pathFromUtf8(libraryRelative);
    const auto notice = root / pathFromUtf8(noticeRelative);
    const auto thirdPartyNotices =
        root / pathFromUtf8(thirdPartyNoticesRelative);
    std::unordered_map<std::string, std::filesystem::path> actualFiles;
    std::filesystem::recursive_directory_iterator iterator{
        root, std::filesystem::directory_options::none, error};
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        const auto path = iterator->path();
        if (isReparsePoint(path)) {
            return invalid(
                "Inochi2D runtime contains a redirected artifact");
        }
        if (iterator->is_regular_file(error) && !error) {
            const auto relative =
                fromUtf8String(path.lexically_relative(root).generic_u8string());
            if (auto valid = validateRelativePath(relative); !valid.hasValue()) {
                return valid.error();
            }
            if (!actualFiles.emplace(keyFor(relative), path).second) {
                return invalid(
                    "Inochi2D runtime contains duplicate artifacts");
            }
        }
        iterator.increment(error);
    }
    if (error) {
        return invalid("Could not enumerate the Inochi2D runtime");
    }
    const std::unordered_set<std::string> expectedFiles{
        keyFor("runtime-manifest.json"), keyFor(libraryRelative),
        keyFor(noticeRelative), keyFor(thirdPartyNoticesRelative)};
    auto completeExpectedFiles = expectedFiles;
    for (const auto& [relative, unusedHash] : runtimeDependencies) {
        static_cast<void>(unusedHash);
        completeExpectedFiles.emplace(keyFor(relative));
    }
    if (actualFiles.size() != completeExpectedFiles.size() ||
        std::any_of(actualFiles.begin(), actualFiles.end(),
                    [&](const auto& entry) {
                        return !completeExpectedFiles.contains(entry.first);
                    })) {
        return invalid(
            "Inochi2D runtime contains an unexpected staged artifact");
    }

    if (isReparsePoint(library) || isReparsePoint(notice) ||
        isReparsePoint(thirdPartyNotices)) {
        return invalid("Inochi2D runtime contains a redirected artifact");
    }
    auto actualLibraryHash = core::sha256File(library);
    if (!actualLibraryHash.hasValue()) return actualLibraryHash.error();
    if (actualLibraryHash.value() != librarySha256) {
        return AppError{ErrorCode::IoFailure,
                        "Inochi2D runtime library hash does not match"};
    }
    auto actualNoticeHash = core::sha256File(notice);
    if (!actualNoticeHash.hasValue()) return actualNoticeHash.error();
    if (actualNoticeHash.value() != noticeSha256) {
        return AppError{ErrorCode::IoFailure,
                        "Inochi2D notice hash does not match"};
    }
    auto actualThirdPartyNoticesHash = core::sha256File(thirdPartyNotices);
    if (!actualThirdPartyNoticesHash.hasValue()) {
        return actualThirdPartyNoticesHash.error();
    }
    if (actualThirdPartyNoticesHash.value() != thirdPartyNoticesSha256) {
        return AppError{ErrorCode::IoFailure,
                        "Inochi2D third-party notices hash does not match"};
    }
    for (const auto& [relative, expectedHash] : runtimeDependencies) {
        const auto path = root / pathFromUtf8(relative);
        if (isReparsePoint(path)) {
            return invalid(
                "Inochi2D runtime contains a redirected artifact");
        }
        auto actualHash = core::sha256File(path);
        if (!actualHash.hasValue()) return actualHash.error();
        if (actualHash.value() != expectedHash) {
            return AppError{
                ErrorCode::IoFailure,
                "Inochi2D runtime dependency hash does not match"};
        }
    }
    if (!isExpectedArchitecture(library, platform.target)) {
        return unsupported(
            "Inochi2D runtime library architecture is not approved");
    }

    info.libraryPath = std::filesystem::weakly_canonical(library, error);
    if (error) {
        return AppError{ErrorCode::IoFailure,
                        "Could not resolve the Inochi2D runtime library"};
    }
    info.librarySha256 = std::move(librarySha256);
    return info;
}

}  // namespace creator::avatar::inochi2d
