#include "avatar/AvatarAssetManifestCodec.h"
#include "avatar_pack_adapter/AvatarPackArchive.h"
#include "avatar_pack_adapter/AvatarPackValidator.h"
#include "core/Sha256.h"
#include "core/Uuid.h"

#include <gtest/gtest.h>
#include <miniz.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace creator::avatar_pack_adapter {
namespace {

namespace fs = std::filesystem;
using avatar::AvatarAssetManifest;
using avatar::AvatarAssetManifestCodec;
using avatar::AvatarAssetManifestDraft;
using avatar::AvatarPayloadHash;
using avatar::AvatarRepresentation;
using avatar::AvatarRight;
using avatar::AvatarSlot;

static_assert(!std::is_copy_constructible_v<AvatarPackStaging>);
static_assert(std::is_nothrow_move_constructible_v<AvatarPackStaging>);
static_assert(!std::is_copy_constructible_v<ValidatedAvatarPack>);
static_assert(std::is_nothrow_move_constructible_v<ValidatedAvatarPack>);
static_assert(
    noexcept(AvatarPackArchive::open(std::declval<const fs::path&>())));
static_assert(noexcept(std::declval<AvatarPackArchive&>().read(
    std::declval<const AvatarPackArchiveEntry&>(), 1U)));
static_assert(noexcept(std::declval<AvatarPackArchive&>().stream(
    std::declval<const AvatarPackArchiveEntry&>(), 1U,
    std::declval<const AvatarPackArchive::ChunkWriter&>())));
static_assert(
    noexcept(std::declval<const AvatarPackValidator&>().validateAndExtract(
        std::declval<const fs::path&>())));
using avatar::GrantState;
using avatar::LicenseGrant;
using avatar::RigFamily;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kGiB = 1024ULL * kMiB;

struct ZipEntry final {
    std::string path;
    std::vector<std::uint8_t> contents;
    mz_uint compression{MZ_NO_COMPRESSION};
    bool dataDescriptor{false};
};

struct Payload final {
    std::string path;
    std::vector<std::uint8_t> contents;
    mz_uint compression{MZ_NO_COMPRESSION};
    bool dataDescriptor{false};
};

std::vector<std::uint8_t> bytes(std::string_view value) {
    return {value.begin(), value.end()};
}

std::string digest(std::span<const std::uint8_t> value) {
    core::Sha256 hash;
    hash.update(value);
    return hash.finish();
}

std::array<std::uint8_t, crypto_hash_sha256_BYTES> decodeDigest(
    std::string_view value) {
    std::array<std::uint8_t, crypto_hash_sha256_BYTES> decoded{};
    std::size_t decodedLength = 0;
    if (sodium_hex2bin(decoded.data(), decoded.size(), value.data(), value.size(),
                       nullptr, &decodedLength, nullptr) != 0 ||
        decodedLength != decoded.size()) {
        throw std::runtime_error{"test digest could not be decoded"};
    }
    return decoded;
}

void writeBytes(const fs::path& path, std::span<const std::uint8_t> contents) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error{"test file could not be created"};
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) throw std::runtime_error{"test file could not be written"};
}

std::vector<std::uint8_t> readBytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error{"test file could not be opened"};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::uint16_t read16(const std::vector<std::uint8_t>& value,
                     std::size_t offset) {
    return static_cast<std::uint16_t>(value.at(offset)) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(value.at(offset + 1U)) << 8U);
}

std::uint32_t read32(const std::vector<std::uint8_t>& value,
                     std::size_t offset) {
    return static_cast<std::uint32_t>(value.at(offset)) |
           (static_cast<std::uint32_t>(value.at(offset + 1U)) << 8U) |
           (static_cast<std::uint32_t>(value.at(offset + 2U)) << 16U) |
           (static_cast<std::uint32_t>(value.at(offset + 3U)) << 24U);
}

void write16(std::vector<std::uint8_t>& value, std::size_t offset,
             std::uint16_t replacement) {
    value.at(offset) = static_cast<std::uint8_t>(replacement & 0xffU);
    value.at(offset + 1U) =
        static_cast<std::uint8_t>((replacement >> 8U) & 0xffU);
}

void write32(std::vector<std::uint8_t>& value, std::size_t offset,
             std::uint32_t replacement) {
    value.at(offset) = static_cast<std::uint8_t>(replacement & 0xffU);
    value.at(offset + 1U) =
        static_cast<std::uint8_t>((replacement >> 8U) & 0xffU);
    value.at(offset + 2U) =
        static_cast<std::uint8_t>((replacement >> 16U) & 0xffU);
    value.at(offset + 3U) =
        static_cast<std::uint8_t>((replacement >> 24U) & 0xffU);
}

std::size_t eocdOffset(const std::vector<std::uint8_t>& archive) {
    constexpr std::uint32_t kEocdSignature = 0x06054b50U;
    if (archive.size() < 22U)
        throw std::runtime_error{"test ZIP has no EOCD"};
    for (std::size_t offset = archive.size() - 22U;; --offset) {
        if (read32(archive, offset) == kEocdSignature) return offset;
        if (offset == 0U) break;
    }
    throw std::runtime_error{"test ZIP EOCD was not found"};
}

struct CentralEntry final {
    std::size_t offset{};
    std::size_t nameOffset{};
    std::size_t nameLength{};
    std::size_t localOffset{};
};

std::vector<CentralEntry> centralEntries(
    const std::vector<std::uint8_t>& archive) {
    constexpr std::uint32_t kCentralSignature = 0x02014b50U;
    std::vector<CentralEntry> result;
    for (std::size_t offset = 0; offset + 46U <= archive.size();) {
        if (read32(archive, offset) != kCentralSignature) {
            ++offset;
            continue;
        }
        const auto nameLength = read16(archive, offset + 28U);
        const auto extraLength = read16(archive, offset + 30U);
        const auto commentLength = read16(archive, offset + 32U);
        const std::size_t next = offset + 46U + nameLength + extraLength +
                                 commentLength;
        if (next > archive.size())
            throw std::runtime_error{"invalid test central directory"};
        result.push_back(
            {.offset = offset,
             .nameOffset = offset + 46U,
             .nameLength = nameLength,
             .localOffset = read32(archive, offset + 42U)});
        offset = next;
    }
    return result;
}

std::string centralName(const std::vector<std::uint8_t>& archive,
                        const CentralEntry& entry) {
    return {reinterpret_cast<const char*>(archive.data() + entry.nameOffset),
            entry.nameLength};
}

CentralEntry findCentral(const std::vector<std::uint8_t>& archive,
                         std::string_view path) {
    const auto entries = centralEntries(archive);
    const auto found =
        std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return centralName(archive, entry) == path;
        });
    if (found == entries.end())
        throw std::runtime_error{"test ZIP entry was not found"};
    return *found;
}

void patchSizes(const fs::path& path, std::string_view entryPath,
                std::optional<std::uint32_t> compressed,
                std::optional<std::uint32_t> uncompressed) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (compressed.has_value())
        write32(archive, entry.offset + 20U, *compressed);
    if (uncompressed.has_value())
        write32(archive, entry.offset + 24U, *uncompressed);
    writeBytes(path, archive);
}

void patchMethod(const fs::path& path, std::string_view entryPath,
                 std::uint16_t method) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write16(archive, entry.offset + 10U, method);
    write16(archive, entry.localOffset + 8U, method);
    writeBytes(path, archive);
}

void patchEncrypted(const fs::path& path, std::string_view entryPath) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write16(archive, entry.offset + 8U,
            static_cast<std::uint16_t>(
                read16(archive, entry.offset + 8U) | 1U));
    write16(archive, entry.localOffset + 6U,
            static_cast<std::uint16_t>(
                read16(archive, entry.localOffset + 6U) | 1U));
    writeBytes(path, archive);
}

void patchUnixMode(const fs::path& path, std::string_view entryPath,
                   std::uint16_t mode) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write16(archive, entry.offset + 4U, 0x0314U);
    write32(archive, entry.offset + 38U,
            static_cast<std::uint32_t>(mode) << 16U);
    writeBytes(path, archive);
}

void patchDosAttributes(const fs::path& path, std::string_view entryPath,
                        std::uint32_t attributes) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write16(archive, entry.offset + 4U, 0x0014U);
    write32(archive, entry.offset + 38U, attributes);
    writeBytes(path, archive);
}

void patchLocalNameOnly(const fs::path& path, std::string_view entryPath,
                        std::string_view replacement) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (replacement.size() != entry.nameLength)
        throw std::runtime_error{"test local name patch changes length"};
    std::copy(replacement.begin(), replacement.end(),
              archive.begin() +
                  static_cast<std::ptrdiff_t>(entry.localOffset + 30U));
    writeBytes(path, archive);
}

void patchLocalSizesOnly(const fs::path& path, std::string_view entryPath,
                         std::uint32_t compressed,
                         std::uint32_t uncompressed) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    write32(archive, entry.localOffset + 18U, compressed);
    write32(archive, entry.localOffset + 22U, uncompressed);
    writeBytes(path, archive);
}

void addCentralExtra(const fs::path& path, std::string_view entryPath,
                     std::size_t size) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (size > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error{"test central extra is too large"};
    const auto insertion = entry.nameOffset + entry.nameLength;
    const auto oldEocd = eocdOffset(archive);
    archive.insert(archive.begin() + static_cast<std::ptrdiff_t>(insertion),
                   size, 0x5aU);
    write16(archive, entry.offset + 30U,
            static_cast<std::uint16_t>(size));
    const auto newEocd = oldEocd + size;
    write32(archive, newEocd + 12U,
            read32(archive, newEocd + 12U) +
                static_cast<std::uint32_t>(size));
    writeBytes(path, archive);
}

void addCentralComment(const fs::path& path, std::string_view entryPath,
                       std::size_t size) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (size > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error{"test central comment is too large"};
    const auto extraLength = read16(archive, entry.offset + 30U);
    const auto insertion =
        entry.nameOffset + entry.nameLength + extraLength;
    const auto oldEocd = eocdOffset(archive);
    archive.insert(archive.begin() + static_cast<std::ptrdiff_t>(insertion),
                   size, 0x43U);
    write16(archive, entry.offset + 32U,
            static_cast<std::uint16_t>(size));
    const auto newEocd = oldEocd + size;
    write32(archive, newEocd + 12U,
            read32(archive, newEocd + 12U) +
                static_cast<std::uint32_t>(size));
    writeBytes(path, archive);
}

void expandCentralDirectory(const fs::path& path, std::size_t targetSize) {
    auto archive = readBytes(path);
    const auto oldEocd = eocdOffset(archive);
    const auto oldSize = static_cast<std::size_t>(
        read32(archive, oldEocd + 12U));
    if (targetSize <= oldSize ||
        targetSize > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error{"test central target is invalid"};
    }
    const auto added = targetSize - oldSize;
    archive.insert(archive.begin() + static_cast<std::ptrdiff_t>(oldEocd),
                   added, 0U);
    const auto newEocd = oldEocd + added;
    write32(archive, newEocd + 12U,
            static_cast<std::uint32_t>(targetSize));
    writeBytes(path, archive);
}

void addArchiveComment(const fs::path& path, std::size_t size) {
    auto archive = readBytes(path);
    if (size > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error{"test archive comment is too large"};
    const auto eocd = eocdOffset(archive);
    write16(archive, eocd + 20U, static_cast<std::uint16_t>(size));
    archive.insert(archive.begin() + static_cast<std::ptrdiff_t>(eocd + 22U),
                   size, 0x41U);
    writeBytes(path, archive);
}

void addSecondStructurallyValidEocd(const fs::path& path) {
    auto archive = readBytes(path);
    const auto trueEocd = eocdOffset(archive);
    constexpr std::size_t kEocdBytes = 22U;
    write16(archive, trueEocd + 20U, static_cast<std::uint16_t>(kEocdBytes));
    std::vector<std::uint8_t> falseEocd(kEocdBytes);
    write32(falseEocd, 0U, 0x06054b50U);
    write32(falseEocd, 16U, static_cast<std::uint32_t>(trueEocd + kEocdBytes));
    archive.insert(archive.end(), falseEocd.begin(), falseEocd.end());
    writeBytes(path, archive);
}

void stripUnrequestedDataDescriptors(
    const fs::path& path, const std::vector<ZipEntry>& requested) {
    auto archive = readBytes(path);
    const auto entries = centralEntries(archive);
    std::vector<std::pair<std::size_t, std::size_t>> removed;
    for (const auto& entry : entries) {
        const auto name = centralName(archive, entry);
        const auto requestedEntry =
            std::find_if(requested.begin(), requested.end(),
                         [&](const ZipEntry& candidate) {
                             return candidate.path == name;
                         });
        if (requestedEntry == requested.end() ||
            requestedEntry->dataDescriptor ||
            (read16(archive, entry.offset + 8U) & 0x0008U) == 0U) {
            continue;
        }
        const auto localNameBytes =
            read16(archive, entry.localOffset + 26U);
        const auto localExtraBytes =
            read16(archive, entry.localOffset + 28U);
        const auto compressedBytes =
            read32(archive, entry.offset + 20U);
        const auto descriptorOffset =
            entry.localOffset + 30U + localNameBytes +
            localExtraBytes + compressedBytes;
        if (descriptorOffset + 16U > archive.size() ||
            read32(archive, descriptorOffset) != 0x08074b50U) {
            throw std::runtime_error{
                "test ZIP data descriptor was not found"};
        }
        write16(archive, entry.offset + 8U,
                static_cast<std::uint16_t>(
                    read16(archive, entry.offset + 8U) & ~0x0008U));
        write16(archive, entry.localOffset + 6U,
                static_cast<std::uint16_t>(
                    read16(archive, entry.localOffset + 6U) & ~0x0008U));
        write32(archive, entry.localOffset + 14U,
                read32(archive, entry.offset + 16U));
        write32(archive, entry.localOffset + 18U, compressedBytes);
        write32(archive, entry.localOffset + 22U,
                read32(archive, entry.offset + 24U));
        removed.emplace_back(descriptorOffset, 16U);
    }
    const auto removedBefore = [&](std::size_t offset) {
        std::size_t total = 0;
        for (const auto& [start, size] : removed) {
            if (start < offset) total += size;
        }
        return total;
    };
    for (const auto& entry : entries) {
        write32(archive, entry.offset + 42U,
                static_cast<std::uint32_t>(
                    entry.localOffset - removedBefore(entry.localOffset)));
    }
    const auto eocd = eocdOffset(archive);
    const auto centralOffset = read32(archive, eocd + 16U);
    write32(archive, eocd + 16U,
            static_cast<std::uint32_t>(
                centralOffset - removedBefore(centralOffset)));
    std::sort(removed.begin(), removed.end(), std::greater<>{});
    for (const auto& [start, size] : removed) {
        archive.erase(
            archive.begin() + static_cast<std::ptrdiff_t>(start),
            archive.begin() +
                static_cast<std::ptrdiff_t>(start + size));
    }
    writeBytes(path, archive);
}

#ifdef _WIN32
std::error_code createDirectoryJunction(const fs::path& junction,
                                        const fs::path& target) {
    struct MountPointReparseData final {
        DWORD tag;
        WORD dataLength;
        WORD reserved;
        WORD substituteNameOffset;
        WORD substituteNameLength;
        WORD printNameOffset;
        WORD printNameLength;
        WCHAR pathBuffer[1];
    };

    if (!CreateDirectoryW(junction.c_str(), nullptr)) {
        return {static_cast<int>(GetLastError()), std::system_category()};
    }
    const std::wstring printName = fs::absolute(target).wstring();
    const std::wstring substituteName = L"\\??\\" + printName;
    const auto substituteBytes =
        static_cast<WORD>(substituteName.size() * sizeof(WCHAR));
    const auto printBytes =
        static_cast<WORD>(printName.size() * sizeof(WCHAR));
    constexpr std::size_t kMountPointHeaderBytes = 8U;
    const std::size_t pathBytes =
        static_cast<std::size_t>(substituteBytes) + sizeof(WCHAR) +
        printBytes + sizeof(WCHAR);
    alignas(void*) std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE>
        storage{};
    auto* data =
        reinterpret_cast<MountPointReparseData*>(storage.data());
    data->tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->dataLength =
        static_cast<WORD>(kMountPointHeaderBytes + pathBytes);
    data->substituteNameOffset = 0;
    data->substituteNameLength = substituteBytes;
    data->printNameOffset =
        static_cast<WORD>(substituteBytes + sizeof(WCHAR));
    data->printNameLength = printBytes;
    std::memcpy(data->pathBuffer, substituteName.data(), substituteBytes);
    std::memcpy(reinterpret_cast<std::byte*>(data->pathBuffer) +
                    data->printNameOffset,
                printName.data(), printBytes);

    const HANDLE handle = CreateFileW(
        junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = std::error_code{
            static_cast<int>(GetLastError()), std::system_category()};
        RemoveDirectoryW(junction.c_str());
        return error;
    }
    DWORD returned = 0;
    const DWORD inputBytes = static_cast<DWORD>(8U + data->dataLength);
    const BOOL created =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, inputBytes,
                        nullptr, 0, &returned, nullptr);
    const DWORD code = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!created) RemoveDirectoryW(junction.c_str());
    return {static_cast<int>(code), std::system_category()};
}
#endif

void patchNameByte(const fs::path& path, std::string_view entryPath,
                   std::size_t index, std::uint8_t replacement) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (index >= entry.nameLength)
        throw std::runtime_error{"test filename patch is out of range"};
    const auto localNameLength = read16(archive, entry.localOffset + 26U);
    if (localNameLength != entry.nameLength)
        throw std::runtime_error{"test local filename length differs"};
    archive.at(entry.nameOffset + index) = replacement;
    archive.at(entry.localOffset + 30U + index) = replacement;
    writeBytes(path, archive);
}

void patchName(const fs::path& path, std::string_view entryPath,
               std::string_view replacement) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (replacement.size() != entry.nameLength)
        throw std::runtime_error{"test filename patch changes length"};
    const auto localNameLength = read16(archive, entry.localOffset + 26U);
    if (localNameLength != entry.nameLength)
        throw std::runtime_error{"test local filename length differs"};
    std::copy(replacement.begin(), replacement.end(),
              archive.begin() +
                  static_cast<std::ptrdiff_t>(entry.nameOffset));
    std::copy(replacement.begin(), replacement.end(),
              archive.begin() +
                  static_cast<std::ptrdiff_t>(entry.localOffset + 30U));
    writeBytes(path, archive);
}

void corruptStoredPayload(const fs::path& path, std::string_view entryPath) {
    auto archive = readBytes(path);
    const auto entry = findCentral(archive, entryPath);
    if (read16(archive, entry.offset + 10U) != 0U)
        throw std::runtime_error{"test payload is not stored"};
    const auto nameLength = read16(archive, entry.localOffset + 26U);
    const auto extraLength = read16(archive, entry.localOffset + 28U);
    const auto dataOffset =
        entry.localOffset + 30U + nameLength + extraLength;
    archive.at(dataOffset) ^= 0x5aU;
    writeBytes(path, archive);
}

bool directoryEmpty(const fs::path& path) {
    return fs::directory_iterator{path} == fs::directory_iterator{};
}

std::optional<fs::path> waitForStagingRoot(
    const fs::path& parent,
    std::chrono::steady_clock::duration timeout =
        std::chrono::seconds{10}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        for (fs::directory_iterator iterator{parent, error};
             !error && iterator != fs::directory_iterator{};
             iterator.increment(error)) {
            if (iterator->path().filename().wstring().starts_with(
                    L".avatar-pack-")) {
                return iterator->path();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return std::nullopt;
}

class WritableFixtureFile final {
public:
    explicit WritableFixtureFile(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        descriptor_ =
            ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
#endif
    }

    ~WritableFixtureFile() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
    }

    WritableFixtureFile(const WritableFixtureFile&) = delete;
    WritableFixtureFile& operator=(const WritableFixtureFile&) = delete;

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }

    bool overwriteByte(std::uint64_t offset, std::uint8_t value) {
#ifdef _WIN32
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD written = 0;
        return SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) !=
                   FALSE &&
               WriteFile(handle_, &value, 1U, &written, nullptr) != FALSE &&
               written == 1U && FlushFileBuffers(handle_) != FALSE;
#else
        const auto written =
            ::pwrite(descriptor_, &value, 1U, static_cast<off_t>(offset));
        return written == 1 && ::fsync(descriptor_) == 0;
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

class AvatarPackValidatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_GE(sodium_init(), 0);
        root_ = fs::temp_directory_path() /
                ("creator-avatar-pack-" + core::generateUuidV4());
        staging_ = root_ / "staging";
        ASSERT_TRUE(fs::create_directories(staging_));
        ASSERT_EQ(crypto_sign_keypair(publicKey_.data(), secretKey_.data()), 0);
    }

    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    TrustedAvatarKey trustedKey(std::string keyId = "test.release") const {
        TrustedAvatarKey key{.keyId = std::move(keyId)};
        std::transform(publicKey_.begin(), publicKey_.end(),
                       key.publicKey.begin(), [](std::uint8_t value) {
                           return static_cast<std::byte>(value);
                       });
        return key;
    }

    AvatarPackValidator validator() const {
        return AvatarPackValidator{{trustedKey()}, staging_};
    }

    fs::path uniquePack() {
        return root_ / (core::generateUuidV4() + ".csavatarpack");
    }

    fs::path writeArchive(const std::vector<ZipEntry>& entries,
                          mz_uint writerFlags = 0U) {
        const auto path = uniquePack();
        mz_zip_archive archive{};
        mz_zip_zero_struct(&archive);
        if (!mz_zip_writer_init_file_v2(&archive, path.string().c_str(), 0U,
                                        writerFlags)) {
            throw std::runtime_error{"miniz writer could not open fixture"};
        }
        bool ended = false;
        try {
            for (const auto& entry : entries) {
                const void* data = entry.contents.empty()
                                       ? static_cast<const void*>("")
                                       : entry.contents.data();
                if (!mz_zip_writer_add_mem(&archive, entry.path.c_str(), data,
                                           entry.contents.size(),
                                           entry.compression)) {
                    throw std::runtime_error{"miniz could not add fixture entry"};
                }
            }
            if (!mz_zip_writer_finalize_archive(&archive))
                throw std::runtime_error{"miniz could not finalize fixture"};
            if (!mz_zip_writer_end(&archive))
                throw std::runtime_error{"miniz could not close fixture"};
            ended = true;
            if ((writerFlags &
                 static_cast<mz_uint>(MZ_ZIP_FLAG_WRITE_ZIP64)) == 0U) {
                stripUnrequestedDataDescriptors(path, entries);
            }
        } catch (...) {
            if (!ended) mz_zip_writer_end(&archive);
            throw;
        }
        return path;
    }

    AvatarAssetManifest manifestFor(
        const std::vector<Payload>& payloads,
        std::optional<std::uint64_t> declaredBytes = std::nullopt,
        std::optional<std::string> firstDigest = std::nullopt) {
        std::vector<AvatarPayloadHash> hashes;
        std::uint64_t payloadBytes = 0;
        for (std::size_t index = 0; index < payloads.size(); ++index) {
            payloadBytes += payloads[index].contents.size();
            hashes.push_back(
                {.path = payloads[index].path,
                 .sha256 = index == 0U && firstDigest.has_value()
                               ? *firstDigest
                               : digest(payloads[index].contents)});
        }
        auto packageId =
            avatar::AvatarPackageId::create("vendor.foundation");
        auto assetId =
            avatar::AvatarAssetId::create("core.body.humanoid");
        if (!packageId.hasValue() || !assetId.hasValue())
            throw std::runtime_error{"test identifier is invalid"};
        const auto validFrom =
            core::Utc::parseRfc3339("2026-07-24T00:00:00Z");
        if (!validFrom.hasValue())
            throw std::runtime_error{"test timestamp is invalid"};
        AvatarAssetManifestDraft draft{
            .packageId = packageId.value(),
            .packageVersion = "1.0.0",
            .assetId = assetId.value(),
            .assetVersion = "1.0.0",
            .displayName = "Foundation Body",
            .vendor = "Creator Studio",
            .supportedRepresentations = {AvatarRepresentation::GltfRig},
            .supportedRigFamilies = {RigFamily::Humanoid},
            .allowedSlots = {AvatarSlot::Body},
            .payloads = std::move(hashes),
            .performance =
                {.payloadBytes = declaredBytes.value_or(payloadBytes),
                 .textureBytes = 0,
                 .textureCount = 0,
                 .maxTextureDimension = 0,
                 .vertexCount = 1,
                 .triangleCount = 1,
                 .drawCallCount = 1,
                 .drawPartCount = 1,
                 .boneCount = 1},
            .sourceUri = "https://example.invalid/foundation",
            .licenseId = "commercial-test",
            .licenseVersion = "1.0.0",
            .grants =
                {{.right = AvatarRight::CommercialBroadcast,
                  .state = GrantState::Allowed,
                  .condition = ""},
                 {.right = AvatarRight::AppBundle,
                  .state = GrantState::Allowed,
                  .condition = ""},
                 {.right = AvatarRight::DerivativeCharacter,
                  .state = GrantState::Allowed,
                  .condition = ""}},
            .attributionText = "",
            .regionAllowList = {},
            .validFrom = validFrom.value(),
            .validUntil = std::nullopt,
        };
        auto manifest = AvatarAssetManifest::create(std::move(draft));
        if (!manifest.hasValue())
            throw std::runtime_error{manifest.error().message()};
        return std::move(manifest).value();
    }

    std::vector<std::uint8_t> signatureMessage(
        const AvatarAssetManifest& manifest) {
        const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
        std::vector<std::uint8_t> message{canonical.begin(), canonical.end()};
        auto payloads = manifest.values().payloads;
        std::sort(payloads.begin(), payloads.end(),
                  [](const auto& left, const auto& right) {
                      return left.path < right.path;
                  });
        for (const auto& payload : payloads) {
            message.insert(message.end(), payload.path.begin(),
                           payload.path.end());
            message.push_back(0U);
            const auto decoded = decodeDigest(payload.sha256);
            message.insert(message.end(), decoded.begin(), decoded.end());
        }
        return message;
    }

    fs::path writeSignedPack(
        const std::vector<Payload>& payloads,
        std::optional<std::uint64_t> declaredBytes = std::nullopt,
        std::optional<std::string> firstDigest = std::nullopt,
        std::string keyId = "test.release",
        std::optional<std::size_t> signatureSize = std::nullopt,
        bool corruptSignature = false,
        std::vector<ZipEntry> extraEntries = {},
        std::optional<std::string> omitPayload = std::nullopt) {
        const auto manifest =
            manifestFor(payloads, declaredBytes, std::move(firstDigest));
        const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
        const auto message = signatureMessage(manifest);
        std::array<std::uint8_t, crypto_sign_BYTES> signature{};
        unsigned long long signatureLength = 0;
        if (crypto_sign_detached(signature.data(), &signatureLength,
                                 message.data(), message.size(),
                                 secretKey_.data()) != 0 ||
            signatureLength != signature.size()) {
            throw std::runtime_error{"libsodium could not sign fixture"};
        }
        if (corruptSignature) signature[0] ^= 0x80U;
        const auto outputSignatureSize =
            signatureSize.value_or(signature.size());
        std::vector<ZipEntry> entries{
            {.path = "manifest.json", .contents = bytes(canonical)},
            {.path = "signature.ed25519",
             .contents =
                 std::vector<std::uint8_t>(
                     signature.begin(),
                     signature.begin() +
                         std::min(outputSignatureSize, signature.size()))},
            {.path = "signing-key-id.txt", .contents = bytes(keyId)},
        };
        if (outputSignatureSize > signature.size()) {
            entries[1].contents.resize(outputSignatureSize, 0U);
        }
        for (const auto& payload : payloads) {
            if (omitPayload.has_value() && payload.path == *omitPayload)
                continue;
            entries.push_back({.path = payload.path,
                               .contents = payload.contents,
                               .compression = payload.compression,
                               .dataDescriptor =
                                   payload.dataDescriptor});
        }
        entries.insert(entries.end(),
                       std::make_move_iterator(extraEntries.begin()),
                       std::make_move_iterator(extraEntries.end()));
        return writeArchive(entries);
    }

    fs::path writeSignedPack(
        std::string path = "payload/model.bin",
        std::string_view contents = "real-avatar-model") {
        return writeSignedPack(
            {{.path = std::move(path), .contents = bytes(contents)}});
    }

    fs::path writeSignedPackWithRawManifest(
        const std::vector<Payload>& payloads,
        const AvatarAssetManifest& manifest,
        const std::vector<std::uint8_t>& rawManifest) {
        const auto message = signatureMessage(manifest);
        std::array<std::uint8_t, crypto_sign_BYTES> signature{};
        unsigned long long signatureLength = 0;
        if (crypto_sign_detached(signature.data(), &signatureLength,
                                 message.data(), message.size(),
                                 secretKey_.data()) != 0 ||
            signatureLength != signature.size()) {
            throw std::runtime_error{"libsodium could not sign fixture"};
        }
        std::vector<ZipEntry> entries{
            {.path = "manifest.json", .contents = rawManifest},
            {.path = "signature.ed25519",
             .contents = std::vector<std::uint8_t>(
                 signature.begin(), signature.end())},
            {.path = "signing-key-id.txt",
             .contents = bytes("test.release")}};
        for (const auto& payload : payloads) {
            entries.push_back({.path = payload.path,
                               .contents = payload.contents,
                               .compression = payload.compression,
                               .dataDescriptor =
                                   payload.dataDescriptor});
        }
        return writeArchive(entries);
    }

    fs::path root_;
    fs::path staging_;
    std::array<std::uint8_t, crypto_sign_PUBLICKEYBYTES> publicKey_{};
    std::array<std::uint8_t, crypto_sign_SECRETKEYBYTES> secretKey_{};
};

TEST_F(AvatarPackValidatorTest, ExtractsARealValidSignedFixture) {
    const auto package = writeSignedPack();

    const auto result = validator().validateAndExtract(package);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().manifest.assetId().value(),
              "core.body.humanoid");
    auto manifestExists = result.value().staging.exists(
                                    "manifest.json");
    ASSERT_TRUE(manifestExists.hasValue());
    EXPECT_TRUE(manifestExists.value());
    auto signature =
        result.value().staging.read("signature.ed25519", crypto_sign_BYTES);
    ASSERT_TRUE(signature.hasValue());
    EXPECT_EQ(signature.value()
                  .size(),
              crypto_sign_BYTES);
    auto keyId =
        result.value().staging.read("signing-key-id.txt", 128U);
    ASSERT_TRUE(keyId.hasValue());
    EXPECT_EQ(keyId.value(),
              bytes("test.release"));
    auto payload =
        result.value().staging.read("payload/model.bin", 1024U);
    ASSERT_TRUE(payload.hasValue());
    EXPECT_EQ(payload.value(),
              bytes("real-avatar-model"));
}

TEST_F(AvatarPackValidatorTest,
       ExtractsCanonicalUtf8PathWithoutSystemCodePageConversion) {
    const std::string utf8Path = "payload/\xf0\x9f\x90\xb1.bin";
    const auto package = writeSignedPack(utf8Path, "model");

    const auto result = validator().validateAndExtract(package);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    auto payload = result.value().staging.read(utf8Path, 1024U);
    ASSERT_TRUE(payload.hasValue());
    EXPECT_EQ(payload.value(),
              bytes("model"));
}

TEST_F(AvatarPackValidatorTest, RejectsUnsafeRawEntryPathsBeforeExtraction) {
    const std::vector<std::string> unsafe{
        "../escape",       "payload/../escape", "payload/./model",
        "payload//model",  "payload/model/",    "/absolute",
        "//server/share",  "C:/drive",           "payload\\model",
        std::string{"payload/"} + static_cast<char>(1) + "model",
    };
    const auto outside = root_ / "outside-sentinel";
    writeBytes(outside, bytes("untouched"));
    for (const auto& path : unsafe) {
        const std::string placeholder(path.size(), 'x');
        const auto package =
            writeArchive({{.path = placeholder, .contents = bytes("x")}});
        patchName(package, placeholder, path);
        const auto result = validator().validateAndExtract(package);
        EXPECT_FALSE(result.hasValue()) << path;
        if (!result.hasValue())
            EXPECT_EQ(result.error().code(),
                      core::ErrorCode::InvalidArgument)
                << path;
        EXPECT_TRUE(directoryEmpty(staging_)) << path;
    }
    EXPECT_EQ(readBytes(outside), bytes("untouched"));
}

TEST_F(AvatarPackValidatorTest, RejectsEmbeddedNulInRawZipName) {
    const std::string original = "payload/xmodel";
    const auto package =
        writeArchive({{.path = original, .contents = bytes("x")}});
    patchNameByte(package, original, std::string_view{"payload/"}.size(), 0U);

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsDuplicateCaseAndUnicodeNormalizedEntryNames) {
    const auto duplicate = writeArchive(
        {{.path = "same", .contents = bytes("one")},
         {.path = "same", .contents = bytes("two")}});
    EXPECT_FALSE(validator().validateAndExtract(duplicate).hasValue());

    const auto caseCollision = writeSignedPack(
        {{.path = "payload/Model.bin", .contents = bytes("one")},
         {.path = "payload/model.bin", .contents = bytes("two")}});
    EXPECT_FALSE(validator().validateAndExtract(caseCollision).hasValue());

    const auto unicodeCollision = writeSignedPack(
        {{.path = "payload/caf\xc3\xa9.bin", .contents = bytes("one")},
         {.path = "payload/cafe\xcc\x81.bin", .contents = bytes("two")}});
    EXPECT_FALSE(validator().validateAndExtract(unicodeCollision).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsCaseInsensitiveFileDirectoryPrefixCollisionBeforeStaging) {
    const auto package = writeSignedPack(
        {{.path = "Payload", .contents = bytes("one")},
         {.path = "payload/model.bin", .contents = bytes("two")}});

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsDirectorySymlinkEncryptionAndUnsupportedMethods) {
    const auto directory =
        writeArchive({{.path = "payload/", .contents = {}}});
    EXPECT_FALSE(validator().validateAndExtract(directory).hasValue());

    auto symlink =
        writeArchive({{.path = "payload/link", .contents = bytes("target")}});
    patchUnixMode(symlink, "payload/link", 0120777U);
    EXPECT_FALSE(validator().validateAndExtract(symlink).hasValue());

    auto encrypted =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchEncrypted(encrypted, "payload/model");
    EXPECT_FALSE(validator().validateAndExtract(encrypted).hasValue());

    auto unsupported =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchMethod(unsupported, "payload/model", 99U);
    EXPECT_FALSE(validator().validateAndExtract(unsupported).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsEntryCountAndIndividualOrAggregateZipBombMetadata) {
    std::vector<ZipEntry> tooMany;
    tooMany.reserve(10'001U);
    for (std::size_t index = 0; index < 10'001U; ++index) {
        tooMany.push_back(
            {.path = "payload/" + std::to_string(index), .contents = {}});
    }
    EXPECT_FALSE(
        validator().validateAndExtract(writeArchive(tooMany)).hasValue());

    auto individual =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchSizes(individual, "payload/model", std::nullopt,
               static_cast<std::uint32_t>(512ULL * kMiB + 1ULL));
    EXPECT_FALSE(validator().validateAndExtract(individual).hasValue());

    auto compressed =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    patchSizes(compressed, "payload/model",
               static_cast<std::uint32_t>(512ULL * kMiB + 1ULL),
               std::nullopt);
    EXPECT_FALSE(validator().validateAndExtract(compressed).hasValue());

    std::vector<ZipEntry> five;
    for (std::size_t index = 0; index < 5U; ++index) {
        five.push_back(
            {.path = "payload/" + std::to_string(index),
             .contents = bytes("x")});
    }
    auto aggregateExpanded = writeArchive(five);
    auto aggregateCompressed = writeArchive(five);
    for (std::size_t index = 0; index < 5U; ++index) {
        const auto name = "payload/" + std::to_string(index);
        patchSizes(aggregateExpanded, name, std::nullopt,
                   static_cast<std::uint32_t>(500ULL * kMiB));
        patchSizes(aggregateCompressed, name,
                   static_cast<std::uint32_t>(500ULL * kMiB),
                   std::nullopt);
    }
    EXPECT_FALSE(
        validator().validateAndExtract(aggregateExpanded).hasValue());
    EXPECT_FALSE(
        validator().validateAndExtract(aggregateCompressed).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest, RejectsOversizedBoundedMetadata) {
    std::vector<std::uint8_t> oversizedManifest(
        static_cast<std::size_t>(8ULL * kMiB + 1ULL), 'x');
    const auto package = writeArchive(
        {{.path = "manifest.json",
          .contents = std::move(oversizedManifest),
          .compression = MZ_BEST_COMPRESSION},
         {.path = "signature.ed25519",
          .contents = std::vector<std::uint8_t>(crypto_sign_BYTES)},
         {.path = "signing-key-id.txt", .contents = bytes("test.release")}});

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::InvalidArgument);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsTruncationCrcMismatchAndSignatureTamperWithCleanup) {
    auto truncated = readBytes(writeSignedPack());
    truncated.resize(truncated.size() - 12U);
    const auto truncatedPath = uniquePack();
    writeBytes(truncatedPath, truncated);
    EXPECT_FALSE(validator().validateAndExtract(truncatedPath).hasValue());

    auto crcMismatch = writeSignedPack();
    corruptStoredPayload(crcMismatch, "payload/model.bin");
    EXPECT_FALSE(validator().validateAndExtract(crcMismatch).hasValue());

    const auto signatureMismatch =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("real-avatar-model")}},
                        std::nullopt, std::nullopt, "test.release",
                        std::nullopt, true);
    EXPECT_FALSE(
        validator().validateAndExtract(signatureMismatch).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsUnknownKeyInvalidSignatureLengthAndInvalidKeyId) {
    const auto unknown =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, "unknown.release");
    EXPECT_FALSE(validator().validateAndExtract(unknown).hasValue());

    const auto shortSignature =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, "test.release", 63U);
    EXPECT_FALSE(
        validator().validateAndExtract(shortSignature).hasValue());

    const auto invalidId =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, "test.release\n");
    EXPECT_FALSE(validator().validateAndExtract(invalidId).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsDuplicateOrInvalidTrustedKeyConfiguration) {
    const auto package = writeSignedPack();
    auto duplicate = trustedKey();
    AvatarPackValidator duplicateKeys{
        {trustedKey(), std::move(duplicate)}, staging_};
    EXPECT_FALSE(duplicateKeys.validateAndExtract(package).hasValue());

    AvatarPackValidator invalidId{{trustedKey("bad key id")}, staging_};
    EXPECT_FALSE(invalidId.validateAndExtract(package).hasValue());

    auto invalidBytes = trustedKey();
    invalidBytes.publicKey.fill(std::byte{0});
    AvatarPackValidator invalidKey{{std::move(invalidBytes)}, staging_};
    EXPECT_FALSE(invalidKey.validateAndExtract(package).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest, RejectsMissingAndExtraPayloadEntries) {
    const std::vector<Payload> payloads{
        {.path = "payload/model.bin", .contents = bytes("model")}};
    const auto missing =
        writeSignedPack(payloads, std::nullopt, std::nullopt,
                        "test.release", std::nullopt, false, {},
                        "payload/model.bin");
    EXPECT_FALSE(validator().validateAndExtract(missing).hasValue());

    const auto extra =
        writeSignedPack(payloads, std::nullopt, std::nullopt,
                        "test.release", std::nullopt, false,
                        {{.path = "payload/extra.bin",
                          .contents = bytes("extra")}});
    EXPECT_FALSE(validator().validateAndExtract(extra).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsManifestHashAndDeclaredPayloadByteMismatch) {
    const std::vector<Payload> payloads{
        {.path = "payload/model.bin", .contents = bytes("model")}};
    std::string wrongDigest(64U, '0');
    const auto wrongHash =
        writeSignedPack(payloads, std::nullopt, wrongDigest);
    EXPECT_FALSE(validator().validateAndExtract(wrongHash).hasValue());

    const auto wrongBytes =
        writeSignedPack(payloads, payloads.front().contents.size() + 1U);
    EXPECT_FALSE(validator().validateAndExtract(wrongBytes).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest, CleansStagingAfterExtractionWriteFailure) {
    const std::string overlongComponent(300U, 'x');
    const auto package =
        writeSignedPack("payload/" + overlongComponent, "model");

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::IoFailure);
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       AppliesArchiveValidationBeforeSignatureValidation) {
    auto package = writeSignedPack(
        {{.path = "payload/model.bin", .contents = bytes("model")}},
        std::nullopt, std::nullopt, "test.release", std::nullopt, true);
    patchNameByte(package, "payload/model.bin", 7U,
                  static_cast<std::uint8_t>('\\'));

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    ASSERT_TRUE(result.error().issueCode().has_value());
    EXPECT_EQ(*result.error().issueCode(), "avatar.pack.archive.path");
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsMetadataSizeAndAggregateExpansionPastTwoGiB) {
    const auto longKey =
        writeSignedPack({{.path = "payload/model.bin",
                          .contents = bytes("model")}},
                        std::nullopt, std::nullopt, std::string(129U, 'a'));
    EXPECT_FALSE(validator().validateAndExtract(longKey).hasValue());

    auto boundary =
        writeArchive({{.path = "payload/0", .contents = bytes("x")},
                      {.path = "payload/1", .contents = bytes("x")},
                      {.path = "payload/2", .contents = bytes("x")},
                      {.path = "payload/3", .contents = bytes("x")},
                      {.path = "payload/4", .contents = bytes("x")}});
    for (std::size_t index = 0; index < 4U; ++index) {
        patchSizes(boundary, "payload/" + std::to_string(index),
                   std::nullopt,
                   static_cast<std::uint32_t>(512ULL * kMiB));
    }
    patchSizes(boundary, "payload/4", std::nullopt, 1U);
    EXPECT_FALSE(validator().validateAndExtract(boundary).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsOversizedOrAmbiguousZipEnvelopeBeforeMinizAllocation) {
    const auto expectIssue = [&](const fs::path& package,
                                 std::string_view issue) {
        const auto result = validator().validateAndExtract(package);
        ASSERT_FALSE(result.hasValue());
        ASSERT_TRUE(result.error().issueCode().has_value());
        EXPECT_EQ(*result.error().issueCode(), issue);
        EXPECT_TRUE(directoryEmpty(staging_));
    };

    auto excessiveCount =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    auto countBytes = readBytes(excessiveCount);
    const auto countEocd = eocdOffset(countBytes);
    write16(countBytes, countEocd + 8U, 10'001U);
    write16(countBytes, countEocd + 10U, 10'001U);
    writeBytes(excessiveCount, countBytes);
    expectIssue(excessiveCount, "avatar.pack.archive.entry-count");

    auto oversizedCentral =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    expandCentralDirectory(oversizedCentral, 16U * 1024U * 1024U + 1U);
    expectIssue(oversizedCentral, "avatar.pack.archive.envelope");

    auto oversizedExtra =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    addCentralExtra(oversizedExtra, "payload/model", 4097U);
    expectIssue(oversizedExtra, "avatar.pack.archive.envelope");

    auto oversizedEntryComment =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    addCentralComment(oversizedEntryComment, "payload/model", 4097U);
    expectIssue(oversizedEntryComment, "avatar.pack.archive.envelope");

    auto oversizedArchiveComment =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    addArchiveComment(oversizedArchiveComment, 4097U);
    expectIssue(oversizedArchiveComment, "avatar.pack.archive.envelope");

    auto trailingData =
        readBytes(writeArchive(
            {{.path = "payload/model", .contents = bytes("x")}}));
    trailingData.push_back(0x42U);
    const auto trailingPath = uniquePack();
    writeBytes(trailingPath, trailingData);
    expectIssue(trailingPath, "avatar.pack.archive.envelope");

    auto oversizedArchive =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    std::error_code resizeError;
    fs::resize_file(oversizedArchive,
                    2ULL * kGiB + 80ULL * kMiB + 1ULL, resizeError);
    ASSERT_FALSE(resizeError) << resizeError.message();
    expectIssue(oversizedArchive, "avatar.pack.archive.archive-size");
}

TEST_F(AvatarPackValidatorTest,
       RejectsMultipleStructurallyValidEocdCandidates) {
    const auto package = writeSignedPack();
    addSecondStructurallyValidEocd(package);

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    ASSERT_TRUE(result.error().issueCode().has_value());
    EXPECT_EQ(*result.error().issueCode(), "avatar.pack.archive.envelope");
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsZip64SentinelsLocatorsAndRecordsBeforeMinizInitialization) {
    const auto expectEnvelope = [&](const fs::path& package) {
        const auto result = validator().validateAndExtract(package);
        ASSERT_FALSE(result.hasValue());
        ASSERT_TRUE(result.error().issueCode().has_value());
        EXPECT_EQ(*result.error().issueCode(),
                  "avatar.pack.archive.envelope");
        EXPECT_TRUE(directoryEmpty(staging_));
    };

    auto sentinel =
        writeArchive({{.path = "payload/model", .contents = bytes("x")}});
    auto sentinelBytes = readBytes(sentinel);
    const auto eocd = eocdOffset(sentinelBytes);
    write16(sentinelBytes, eocd + 8U, 0xffffU);
    write16(sentinelBytes, eocd + 10U, 0xffffU);
    write32(sentinelBytes, eocd + 12U, 0xffffffffU);
    write32(sentinelBytes, eocd + 16U, 0xffffffffU);
    writeBytes(sentinel, sentinelBytes);
    expectEnvelope(sentinel);

    const auto zip64 = writeArchive(
        {{.path = "payload/model", .contents = bytes("x")}},
        MZ_ZIP_FLAG_WRITE_ZIP64);
    const auto zip64Bytes = readBytes(zip64);
    const auto zip64Eocd = eocdOffset(zip64Bytes);
    ASSERT_GE(zip64Eocd, 20U);
    EXPECT_EQ(read32(zip64Bytes, zip64Eocd - 20U), 0x07064b50U);
    expectEnvelope(zip64);
}

TEST_F(AvatarPackValidatorTest,
       RejectsDataDescriptorsAndLocalCentralHeaderDisagreement) {
    auto descriptor = writeSignedPack(
        {{.path = "payload/model.bin",
          .contents = bytes("real-avatar-model"),
          .compression = MZ_NO_COMPRESSION,
          .dataDescriptor = true}});
    EXPECT_FALSE(validator().validateAndExtract(descriptor).hasValue());

    auto nameMismatch =
        writeSignedPack("payload/model.bin", "real-avatar-model");
    patchLocalNameOnly(nameMismatch, "payload/model.bin",
                       "payload/Model.bin");
    EXPECT_FALSE(validator().validateAndExtract(nameMismatch).hasValue());

    auto sizeMismatch =
        writeSignedPack("payload/model.bin", "real-avatar-model");
    patchLocalSizesOnly(sizeMismatch, "payload/model.bin", 18U, 19U);
    EXPECT_FALSE(validator().validateAndExtract(sizeMismatch).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsDecodedDuplicateManifestMembersAndWindowsReparseMetadata) {
    const std::vector<Payload> payloads{
        {.path = "payload/model.bin", .contents = bytes("model")}};
    const auto manifest = manifestFor(payloads);
    const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
    const auto vendor = canonical.find("\"vendor\"");
    ASSERT_NE(vendor, std::string::npos);

    for (const std::string duplicate :
         {"\"vendor\":\"attacker\",",
          "\"v\\u0065ndor\":\"attacker\","}) {
        auto raw = canonical;
        raw.insert(vendor, duplicate);
        const auto package = writeArchive(
            {{.path = "manifest.json", .contents = bytes(raw)},
             {.path = "signature.ed25519",
              .contents =
                  std::vector<std::uint8_t>(crypto_sign_BYTES)},
             {.path = "signing-key-id.txt",
              .contents = bytes("test.release")},
             {.path = "payload/model.bin", .contents = bytes("model")}});
        const auto result = validator().validateAndExtract(package);
        ASSERT_FALSE(result.hasValue());
        ASSERT_TRUE(result.error().issueCode().has_value());
        EXPECT_EQ(*result.error().issueCode(),
                  "avatar.pack.manifest.duplicate-member")
            << result.error().message();
    }

    auto reparse =
        writeArchive({{.path = "payload/reparse", .contents = bytes("x")}});
    patchDosAttributes(reparse, "payload/reparse", 0x400U);
    EXPECT_FALSE(validator().validateAndExtract(reparse).hasValue());
    EXPECT_TRUE(directoryEmpty(staging_));
}

TEST_F(AvatarPackValidatorTest,
       RejectsSuperscriptWindowsDeviceNamesDuringPreflight) {
    const std::vector<std::string> reserved{
        "payload/COM\xc2\xb9", "payload/COM\xc2\xb2",
        "payload/COM\xc2\xb3", "payload/LPT\xc2\xb9",
        "payload/LPT\xc2\xb2", "payload/LPT\xc2\xb3"};
    for (const auto& path : reserved) {
        const auto package = writeSignedPack(path, "model");
        const auto result = validator().validateAndExtract(package);
        EXPECT_FALSE(result.hasValue()) << path;
        EXPECT_TRUE(directoryEmpty(staging_)) << path;
    }
}

#ifdef _WIN32

TEST_F(AvatarPackValidatorTest, RejectsAPreOpenedWritableArchiveHandle) {
    const auto package = writeSignedPack();
    WritableFixtureFile mutablePackage{package};
    ASSERT_TRUE(mutablePackage.valid());

    const auto result = validator().validateAndExtract(package);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::IoFailure);
    EXPECT_TRUE(directoryEmpty(staging_));
}
#endif

TEST_F(AvatarPackValidatorTest,
       KeepsOneImmutableArchiveSourceDuringValidation) {
    const std::vector<Payload> payloads{
        {.path = "payload/model.bin", .contents = bytes("model")}};
    const auto manifest = manifestFor(payloads);
    const auto canonical = AvatarAssetManifestCodec{}.toJson(manifest).dump();
    std::vector<std::uint8_t> rawManifest(
        static_cast<std::size_t>(7ULL * kMiB), ' ');
    ASSERT_LT(canonical.size(), rawManifest.size());
    std::copy(canonical.begin(), canonical.end(), rawManifest.begin());
    const auto package =
        writeSignedPackWithRawManifest(payloads, manifest, rawManifest);
    const auto archive = readBytes(package);
    const auto central = findCentral(archive, "manifest.json");
    const auto eocd = eocdOffset(archive);
    const auto movedPackage = root_ / "moved-source.csavatarpack";

    std::atomic_bool mutationAttempted{false};
    std::atomic_bool writerOpened{false};
    std::atomic_bool mutationSucceeded{false};
    std::atomic_bool moveSucceeded{false};
    std::thread mutator{[&] {
        const auto observed = waitForStagingRoot(staging_);
        if (!observed.has_value()) return;
        mutationAttempted.store(true, std::memory_order_release);
        {
            WritableFixtureFile mutablePackage{package};
            writerOpened.store(
            mutablePackage.valid(),
                               std::memory_order_release);
            if (mutablePackage.valid()) {
                const bool centralChanged = mutablePackage.overwriteByte(
                    central.offset + 38U,
                static_cast<std::uint8_t>(archive.at(central.offset + 38U) ^
                                              0x40U));
                const bool eocdChanged = mutablePackage.overwriteByte(
                    eocd + 4U,
                    static_cast<std::uint8_t>(archive.at(eocd + 4U) ^ 0x01U));
                mutationSucceeded.store(centralChanged && eocdChanged,
            std::memory_order_release);
            }
        }
#ifdef _WIN32
        moveSucceeded.store(
            MoveFileExW(package.c_str(), movedPackage.c_str(), 0U) != FALSE,
            std::memory_order_release);
#else
        std::error_code error;
        fs::rename(package, movedPackage, error);
        moveSucceeded.store(!error, std::memory_order_release);
#endif
    }};

    const auto result = validator().validateAndExtract(package);
    mutator.join();

    ASSERT_TRUE(mutationAttempted.load(std::memory_order_acquire));
#ifdef _WIN32
    EXPECT_FALSE(writerOpened.load(std::memory_order_acquire));
    EXPECT_FALSE(mutationSucceeded.load(std::memory_order_acquire));
    EXPECT_FALSE(moveSucceeded.load(std::memory_order_acquire));
#else
    EXPECT_TRUE(writerOpened.load(std::memory_order_acquire));
    ASSERT_TRUE(mutationSucceeded.load(std::memory_order_acquire));
    EXPECT_TRUE(moveSucceeded.load(std::memory_order_acquire));
#endif
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    auto stagedManifest =
        result.value().staging.read("manifest.json", rawManifest.size());
    ASSERT_TRUE(stagedManifest.hasValue());
    EXPECT_EQ(stagedManifest.value(), rawManifest);
}

TEST_F(AvatarPackValidatorTest,
       SealedStagingCapabilityNeverReadsAPathReplacement) {
    auto result = validator().validateAndExtract(writeSignedPack());
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    auto& staging = result.value().staging;
    const auto displayPath = staging.displayPath();
    const auto moved = root_ / "moved-sealed-staging";

#ifdef _WIN32
    EXPECT_FALSE(MoveFileExW(displayPath.c_str(), moved.c_str(), 0U) != FALSE);
#else
    std::error_code renameError;
    fs::rename(displayPath, moved, renameError);
    ASSERT_FALSE(renameError) << renameError.message();
    ASSERT_TRUE(fs::create_directories(displayPath / "payload"));
    writeBytes(displayPath / "payload/model.bin", bytes("replacement"));
#endif

    auto payload = staging.read("payload/model.bin", 1024U);
    ASSERT_TRUE(payload.hasValue()) << payload.error().message();
    EXPECT_EQ(payload.value(), bytes("real-avatar-model"));

    auto cleaned = staging.cleanup();
#ifdef _WIN32
    EXPECT_TRUE(cleaned.hasValue())
        << (cleaned.hasValue() ? "" : cleaned.error().message());
    EXPECT_FALSE(fs::exists(displayPath));
#else
    ASSERT_FALSE(cleaned.hasValue());
    ASSERT_TRUE(cleaned.error().issueCode().has_value());
    EXPECT_EQ(*cleaned.error().issueCode(), "avatar.pack.staging.cleanup");
    EXPECT_EQ(readBytes(displayPath / "payload/model.bin"),
              bytes("replacement"));
#endif
}

#ifdef _WIN32
TEST_F(AvatarPackValidatorTest,
       RejectsARealJunctionInsertionWithoutWritingOutsideStaging) {
    const auto outside = root_ / "outside";
    ASSERT_TRUE(fs::create_directory(outside));
    writeBytes(outside / "sentinel", bytes("untouched"));
    std::vector<std::uint8_t> largePayload(
        static_cast<std::size_t>(32ULL * kMiB), 0x5aU);
    const auto package = writeSignedPack(
        {{.path = "aaa/large.bin", .contents = std::move(largePayload)},
         {.path = "zzz/escape.bin", .contents = bytes("escape")}});

    std::atomic_bool attackAttempted{false};
    std::atomic_bool moveSucceeded{false};
    std::atomic_int junctionCode{ERROR_TIMEOUT};
    fs::path observedRoot;
    fs::path injectedJunction;
    std::thread attacker{[&] {
        const auto observed = waitForStagingRoot(staging_);
        if (!observed.has_value()) return;
        observedRoot = *observed;
        const auto manifest = observedRoot / "manifest.json";
        const auto readyDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!fs::is_regular_file(manifest) &&
               std::chrono::steady_clock::now() < readyDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        const auto moved = root_ / "moved-staging";
        moveSucceeded.store(
            MoveFileExW(observedRoot.c_str(), moved.c_str(), 0U) != FALSE,
            std::memory_order_release);
        if (moveSucceeded.load(std::memory_order_acquire)) {
            injectedJunction = observedRoot;
        } else {
            injectedJunction = observedRoot / "zzz";
        }
        const auto junctionError =
            createDirectoryJunction(injectedJunction, outside);
        junctionCode.store(junctionError.value(),
                           std::memory_order_release);
        attackAttempted.store(true, std::memory_order_release);
    }};

    const auto result = validator().validateAndExtract(package);
    attacker.join();

    ASSERT_TRUE(attackAttempted.load(std::memory_order_acquire));
    EXPECT_FALSE(moveSucceeded.load(std::memory_order_acquire));
    ASSERT_EQ(junctionCode.load(std::memory_order_acquire), ERROR_SUCCESS);
    EXPECT_FALSE(result.hasValue());
    EXPECT_EQ(readBytes(outside / "sentinel"), bytes("untouched"));
    EXPECT_FALSE(fs::exists(outside / "escape.bin"));
    EXPECT_FALSE(fs::exists(outside / "zzz/escape.bin"));
    if (!injectedJunction.empty()) {
        RemoveDirectoryW(injectedJunction.c_str());
    }
}

TEST_F(AvatarPackValidatorTest,
       SurfacesCleanupFailureWhenARealOpenHandleBlocksRemoval) {
    std::vector<std::uint8_t> largePayload(
        static_cast<std::size_t>(32ULL * kMiB), 0x31U);
    const auto package = writeSignedPack(
        {{.path = "payload/model.bin",
          .contents = std::move(largePayload)}},
        std::nullopt, std::string(64U, '0'));

    std::atomic_bool locked{false};
    std::atomic_bool release{false};
    std::thread blocker{[&] {
        const auto observed = waitForStagingRoot(staging_);
        if (!observed.has_value()) return;
        const auto payload = *observed / "manifest.json";
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (!fs::is_regular_file(payload) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        HANDLE handle = INVALID_HANDLE_VALUE;
        while (handle == INVALID_HANDLE_VALUE &&
               std::chrono::steady_clock::now() < deadline) {
            handle = CreateFileW(
                payload.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                std::this_thread::yield();
        }
        if (handle == INVALID_HANDLE_VALUE) return;
        locked.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        CloseHandle(handle);
    }};

    const auto result = validator().validateAndExtract(package);
    release.store(true, std::memory_order_release);
    blocker.join();

    ASSERT_TRUE(locked.load(std::memory_order_acquire));
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), core::ErrorCode::IoFailure);
    ASSERT_TRUE(result.error().issueCode().has_value());
    EXPECT_EQ(*result.error().issueCode(),
              "avatar.pack.staging.cleanup");
}

TEST_F(AvatarPackValidatorTest, RejectsARealReparseStagingParent) {
    const auto outside = root_ / "outside-parent";
    ASSERT_TRUE(fs::create_directory(outside));
    const auto redirectedParent = root_ / "redirected-staging";
    const auto junctionError =
        createDirectoryJunction(redirectedParent, outside);
    ASSERT_FALSE(junctionError) << junctionError.message();
    AvatarPackValidator redirected{{trustedKey()}, redirectedParent};

    const auto result =
        redirected.validateAndExtract(writeSignedPack());

    ASSERT_FALSE(result.hasValue());
    EXPECT_FALSE(fs::exists(outside / "manifest.json"));
    ASSERT_TRUE(RemoveDirectoryW(redirectedParent.c_str()));
}
#endif

class AvatarPackValidatorFuzzSeedTest : public AvatarPackValidatorTest {};

TEST_F(AvatarPackValidatorFuzzSeedTest,
       RunsDeterministicMalformedArchiveSeedLoop) {
    const auto valid = readBytes(writeSignedPack());
    ASSERT_GT(valid.size(), 32U);
    for (std::size_t seed = 1U; seed <= 32U; ++seed) {
        auto mutated = valid;
        mutated.resize(mutated.size() - seed);
        const auto path = uniquePack();
        writeBytes(path, mutated);
        const auto result = validator().validateAndExtract(path);
        EXPECT_FALSE(result.hasValue()) << "seed " << seed;
        EXPECT_TRUE(directoryEmpty(staging_)) << "seed " << seed;
    }

    const auto central = findCentral(valid, "payload/model.bin");
    const auto eocd = eocdOffset(valid);
    const std::array<std::size_t, 12> mutationOffsets{
        0U,
        3U,
        central.offset,
        central.offset + 8U,
        central.offset + 20U,
        central.offset + 24U,
        central.offset + 28U,
        central.offset + 42U,
        eocd,
        eocd + 8U,
        eocd + 12U,
        eocd + 20U};
    for (std::size_t seed = 0; seed < mutationOffsets.size(); ++seed) {
        auto mutated = valid;
        mutated.at(mutationOffsets[seed]) ^=
            static_cast<std::uint8_t>(0xa5U + seed);
        const auto path = uniquePack();
        writeBytes(path, mutated);
        const auto result = validator().validateAndExtract(path);
        EXPECT_FALSE(result.hasValue()) << "structure seed " << seed;
        EXPECT_TRUE(directoryEmpty(staging_))
            << "structure seed " << seed;
    }
}

}  // namespace
}  // namespace creator::avatar_pack_adapter
