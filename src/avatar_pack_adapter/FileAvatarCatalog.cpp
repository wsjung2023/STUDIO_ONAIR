#include "avatar_pack_adapter/FileAvatarCatalog.h"
#include "avatar_pack_adapter/FileAvatarCatalogInternal.h"
#include "avatar_pack_adapter/CatalogRootAuthority.h"
#include "avatar_pack_adapter/SodiumSignatureVerifier.h"

#include "avatar/AvatarAssetManifestCodec.h"
#include "core/Sha256.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace creator::avatar_pack_adapter {
namespace {

namespace fs = std::filesystem;
using avatar::AvatarAssetManifest;
using avatar::AvatarAssetManifestCodec;
using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::uintmax_t kMaximumManifestBytes = 8U * 1024U * 1024U;

AppError catalogError(ErrorCode code, std::string message,
                      std::string issue) {
    return {code, std::move(message), std::move(issue),
            "avatar.catalog.error"};
}

AppError ioError(std::string issue = "avatar.catalog.io") {
    return catalogError(ErrorCode::IoFailure,
                        "avatar catalog integrity check failed",
                        std::move(issue));
}

std::mutex& processCatalogMutex() {
    static std::mutex mutex;
    return mutex;
}

bool trustedPrivateDirectory(const fs::path& path) {
    return detail::isTrustedPrivateDirectory(path);
}

bool isNoFollowDirectory(const fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
#else
    std::error_code error;
    return fs::symlink_status(path, error).type() ==
               fs::file_type::directory &&
           !error;
#endif
}

bool isNoFollowRegularFile(const fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
#else
    std::error_code error;
    return fs::symlink_status(path, error).type() ==
               fs::file_type::regular &&
           !error;
#endif
}

Result<void> verifyRemovableTree(const fs::path& root) {
    if (!trustedPrivateDirectory(root))
        return ioError("avatar.catalog.staging.cleanup");
    std::error_code error;
    for (fs::recursive_directory_iterator iterator{
             root, fs::directory_options::none, error},
         end;
         !error && iterator != end; iterator.increment(error)) {
        if ((isNoFollowDirectory(iterator->path()) &&
             trustedPrivateDirectory(iterator->path())) ||
            isNoFollowRegularFile(iterator->path())) {
            continue;
        }
        iterator.disable_recursion_pending();
        return ioError("avatar.catalog.staging.cleanup");
    }
    if (error) return ioError("avatar.catalog.staging.cleanup");
    return core::ok();
}

Result<void> flushDirectory(const fs::path& path) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return ioError("avatar.catalog.staging.flush");
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const bool closed = CloseHandle(handle) != FALSE;
    if (!flushed || !closed)
        return ioError("avatar.catalog.staging.flush");
#else
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
        return ioError("avatar.catalog.staging.flush");
    bool flushed = false;
    do {
        flushed = ::fsync(descriptor) == 0;
    } while (!flushed && errno == EINTR);
    const bool closed = ::close(descriptor) == 0;
    if (!flushed || !closed)
        return ioError("avatar.catalog.staging.flush");
#endif
    return core::ok();
}

Result<void> cleanupAbandonedStaging(const fs::path& staging) {
    const auto now = fs::file_time_type::clock::now();
    bool removed = false;
    std::error_code error;
    for (fs::directory_iterator iterator{staging, error}, end;
         !error && iterator != end; iterator.increment(error)) {
        const auto lastWrite = fs::last_write_time(iterator->path(), error);
        if (error) break;
        if (!detail::isAbandonedStagingEntry(lastWrite, now)) continue;
        auto safe = verifyRemovableTree(iterator->path());
        if (!safe.hasValue()) return safe.error();
        const auto count = fs::remove_all(iterator->path(), error);
        if (error || count == 0U)
            return ioError("avatar.catalog.staging.cleanup");
        removed = true;
    }
    if (error) return ioError("avatar.catalog.staging.cleanup");
    if (removed) return flushDirectory(staging);
    return core::ok();
}

Result<std::string> fileDigest(const fs::path& path,
                               std::uint64_t expectedBytes) {
    core::Sha256 hash;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    std::uint64_t consumed = 0;
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return ioError("avatar.catalog.payload");
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER size{};
    if (GetFileInformationByHandle(handle, &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) != expectedBytes) {
        CloseHandle(handle);
        return ioError("avatar.catalog.payload");
    }
    bool readSucceeded = true;
    for (;;) {
        DWORD count = 0U;
        if (ReadFile(handle, buffer.data(),
                     static_cast<DWORD>(buffer.size()), &count,
                     nullptr) == FALSE) {
            readSucceeded = false;
            break;
        }
        if (count == 0U) break;
        consumed += count;
        hash.update(
            std::span<const std::uint8_t>{buffer.data(), count});
    }
    const bool closed = CloseHandle(handle) != FALSE;
    if (!readSucceeded || !closed || consumed != expectedBytes)
        return ioError("avatar.catalog.payload");
#else
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return ioError("avatar.catalog.payload");
    struct stat information {};
    if (::fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_size < 0 ||
        static_cast<std::uint64_t>(information.st_size) != expectedBytes) {
        ::close(descriptor);
        return ioError("avatar.catalog.payload");
    }
    bool readSucceeded = true;
    for (;;) {
        const auto count =
            ::read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            consumed += static_cast<std::uint64_t>(count);
            hash.update(std::span<const std::uint8_t>{
                buffer.data(), static_cast<std::size_t>(count)});
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        readSucceeded = false;
        break;
    }
    const bool closed = ::close(descriptor) == 0;
    if (!readSucceeded || !closed || consumed != expectedBytes)
        return ioError("avatar.catalog.payload");
#endif
    return hash.finish();
}

Result<std::vector<std::uint8_t>> readRegularFile(
    const fs::path& path, std::size_t maximumBytes) {
    std::uint64_t size = 0;
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return ioError("avatar.catalog.metadata");
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER fileSize{};
    if (GetFileInformationByHandle(handle, &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        GetFileSizeEx(handle, &fileSize) == FALSE || fileSize.QuadPart < 0) {
        CloseHandle(handle);
        return ioError("avatar.catalog.metadata");
    }
    size = static_cast<std::uint64_t>(fileSize.QuadPart);
    if (size > maximumBytes) {
        CloseHandle(handle);
        return ioError("avatar.catalog.metadata");
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    bool readSucceeded = true;
    while (offset < contents.size()) {
        DWORD count = 0U;
        const auto remaining = static_cast<DWORD>(
            std::min<std::size_t>(contents.size() - offset, MAXDWORD));
        if (ReadFile(handle, contents.data() + offset, remaining, &count,
                     nullptr) == FALSE ||
            count == 0U) {
            readSucceeded = false;
            break;
        }
        offset += count;
    }
    std::uint8_t extra = 0;
    DWORD extraBytes = 0U;
    const bool exact =
        readSucceeded &&
        ReadFile(handle, &extra, 1U, &extraBytes, nullptr) != FALSE &&
        extraBytes == 0U;
    const bool closed = CloseHandle(handle) != FALSE;
    if (!exact || !closed)
        return ioError("avatar.catalog.metadata");
#else
    const int descriptor =
        ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return ioError("avatar.catalog.metadata");
    struct stat information {};
    if (::fstat(descriptor, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_size < 0) {
        ::close(descriptor);
        return ioError("avatar.catalog.metadata");
    }
    size = static_cast<std::uint64_t>(information.st_size);
    if (size > maximumBytes) {
        ::close(descriptor);
        return ioError("avatar.catalog.metadata");
    }
    std::vector<std::uint8_t> contents(static_cast<std::size_t>(size));
    std::size_t offset = 0;
    bool readSucceeded = true;
    while (offset < contents.size()) {
        const auto count = ::read(descriptor, contents.data() + offset,
                                  contents.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        readSucceeded = false;
        break;
    }
    std::uint8_t extra = 0;
    ssize_t extraBytes = -1;
    do {
        extraBytes = ::read(descriptor, &extra, 1U);
    } while (extraBytes < 0 && errno == EINTR);
    const bool closed = ::close(descriptor) == 0;
    if (!readSucceeded || extraBytes != 0 || !closed)
        return ioError("avatar.catalog.metadata");
#endif
    return contents;
}

std::string canonicalManifestHash(const AvatarAssetManifest& manifest) {
    const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
    core::Sha256 hash;
    hash.update(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(canonical.data()),
        canonical.size()});
    return hash.finish();
}

struct CatalogEntry final {
    AvatarAssetManifest manifest;
    fs::path versionRoot;
    std::string manifestHash;
};

Result<AvatarAssetManifest> loadManifest(const fs::path& versionRoot) {
    const auto manifestPath = versionRoot / "manifest.json";
    auto bytes = readRegularFile(
        manifestPath, static_cast<std::size_t>(kMaximumManifestBytes));
    if (!bytes.hasValue()) return ioError("avatar.catalog.manifest");
    const auto document = nlohmann::json::parse(
        bytes.value().begin(), bytes.value().end(), nullptr, false);
    if (document.is_discarded())
        return ioError("avatar.catalog.manifest");
    auto loaded = AvatarAssetManifestCodec{}.fromJson(document);
    if (!loaded.hasValue()) return ioError("avatar.catalog.manifest");
    return std::move(loaded).value();
}

Result<void> verifyMetadataTopology(const fs::path& versionRoot) {
    const std::set<std::string> expected{
        "manifest.json", "signature.ed25519", "signing-key-id.txt", "payload"};
    std::error_code error;
    std::set<std::string> actual;
    for (fs::directory_iterator iterator{versionRoot, error}, end;
         !error && iterator != end; iterator.increment(error)) {
        actual.insert(iterator->path().filename().string());
    }
    if (error || actual != expected)
        return ioError("avatar.catalog.topology");
    for (const auto* metadata :
         {"manifest.json", "signature.ed25519", "signing-key-id.txt"}) {
        const auto status =
            fs::symlink_status(versionRoot / metadata, error);
        if (error || status.type() != fs::file_type::regular)
            return ioError("avatar.catalog.topology");
    }
    const auto payloadStatus =
        fs::symlink_status(versionRoot / "payload", error);
    if (error || payloadStatus.type() != fs::file_type::directory)
        return ioError("avatar.catalog.topology");
    if (!trustedPrivateDirectory(versionRoot / "payload"))
        return ioError("avatar.catalog.topology");
    return core::ok();
}

Result<AvatarAssetManifest> loadVerifiedManifest(
    const fs::path& versionRoot,
    const std::vector<TrustedAvatarKey>& trustedKeys) {
    auto topology = verifyMetadataTopology(versionRoot);
    if (!topology.hasValue()) return topology.error();
    auto manifest = loadManifest(versionRoot);
    if (!manifest.hasValue()) return manifest.error();
    auto signatureBytes =
        readRegularFile(versionRoot / "signature.ed25519",
                        crypto_sign_BYTES);
    auto keyIdBytes =
        readRegularFile(versionRoot / "signing-key-id.txt", 128U);
    if (!signatureBytes.hasValue() || !keyIdBytes.hasValue() ||
        signatureBytes.value().size() != crypto_sign_BYTES ||
        keyIdBytes.value().empty()) {
        return ioError("avatar.catalog.signature");
    }
    const std::string keyId{keyIdBytes.value().begin(),
                            keyIdBytes.value().end()};
    const auto key =
        std::find_if(trustedKeys.begin(), trustedKeys.end(),
                     [&](const auto& candidate) {
                         return candidate.keyId == keyId;
                     });
    if (key == trustedKeys.end())
        return ioError("avatar.catalog.signature");

    const auto canonical =
        AvatarAssetManifestCodec{}.toJson(manifest.value()).dump();
    std::vector<std::uint8_t> message{canonical.begin(), canonical.end()};
    auto payloads = manifest.value().values().payloads;
    std::sort(payloads.begin(), payloads.end(),
              [](const auto& left, const auto& right) {
                  return left.path < right.path;
              });
    for (const auto& payload : payloads) {
        message.insert(message.end(), payload.path.begin(),
                       payload.path.end());
        message.push_back(0U);
        std::array<std::uint8_t, crypto_hash_sha256_BYTES> decoded{};
        std::size_t decodedBytes = 0;
        if (sodium_hex2bin(decoded.data(), decoded.size(),
                           payload.sha256.data(), payload.sha256.size(),
                           nullptr, &decodedBytes, nullptr) != 0 ||
            decodedBytes != decoded.size()) {
            return ioError("avatar.catalog.signature");
        }
        message.insert(message.end(), decoded.begin(), decoded.end());
    }
    std::array<std::byte, crypto_sign_BYTES> signature{};
    std::transform(signatureBytes.value().begin(),
                   signatureBytes.value().end(), signature.begin(),
                   [](std::uint8_t value) {
                       return static_cast<std::byte>(value);
                   });
    auto verified = SodiumSignatureVerifier{}.verifyDetached(
        signature, message, key->publicKey);
    if (!verified.hasValue())
        return ioError("avatar.catalog.signature");
    return std::move(manifest).value();
}

Result<void> verifyPayloads(const CatalogEntry& entry) {
    auto metadata = verifyMetadataTopology(entry.versionRoot);
    if (!metadata.hasValue()) return metadata.error();

    std::map<std::string, std::string> expected;
    std::set<std::string> expectedDirectories{"payload"};
    std::uint64_t declaredBytes = 0;
    for (const auto& payload : entry.manifest.values().payloads) {
        expected.emplace(payload.path, payload.sha256);
        const fs::path relative{payload.path};
        auto parent = relative.parent_path();
        while (!parent.empty()) {
            expectedDirectories.insert(parent.generic_string());
            parent = parent.parent_path();
        }
    }

    std::set<std::string> actualFiles;
    std::set<std::string> actualDirectories;
    std::error_code error;
    const auto payloadRoot = entry.versionRoot / "payload";
    for (fs::recursive_directory_iterator iterator{
             payloadRoot, fs::directory_options::none, error},
         end;
         !error && iterator != end; iterator.increment(error)) {
        const auto relative =
            iterator->path().lexically_relative(entry.versionRoot);
        if (relative.empty() || relative.is_absolute() ||
            relative.begin()->string() == "..") {
            return ioError("avatar.catalog.topology");
        }
        const auto status = iterator->symlink_status(error);
        if (error) break;
        const auto name = relative.generic_string();
        if (status.type() == fs::file_type::regular) {
            actualFiles.insert(name);
        } else if (status.type() == fs::file_type::directory) {
            if (!trustedPrivateDirectory(iterator->path()))
                return ioError("avatar.catalog.topology");
            actualDirectories.insert(name);
        } else {
            return ioError("avatar.catalog.topology");
        }
    }
    if (error) return ioError("avatar.catalog.topology");
    std::set<std::string> expectedFiles;
    for (const auto& [path, unused] : expected) {
        (void)unused;
        expectedFiles.insert(path);
    }
    expectedDirectories.erase("payload");
    actualDirectories.erase("payload");
    if (actualFiles != expectedFiles ||
        actualDirectories != expectedDirectories) {
        return ioError("avatar.catalog.topology");
    }

    for (const auto& payload : entry.manifest.values().payloads) {
        const auto size = fs::file_size(entry.versionRoot / payload.path,
                                        error);
        if (error) return ioError("avatar.catalog.payload");
        auto digestResult =
            fileDigest(entry.versionRoot / payload.path, size);
        if (!digestResult.hasValue() ||
            digestResult.value() != payload.sha256) {
            return ioError("avatar.catalog.payload");
        }
        declaredBytes += static_cast<std::uint64_t>(size);
    }
    if (declaredBytes != entry.manifest.values().performance.payloadBytes)
        return ioError("avatar.catalog.payload");
    return core::ok();
}

std::string utf8Filename(const fs::path& path) {
    const auto value = path.filename().u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

Result<std::vector<CatalogEntry>> scanCatalog(
    const fs::path& installed,
    const std::vector<TrustedAvatarKey>& trustedKeys) {
    std::vector<CatalogEntry> entries;
    std::set<std::pair<std::string, std::string>> assetVersions;
    std::error_code error;
    for (fs::directory_iterator packageIterator{installed, error}, packageEnd;
         !error && packageIterator != packageEnd;
         packageIterator.increment(error)) {
        const auto packageSpelling =
            utf8Filename(packageIterator->path());
        if (!detail::isPortablePackageId(packageSpelling))
            return ioError("avatar.catalog.layout");
        if (!trustedPrivateDirectory(packageIterator->path()))
            return ioError("avatar.catalog.layout");
        for (fs::directory_iterator versionIterator{
                 packageIterator->path(), error},
             versionEnd;
             !error && versionIterator != versionEnd;
             versionIterator.increment(error)) {
            const auto versionSpelling =
                utf8Filename(versionIterator->path());
            if (!detail::isCanonicalPackageVersion(versionSpelling))
                return ioError("avatar.catalog.layout");
            if (!trustedPrivateDirectory(versionIterator->path())) {
                return ioError("avatar.catalog.layout");
            }
            auto manifest =
                loadVerifiedManifest(versionIterator->path(), trustedKeys);
            if (!manifest.hasValue()) return manifest.error();
            const auto& values = manifest.value().values();
            if (values.packageId.value() !=
                    packageSpelling ||
                values.packageVersion !=
                    versionSpelling) {
                return ioError("avatar.catalog.layout");
            }
            const auto assetKey =
                std::pair{values.assetId.value(), values.assetVersion};
            if (!assetVersions.insert(assetKey).second)
                return ioError("avatar.catalog.duplicate");
            entries.push_back(
                {.manifest = std::move(manifest).value(),
                 .versionRoot = versionIterator->path(),
                 .manifestHash = {}});
            entries.back().manifestHash =
                canonicalManifestHash(entries.back().manifest);
        }
    }
    if (error) return ioError("avatar.catalog.layout");
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) {
                  return std::tie(left.manifest.values().assetId.value(),
                                  left.manifest.values().assetVersion,
                                  left.manifest.values().packageId.value(),
                                  left.manifest.values().packageVersion) <
                         std::tie(right.manifest.values().assetId.value(),
                                  right.manifest.values().assetVersion,
                                  right.manifest.values().packageId.value(),
                                  right.manifest.values().packageVersion);
              });
    return entries;
}

Result<CatalogEntry> findEntry(const fs::path& installed,
                               const std::vector<TrustedAvatarKey>& trustedKeys,
                               const avatar::AvatarAssetId& id,
                               std::string_view version) {
    auto entries = scanCatalog(installed, trustedKeys);
    if (!entries.hasValue()) return entries.error();
    const auto found =
        std::find_if(entries.value().begin(), entries.value().end(),
                     [&](const auto& entry) {
                         return entry.manifest.assetId() == id &&
                                entry.manifest.values().assetVersion ==
                                    version;
                     });
    if (found == entries.value().end()) {
        return catalogError(ErrorCode::NotFound,
                            "avatar asset is not installed",
                            "avatar.catalog.not-found");
    }
    return *found;
}

}  // namespace

class FileAvatarCatalog::Impl final {
public:
    Impl(detail::CatalogRootAuthority authority,
         std::vector<TrustedAvatarKey> trustedKeys)
        : authority_(std::move(authority)),
          root_(authority_.rootPath()),
          installed_(root_ / "installed"),
          staging_(root_ / "staging"),
          quarantine_(root_ / "quarantine"),
          trustedKeys_(trustedKeys),
          validator_(std::move(trustedKeys), staging_) {}

    [[nodiscard]] Result<void> verifyDirectories() const {
        auto authority = authority_.revalidate();
        if (!authority.hasValue()) return authority.error();
        for (const auto& path :
             {root_, installed_, staging_, quarantine_}) {
            if (!trustedPrivateDirectory(path))
                return ioError("avatar.catalog.directory");
        }
        return core::ok();
    }

    detail::CatalogRootAuthority authority_;
    fs::path root_;
    fs::path installed_;
    fs::path staging_;
    fs::path quarantine_;
    std::vector<TrustedAvatarKey> trustedKeys_;
    AvatarPackValidator validator_;
};

FileAvatarCatalog::FileAvatarCatalog(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

FileAvatarCatalog::FileAvatarCatalog(FileAvatarCatalog&&) noexcept = default;
FileAvatarCatalog& FileAvatarCatalog::operator=(
    FileAvatarCatalog&&) noexcept = default;
FileAvatarCatalog::~FileAvatarCatalog() = default;

Result<FileAvatarCatalog> FileAvatarCatalog::open(
    fs::path catalogRoot,
    std::vector<TrustedAvatarKey> trustedKeys) noexcept {
    try {
        std::lock_guard lock{processCatalogMutex()};
        auto openedAuthority =
            detail::CatalogRootAuthority::open(std::move(catalogRoot));
        if (!openedAuthority.hasValue())
            return openedAuthority.error();
        auto authority = std::move(openedAuthority).value();
        auto acquired = authority.lock();
        if (!acquired.hasValue()) return acquired.error();
        auto catalogLock = std::move(acquired).value();
        (void)catalogLock;
        for (const auto child :
             {"installed", "staging", "quarantine"}) {
            auto created = authority.ensurePrivateChild(child);
            if (!created.hasValue()) return created.error();
        }
        auto cleaned =
            cleanupAbandonedStaging(authority.rootPath() / "staging");
        if (!cleaned.hasValue()) return cleaned.error();
        auto implementation = std::make_unique<Impl>(
            std::move(authority), std::move(trustedKeys));
        auto entries = scanCatalog(implementation->installed_,
                                   implementation->trustedKeys_);
        if (!entries.hasValue()) return entries.error();
        for (const auto& entry : entries.value()) {
            auto verified = verifyPayloads(entry);
            if (!verified.hasValue()) return verified.error();
        }
        return FileAvatarCatalog{std::move(implementation)};
    } catch (...) {
        return ioError("avatar.catalog.open");
    }
}

Result<CatalogInstallOutcome> FileAvatarCatalog::install(
    const fs::path& packagePath) noexcept {
    try {
        std::lock_guard lock{processCatalogMutex()};
        auto acquired = implementation_->authority_.lock();
        if (!acquired.hasValue()) return acquired.error();
        auto catalogLock = std::move(acquired).value();
        (void)catalogLock;
        auto directories = implementation_->verifyDirectories();
        if (!directories.hasValue()) return directories.error();
        auto validated =
            implementation_->validator_.validateAndExtract(packagePath);
        if (!validated.hasValue()) return validated.error();

        const auto& values = validated.value().manifest.values();
        if (!detail::isPortablePackageId(values.packageId.value()) ||
            !detail::isCanonicalPackageVersion(values.packageVersion)) {
            auto cleaned = validated.value().staging.cleanup();
            return cleaned.hasValue()
                       ? Result<CatalogInstallOutcome>{catalogError(
                             ErrorCode::InvalidArgument,
                             "avatar package identity is not portable",
                             "avatar.catalog.package-path")}
                       : Result<CatalogInstallOutcome>{cleaned.error()};
        }
        const auto packageRoot =
            implementation_->installed_ / values.packageId.value();
        const auto finalRoot = packageRoot / values.packageVersion;
        if (packageRoot.parent_path() != implementation_->installed_ ||
            finalRoot.parent_path() != packageRoot ||
            finalRoot.lexically_normal() != finalRoot) {
            auto cleaned = validated.value().staging.cleanup();
            return cleaned.hasValue()
                       ? Result<CatalogInstallOutcome>{ioError(
                             "avatar.catalog.package-path")}
                       : Result<CatalogInstallOutcome>{cleaned.error()};
        }
        auto existing = scanCatalog(implementation_->installed_,
                                    implementation_->trustedKeys_);
        if (!existing.hasValue()) {
            auto cleaned = validated.value().staging.cleanup();
            return cleaned.hasValue()
                       ? Result<CatalogInstallOutcome>{existing.error()}
                       : Result<CatalogInstallOutcome>{cleaned.error()};
        }
        std::error_code error;
        if (fs::exists(finalRoot, error)) {
            auto installed = loadVerifiedManifest(
                finalRoot, implementation_->trustedKeys_);
            auto cleaned = validated.value().staging.cleanup();
            if (!cleaned.hasValue()) return cleaned.error();
            if (error || !installed.hasValue())
                return ioError("avatar.catalog.conflict");
            if (canonicalManifestHash(installed.value()) ==
                canonicalManifestHash(validated.value().manifest)) {
                CatalogEntry installedEntry{
                    .manifest = installed.value(),
                    .versionRoot = finalRoot,
                    .manifestHash =
                        canonicalManifestHash(installed.value())};
                auto payloads = verifyPayloads(installedEntry);
                if (!payloads.hasValue()) return payloads.error();
                return CatalogInstallOutcome::AlreadyInstalled;
            }
            return catalogError(ErrorCode::AlreadyExists,
                                "avatar package version already exists",
                                "avatar.catalog.already-exists");
        }
        if (error) {
            auto cleaned = validated.value().staging.cleanup();
            return cleaned.hasValue()
                       ? Result<CatalogInstallOutcome>{
                             ioError("avatar.catalog.install")}
                       : Result<CatalogInstallOutcome>{cleaned.error()};
        }

        const auto duplicate =
            std::find_if(existing.value().begin(), existing.value().end(),
                         [&](const auto& entry) {
                             return entry.manifest.assetId() ==
                                        validated.value().manifest.assetId() &&
                                    entry.manifest.values().assetVersion ==
                                        values.assetVersion;
                         });
        if (duplicate != existing.value().end()) {
            auto cleaned = validated.value().staging.cleanup();
            if (!cleaned.hasValue()) return cleaned.error();
            return ioError("avatar.catalog.duplicate");
        }

        auto packageDirectory = detail::ensurePrivateDirectoryChild(
            implementation_->installed_, values.packageId.value());
        if (!packageDirectory.hasValue()) {
            auto cleaned = validated.value().staging.cleanup();
            return cleaned.hasValue() ? Result<CatalogInstallOutcome>{
                                            packageDirectory.error()}
                                      : Result<CatalogInstallOutcome>{
                                            cleaned.error()};
        }
        auto promoted =
            std::move(validated.value().staging).promoteTo(finalRoot);
        if (!promoted.hasValue()) return promoted.error();
        auto installed = loadVerifiedManifest(
            finalRoot, implementation_->trustedKeys_);
        bool targetVerified =
            installed.hasValue() &&
            canonicalManifestHash(installed.value()) ==
                canonicalManifestHash(validated.value().manifest);
        if (targetVerified) {
            CatalogEntry installedEntry{
                .manifest = installed.value(),
                .versionRoot = finalRoot,
                .manifestHash =
                    canonicalManifestHash(installed.value())};
            targetVerified = verifyPayloads(installedEntry).hasValue();
        }
        return detail::reconciledInstallOutcome(promoted.value(),
                                                targetVerified);
    } catch (...) {
        return ioError("avatar.catalog.install");
    }
}

Result<std::vector<AvatarAssetManifest>> FileAvatarCatalog::list() const {
    try {
        std::lock_guard lock{processCatalogMutex()};
        auto acquired = implementation_->authority_.lock();
        if (!acquired.hasValue()) return acquired.error();
        auto catalogLock = std::move(acquired).value();
        (void)catalogLock;
        auto directories = implementation_->verifyDirectories();
        if (!directories.hasValue()) return directories.error();
        auto entries = scanCatalog(implementation_->installed_,
                                   implementation_->trustedKeys_);
        if (!entries.hasValue()) return entries.error();
        std::vector<AvatarAssetManifest> manifests;
        manifests.reserve(entries.value().size());
        for (auto& entry : entries.value())
            manifests.push_back(std::move(entry.manifest));
        return manifests;
    } catch (...) {
        return ioError("avatar.catalog.list");
    }
}

Result<AvatarAssetManifest> FileAvatarCatalog::find(
    const avatar::AvatarAssetId& id, std::string_view version) const {
    try {
        std::lock_guard lock{processCatalogMutex()};
        auto acquired = implementation_->authority_.lock();
        if (!acquired.hasValue()) return acquired.error();
        auto catalogLock = std::move(acquired).value();
        (void)catalogLock;
        auto directories = implementation_->verifyDirectories();
        if (!directories.hasValue()) return directories.error();
        auto entry = findEntry(implementation_->installed_,
                               implementation_->trustedKeys_, id, version);
        if (!entry.hasValue()) return entry.error();
        return std::move(entry).value().manifest;
    } catch (...) {
        return ioError("avatar.catalog.find");
    }
}

Result<fs::path> FileAvatarCatalog::payloadRoot(
    const avatar::AvatarAssetId& id, std::string_view version) const {
    try {
        std::lock_guard lock{processCatalogMutex()};
        auto acquired = implementation_->authority_.lock();
        if (!acquired.hasValue()) return acquired.error();
        auto catalogLock = std::move(acquired).value();
        (void)catalogLock;
        auto directories = implementation_->verifyDirectories();
        if (!directories.hasValue()) return directories.error();
        auto entry = findEntry(implementation_->installed_,
                               implementation_->trustedKeys_, id, version);
        if (!entry.hasValue()) return entry.error();
        auto verified = verifyPayloads(entry.value());
        if (!verified.hasValue()) return verified.error();
        return entry.value().versionRoot / "payload";
    } catch (...) {
        return ioError("avatar.catalog.payload");
    }
}

}  // namespace creator::avatar_pack_adapter
