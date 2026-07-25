#include "avatar/inochi2d/Inochi2dRuntimeManifest.h"

#include "avatar/inochi2d/Inochi2dBinaryInspector.h"
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

bool isApprovedWindowsImport(std::string_view name) {
    static const std::unordered_set<std::string_view> systemLibraries{
        "advapi32.dll",   "comctl32.dll",  "kernel32.dll",
        "msvfw32.dll",    "shell32.dll",   "shlwapi.dll",
        "user32.dll",     "vcruntime140.dll",
        "ws2_32.dll",     "druntime-ldc-shared.dll",
        "phobos2-ldc-shared.dll"};
    return systemLibraries.contains(name) ||
           (name.starts_with("api-ms-win-") && name.ends_with(".dll"));
}

}  // namespace

class Inochi2dVerifiedRuntime::Impl final {
public:
    ~Impl() {
#ifdef _WIN32
        if (module != nullptr) FreeLibrary(module);
        for (const auto handle : fileHandles) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        }
        if (rootHandle != INVALID_HANDLE_VALUE) CloseHandle(rootHandle);
#endif
    }

    Inochi2dRuntimeInfo runtimeInfo;
#ifdef _WIN32
    HANDLE rootHandle{INVALID_HANDLE_VALUE};
    std::vector<HANDLE> fileHandles;
    HMODULE module{nullptr};
#endif
};

Inochi2dVerifiedRuntime::Inochi2dVerifiedRuntime(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
Inochi2dVerifiedRuntime::~Inochi2dVerifiedRuntime() = default;
Inochi2dVerifiedRuntime::Inochi2dVerifiedRuntime(
    Inochi2dVerifiedRuntime&&) noexcept = default;
Inochi2dVerifiedRuntime& Inochi2dVerifiedRuntime::operator=(
    Inochi2dVerifiedRuntime&&) noexcept = default;

const Inochi2dRuntimeInfo& Inochi2dVerifiedRuntime::info() const noexcept {
    return impl_->runtimeInfo;
}

void* Inochi2dVerifiedRuntime::resolveSymbol(
    std::string_view name) const noexcept {
#ifdef _WIN32
    if (!impl_ || impl_->module == nullptr || name.empty() ||
        name.find('\0') != std::string_view::npos) {
        return nullptr;
    }
    const std::string terminated{name};
    return reinterpret_cast<void*>(
        GetProcAddress(impl_->module, terminated.c_str()));
#else
    static_cast<void>(name);
    return nullptr;
#endif
}

Result<Inochi2dRuntimeInfo> Inochi2dRuntimeManifest::loadAndVerify(
    const std::filesystem::path& runtimeRoot) {
    const auto platform = currentPlatform();
    if (platform.target.empty()) {
        return unsupported(
            "This process target has no audited Inochi2D runtime");
    }

    if (isReparsePoint(runtimeRoot)) {
        return invalid("Inochi2D runtime root is missing or redirected");
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
    auto libraryInspection = inspectWindowsX64Dll(library);
    if (!libraryInspection.hasValue()) return libraryInspection.error();
    const std::unordered_set<std::string> actualExports{
        libraryInspection.value().exports.begin(),
        libraryInspection.value().exports.end()};
    for (const auto required : kRequiredSymbols) {
        if (!actualExports.contains(std::string{required})) {
            return unsupported(
                "Inochi2D runtime DLL is missing a required C FFI export");
        }
    }
    for (const auto& imported : libraryInspection.value().imports) {
        if (!isApprovedWindowsImport(imported)) {
            return unsupported(
                "Inochi2D runtime DLL imports an unapproved library");
        }
    }
    for (const auto& [relative, unusedHash] : runtimeDependencies) {
        static_cast<void>(unusedHash);
        auto dependencyInspection =
            inspectWindowsX64Dll(root / pathFromUtf8(relative));
        if (!dependencyInspection.hasValue()) {
            return dependencyInspection.error();
        }
        for (const auto& imported : dependencyInspection.value().imports) {
            if (!isApprovedWindowsImport(imported)) {
                return unsupported(
                    "Inochi2D dependency DLL imports an unapproved library");
            }
        }
    }

    info.libraryPath = std::filesystem::weakly_canonical(library, error);
    if (error) {
        return AppError{ErrorCode::IoFailure,
                        "Could not resolve the Inochi2D runtime library"};
    }
    info.librarySha256 = std::move(librarySha256);
    return info;
}

Result<Inochi2dVerifiedRuntime> Inochi2dRuntimeManifest::openVerified(
    const std::filesystem::path& runtimeRoot) {
#ifndef _WIN32
    static_cast<void>(runtimeRoot);
    return unsupported(
        "This process target has no audited Inochi2D runtime loader");
#else
    auto impl = std::make_unique<Inochi2dVerifiedRuntime::Impl>();
    impl->rootHandle = CreateFileW(
        runtimeRoot.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl->rootHandle == INVALID_HANDLE_VALUE) {
        return invalid("Inochi2D runtime root is missing or redirected");
    }
    FILE_ATTRIBUTE_TAG_INFO rootAttributes{};
    if (!GetFileInformationByHandleEx(
            impl->rootHandle, FileAttributeTagInfo, &rootAttributes,
            sizeof(rootAttributes)) ||
        (rootAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (rootAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return invalid("Inochi2D runtime root is missing or redirected");
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(runtimeRoot, error);
    if (error) {
        return invalid("Inochi2D runtime root is missing or redirected");
    }
    constexpr std::array<std::wstring_view, 6> leasedRelativePaths{
        L"runtime-manifest.json",
        L"LICENSE",
        L"THIRD_PARTY_NOTICES.txt",
        L"bin\\inochi2d.dll",
        L"bin\\druntime-ldc-shared.dll",
        L"bin\\phobos2-ldc-shared.dll",
    };
    impl->fileHandles.reserve(leasedRelativePaths.size());
    for (const auto relative : leasedRelativePaths) {
        const auto path = root / std::filesystem::path{relative};
        const auto handle =
            CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                        nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return invalid(
                "Inochi2D runtime artifact could not be leased");
        }
        impl->fileHandles.push_back(handle);
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(
                handle, FileAttributeTagInfo, &attributes,
                sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return invalid(
                "Inochi2D runtime artifact is redirected");
        }
    }

    auto verified = loadAndVerify(root);
    if (!verified.hasValue()) return verified.error();
    const auto libraryPath = verified.value().libraryPath;
    impl->module = LoadLibraryExW(
        libraryPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (impl->module == nullptr) {
        return AppError{ErrorCode::NotFound,
                        "Verified Inochi2D runtime could not be loaded"};
    }
    for (const auto required : kRequiredSymbols) {
        const std::string name{required};
        if (GetProcAddress(impl->module, name.c_str()) == nullptr) {
            return unsupported(
                "Verified Inochi2D runtime export could not be resolved");
        }
    }
    impl->runtimeInfo = std::move(verified).value();
    return Inochi2dVerifiedRuntime{std::move(impl)};
#endif
}

}  // namespace creator::avatar::inochi2d
