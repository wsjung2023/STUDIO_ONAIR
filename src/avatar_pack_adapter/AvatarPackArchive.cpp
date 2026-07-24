#include "avatar_pack_adapter/AvatarPackArchive.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace creator::avatar_pack_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::uint32_t kUnixFileTypeMask = 0170000U;
constexpr std::uint32_t kUnixRegularFile = 0100000U;
constexpr std::uint32_t kDosDirectoryAttribute = 0x10U;
constexpr std::uint32_t kDosReparseAttribute = 0x400U;

AppError archiveError(ErrorCode code, std::string message,
                      std::string issueCode) {
    return {code, std::move(message), std::move(issueCode),
            "avatar.validation.archive"};
}

AppError invalidArchive(std::string message, std::string issueCode) {
    return archiveError(ErrorCode::InvalidArgument, std::move(message),
                        std::move(issueCode));
}

AppError invalidEnvelope(std::string message) {
    return invalidArchive(std::move(message),
                          "avatar.pack.archive.envelope");
}

std::uint16_t little16(std::span<const std::uint8_t> value,
                       std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(value[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(value[offset + 1U]) << 8U);
}

std::uint32_t little32(std::span<const std::uint8_t> value,
                       std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(value[offset]) |
           (static_cast<std::uint32_t>(value[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(value[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(value[offset + 3U]) << 24U);
}

bool readFileExact(std::FILE* file, std::uint64_t offset,
                   std::span<std::uint8_t> output) {
#ifdef _WIN32
    if (offset >
            static_cast<std::uint64_t>(
                std::numeric_limits<__int64>::max()) ||
        _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) != 0) {
        return false;
    }
#else
    if (offset >
            static_cast<std::uint64_t>(
                std::numeric_limits<off_t>::max()) ||
        ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
        return false;
    }
#endif
    return output.empty() ||
           std::fread(output.data(), 1U, output.size(), file) ==
               output.size();
}

bool validBoundedExtra(std::span<const std::uint8_t> extra) {
    std::size_t offset = 0;
    while (offset < extra.size()) {
        if (extra.size() - offset < 4U) return false;
        const auto identifier = little16(extra, offset);
        const auto size =
            static_cast<std::size_t>(little16(extra, offset + 2U));
        offset += 4U;
        if (size > extra.size() - offset || identifier == 0x0001U)
            return false;
        offset += size;
    }
    return true;
}

struct RawCentralEntry final {
    std::uint32_t localOffset{};
    std::uint16_t flags{};
    std::uint16_t method{};
    std::uint16_t modifiedTime{};
    std::uint16_t modifiedDate{};
    std::uint32_t crc{};
    std::uint32_t compressedBytes{};
    std::uint32_t expandedBytes{};
    std::vector<std::uint8_t> name;
};
constexpr std::uint32_t kEocdSignature = 0x06054b50U;
constexpr std::uint32_t kCentralSignature = 0x02014b50U;
constexpr std::uint32_t kLocalSignature = 0x04034b50U;
constexpr std::uint32_t kZip64LocatorSignature = 0x07064b50U;
constexpr std::size_t kEocdBytes = 22U;
constexpr std::size_t kCentralHeaderBytes = 46U;
constexpr std::size_t kLocalHeaderBytes = 30U;
constexpr std::uint16_t kDataDescriptorFlag = 0x0008U;

Result<void> validateRawEocdCandidate(std::FILE* file, std::uint64_t eocdOffset,
                                      std::span<const std::uint8_t> eocd) {
    const auto disk = little16(eocd, 4U);
    const auto centralDisk = little16(eocd, 6U);
    const auto diskEntries = little16(eocd, 8U);
    const auto totalEntries = little16(eocd, 10U);
    const auto centralBytes = little32(eocd, 12U);
    const auto centralOffset = little32(eocd, 16U);
    if (disk == 0xffffU || centralDisk == 0xffffU || diskEntries == 0xffffU ||
        totalEntries == 0xffffU || centralBytes == 0xffffffffU ||
        centralOffset == 0xffffffffU) {
        return invalidEnvelope("avatar pack ZIP64 is not supported");
    }
    if (disk != 0U || centralDisk != 0U || diskEntries != totalEntries) {
        return invalidEnvelope("avatar pack multi-disk ZIP is not supported");
    }
    if (totalEntries > AvatarPackArchive::kMaximumEntryCount) {
        return invalidArchive("avatar pack contains too many entries",
                              "avatar.pack.archive.entry-count");
    }
    if (centralBytes > AvatarPackArchive::kMaximumCentralDirectoryBytes) {
        return invalidEnvelope(
            "avatar pack central directory exceeds its limit");
    }
    if (static_cast<std::uint64_t>(centralOffset) > eocdOffset ||
        static_cast<std::uint64_t>(centralBytes) > eocdOffset - centralOffset ||
        static_cast<std::uint64_t>(centralOffset) + centralBytes !=
            eocdOffset) {
        return invalidEnvelope(
            "avatar pack central directory bounds are invalid");
    }
    if (static_cast<std::uint64_t>(totalEntries) * kCentralHeaderBytes >
        centralBytes) {
        return invalidEnvelope("avatar pack central directory is too small");
    }
    if (eocdOffset >= 20U) {
        std::array<std::uint8_t, 20> locator{};
        if (!readFileExact(file, eocdOffset - locator.size(), locator)) {
            return archiveError(
                ErrorCode::IoFailure,
                "avatar pack ZIP64 locator could not be inspected",
                "avatar.pack.archive.envelope");
        }
        if (little32(locator, 0U) == kZip64LocatorSignature) {
            return invalidEnvelope("avatar pack ZIP64 is not supported");
        }
    }

    std::vector<std::uint8_t> central(centralBytes);
    if (!readFileExact(file, centralOffset, central)) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack central directory could not be read",
                            "avatar.pack.archive.envelope");
    }
    std::vector<RawCentralEntry> entries;
    entries.reserve(totalEntries);
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < totalEntries; ++index) {
        if (central.size() - cursor < kCentralHeaderBytes ||
            little32(central, cursor) != kCentralSignature) {
            return invalidEnvelope(
                "avatar pack central directory record is invalid");
        }
        const auto header = std::span<const std::uint8_t>{central}.subspan(
            cursor, kCentralHeaderBytes);
        const auto nameBytes = static_cast<std::size_t>(little16(header, 28U));
        const auto extraBytes = static_cast<std::size_t>(little16(header, 30U));
        const auto commentBytes =
            static_cast<std::size_t>(little16(header, 32U));
        if (nameBytes == 0U ||
            nameBytes > AvatarPackArchive::kMaximumPathBytes ||
            extraBytes > AvatarPackArchive::kMaximumZipExtraBytes ||
            commentBytes > AvatarPackArchive::kMaximumZipCommentBytes) {
            return invalidEnvelope(
                "avatar pack central record fields exceed their limits");
        }
        const auto following = nameBytes + extraBytes + commentBytes;
        if (following > central.size() - cursor - kCentralHeaderBytes) {
            return invalidEnvelope("avatar pack central record is truncated");
        }
        if (little16(header, 34U) != 0U ||
            (little16(header, 8U) & kDataDescriptorFlag) != 0U) {
            return invalidEnvelope(
                "avatar pack central record uses unsupported indirection");
        }
        const auto compressedBytes = little32(header, 20U);
        const auto expandedBytes = little32(header, 24U);
        const auto localOffset = little32(header, 42U);
        if (compressedBytes == 0xffffffffU || expandedBytes == 0xffffffffU ||
            localOffset == 0xffffffffU) {
            return invalidEnvelope("avatar pack ZIP64 is not supported");
        }
        const auto nameOffset = cursor + kCentralHeaderBytes;
        const auto extraOffset = nameOffset + nameBytes;
        if (!validBoundedExtra(std::span<const std::uint8_t>{central}.subspan(
                extraOffset, extraBytes))) {
            return invalidEnvelope(
                "avatar pack central extra data is invalid or ZIP64");
        }
        entries.push_back(
            {.localOffset = localOffset,
             .flags = little16(header, 8U),
             .method = little16(header, 10U),
             .modifiedTime = little16(header, 12U),
             .modifiedDate = little16(header, 14U),
             .crc = little32(header, 16U),
             .compressedBytes = compressedBytes,
             .expandedBytes = expandedBytes,
             .name = std::vector<std::uint8_t>(
                 central.begin() + static_cast<std::ptrdiff_t>(nameOffset),
                 central.begin() +
                     static_cast<std::ptrdiff_t>(nameOffset + nameBytes))});
        cursor += kCentralHeaderBytes + following;
    }
    if (cursor != central.size()) {
        return invalidEnvelope(
            "avatar pack central directory has unclaimed bytes");
    }

    std::vector<std::pair<std::uint64_t, std::uint64_t>> localRanges;
    localRanges.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.localOffset >
            static_cast<std::uint64_t>(centralOffset) -
                std::min<std::uint64_t>(centralOffset, kLocalHeaderBytes)) {
            return invalidEnvelope(
                "avatar pack local header offset is invalid");
        }
        std::array<std::uint8_t, kLocalHeaderBytes> local{};
        if (!readFileExact(file, entry.localOffset, local) ||
            little32(local, 0U) != kLocalSignature) {
            return invalidEnvelope(
                "avatar pack local header is missing or invalid");
        }
        const auto nameBytes = static_cast<std::size_t>(little16(local, 26U));
        const auto extraBytes = static_cast<std::size_t>(little16(local, 28U));
        if (nameBytes != entry.name.size() ||
            nameBytes > AvatarPackArchive::kMaximumPathBytes ||
            extraBytes > AvatarPackArchive::kMaximumZipExtraBytes ||
            little16(local, 6U) != entry.flags ||
            little16(local, 8U) != entry.method ||
            little16(local, 10U) != entry.modifiedTime ||
            little16(local, 12U) != entry.modifiedDate ||
            little32(local, 14U) != entry.crc ||
            little32(local, 18U) != entry.compressedBytes ||
            little32(local, 22U) != entry.expandedBytes ||
            (little16(local, 6U) & kDataDescriptorFlag) != 0U) {
            return invalidEnvelope(
                "avatar pack local and central headers disagree");
        }
        const auto following = nameBytes + extraBytes;
        const auto dataOffset = static_cast<std::uint64_t>(entry.localOffset) +
                                kLocalHeaderBytes + following;
        if (dataOffset > static_cast<std::uint64_t>(centralOffset) ||
            entry.compressedBytes >
                static_cast<std::uint64_t>(centralOffset) - dataOffset) {
            return invalidEnvelope(
                "avatar pack local entry bounds are invalid");
        }
        std::vector<std::uint8_t> localFollowing(following);
        if (!readFileExact(file,
                           static_cast<std::uint64_t>(entry.localOffset) +
                               kLocalHeaderBytes,
                           localFollowing)) {
            return invalidEnvelope(
                "avatar pack local header fields are truncated");
        }
        if (!std::equal(entry.name.begin(), entry.name.end(),
                        localFollowing.begin())) {
            return invalidEnvelope(
                "avatar pack local and central names disagree");
        }
        if (!validBoundedExtra(
                std::span<const std::uint8_t>{localFollowing}.subspan(
                    nameBytes, extraBytes))) {
            return invalidEnvelope(
                "avatar pack local extra data is invalid or ZIP64");
        }
        localRanges.emplace_back(entry.localOffset,
                                 dataOffset + entry.compressedBytes);
    }
    std::sort(localRanges.begin(), localRanges.end());
    for (std::size_t index = 1; index < localRanges.size(); ++index) {
        if (localRanges[index].first < localRanges[index - 1U].second) {
            return invalidEnvelope("avatar pack local entries overlap");
        }
    }
    return core::ok();
}

Result<void> preflightRawZip(std::FILE* file, std::uint64_t archiveSize) {
    if (archiveSize < kEocdBytes) {
        return invalidEnvelope("avatar pack ZIP envelope is truncated");
    }
    const auto tailBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
        archiveSize, kEocdBytes + AvatarPackArchive::kMaximumZipCommentBytes));
    std::vector<std::uint8_t> tail(tailBytes);
    const auto tailOffset = archiveSize - tailBytes;
    if (!readFileExact(file, tailOffset, tail)) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack ZIP envelope could not be read",
                            "avatar.pack.archive.envelope");
    }

    std::size_t plausibleCandidates = 0;
    std::size_t validCandidates = 0;
    std::optional<AppError> singleCandidateError;
    for (std::size_t offset = 0; offset + kEocdBytes <= tail.size(); ++offset) {
        if (little32(tail, offset) != kEocdSignature) continue;
        const auto commentBytes =
            static_cast<std::size_t>(little16(tail, offset + 20U));
        if (commentBytes > AvatarPackArchive::kMaximumZipCommentBytes ||
            offset + kEocdBytes + commentBytes != tail.size()) {
            continue;
        }
        ++plausibleCandidates;
        const auto eocdOffset = tailOffset + static_cast<std::uint64_t>(offset);
        auto candidate = validateRawEocdCandidate(
            file, eocdOffset,
            std::span<const std::uint8_t>{tail}.subspan(offset, kEocdBytes));
        if (candidate.hasValue()) {
            ++validCandidates;
        } else if (candidate.error().code() == ErrorCode::IoFailure) {
            return candidate.error();
        } else if (plausibleCandidates == 1U) {
            singleCandidateError = candidate.error();
        }
    }
    if (validCandidates == 1U) return core::ok();
    if (plausibleCandidates == 1U && singleCandidateError.has_value())
        return *singleCandidateError;
    return invalidEnvelope(
        "avatar pack ZIP EOCD is missing, ambiguous, or trailing");
}

struct ImmutableArchiveSource final {
    std::FILE* file{};
    std::uint64_t size{};
};

Result<ImmutableArchiveSource>
openImmutableArchive(const std::filesystem::path& packagePath) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        packagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        return archiveError(
            code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
                ? ErrorCode::NotFound
                : ErrorCode::IoFailure,
            code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND
                ? "avatar pack does not exist"
                : "avatar pack could not be opened",
            "avatar.pack.archive.open");
    }
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER length{};
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        GetFileInformationByHandle(handle, &information) == FALSE ||
        GetFileSizeEx(handle, &length) == FALSE || length.QuadPart < 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        CloseHandle(handle);
        return invalidArchive("avatar pack must be a regular file",
                              "avatar.pack.archive.open");
    }
    const auto size = static_cast<std::uint64_t>(length.QuadPart);
    if (size > AvatarPackArchive::kMaximumArchiveBytes) {
        CloseHandle(handle);
        return invalidArchive("avatar pack archive exceeds its size limit",
                              "avatar.pack.archive.archive-size");
    }
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                            _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(handle);
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack could not be opened",
                            "avatar.pack.archive.open");
    }
    auto* file = _fdopen(descriptor, "rb");
    if (file == nullptr) {
        _close(descriptor);
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack could not be opened",
                            "avatar.pack.archive.open");
    }
    return ImmutableArchiveSource{.file = file, .size = size};
#else
    int source = ::open(packagePath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (source < 0) {
        return archiveError(errno == ENOENT ? ErrorCode::NotFound
                                            : ErrorCode::IoFailure,
                            errno == ENOENT ? "avatar pack does not exist"
                                            : "avatar pack could not be opened",
                            "avatar.pack.archive.open");
    }
    struct stat information{};
    if (::fstat(source, &information) != 0 || !S_ISREG(information.st_mode) ||
        information.st_size < 0) {
        ::close(source);
        return invalidArchive("avatar pack must be a regular file",
                              "avatar.pack.archive.open");
    }
    if (static_cast<std::uint64_t>(information.st_size) >
        AvatarPackArchive::kMaximumArchiveBytes) {
        ::close(source);
        return invalidArchive("avatar pack archive exceeds its size limit",
                              "avatar.pack.archive.archive-size");
    }

    auto pattern = (std::filesystem::temp_directory_path() /
                    "creator-avatar-pack-snapshot-XXXXXX")
                       .string();
    std::vector<char> writablePattern(pattern.begin(), pattern.end());
    writablePattern.push_back('\0');
    const int snapshot = ::mkstemp(writablePattern.data());
    if (snapshot < 0) {
        ::close(source);
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack private snapshot could not be created",
                            "avatar.pack.archive.snapshot");
    }
    const bool privateMode = ::fchmod(snapshot, 0600) == 0;
    const bool closeOnExec = ::fcntl(snapshot, F_SETFD, FD_CLOEXEC) == 0;
    const bool unlinked = ::unlink(writablePattern.data()) == 0;
    if (!privateMode || !closeOnExec || !unlinked) {
        ::close(source);
        ::close(snapshot);
        if (!unlinked) ::unlink(writablePattern.data());
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack private snapshot could not be secured",
                            "avatar.pack.archive.snapshot");
    }

    std::uint64_t copied = 0;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    bool copySucceeded = true;
    for (;;) {
        ssize_t readBytes = ::read(source, buffer.data(), buffer.size());
        if (readBytes < 0 && errno == EINTR) continue;
        if (readBytes < 0) {
            copySucceeded = false;
            break;
        }
        if (readBytes == 0) break;
        const auto count = static_cast<std::uint64_t>(readBytes);
        if (count > AvatarPackArchive::kMaximumArchiveBytes - copied) {
            copied = AvatarPackArchive::kMaximumArchiveBytes + 1U;
            break;
        }
        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(readBytes)) {
            const auto written =
                ::write(snapshot, buffer.data() + offset,
                        static_cast<std::size_t>(readBytes) - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                copySucceeded = false;
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        if (!copySucceeded) break;
        copied += count;
    }
    if (::close(source) != 0) copySucceeded = false;
    if (copied > AvatarPackArchive::kMaximumArchiveBytes) {
        ::close(snapshot);
        return invalidArchive("avatar pack archive exceeds its size limit",
                              "avatar.pack.archive.archive-size");
    }
    if (!copySucceeded || ::lseek(snapshot, 0, SEEK_SET) < 0) {
        ::close(snapshot);
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack private snapshot could not be written",
                            "avatar.pack.archive.snapshot");
    }
    auto* file = ::fdopen(snapshot, "rb");
    if (file == nullptr) {
        ::close(snapshot);
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack private snapshot could not be opened",
                            "avatar.pack.archive.snapshot");
    }
    return ImmutableArchiveSource{.file = file, .size = copied};
#endif
}

std::optional<std::vector<std::uint32_t>> decodeUtf8(
    std::string_view value) {
    std::vector<std::uint32_t> codePoints;
    codePoints.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t trailing = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7fU) {
            codePoint = first;
        } else if ((first & 0xe0U) == 0xc0U) {
            trailing = 1;
            codePoint = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            trailing = 2;
            codePoint = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            trailing = 3;
            codePoint = first & 0x07U;
        } else {
            return std::nullopt;
        }
        if (index + trailing >= value.size()) return std::nullopt;
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const auto byte =
                static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) return std::nullopt;
            codePoint = (codePoint << 6U) | (byte & 0x3fU);
        }
        const auto minimum =
            trailing == 0 ? 0U : trailing == 1 ? 0x80U
                                                : trailing == 2 ? 0x800U
                                                                : 0x10000U;
        if (codePoint < minimum || codePoint > 0x10ffffU ||
            (codePoint >= 0xd800U && codePoint <= 0xdfffU)) {
            return std::nullopt;
        }
        codePoints.push_back(codePoint);
        index += trailing + 1U;
    }
    return codePoints;
}

bool invalidWindowsComponent(std::string_view component) noexcept {
    if (component.empty() || component == "." || component == ".." ||
        component.back() == ' ' || component.back() == '.') {
        return true;
    }
    if (component.find_first_of("<>:\"|?*") != std::string_view::npos)
        return true;

    std::string stem{component.substr(0, component.find('.'))};
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(
                           character >= 'a' && character <= 'z'
                               ? character - ('a' - 'A')
                               : character);
                   });
    static constexpr std::array<std::string_view, 22> kReserved{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
        "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    if (std::find(kReserved.begin(), kReserved.end(), stem) !=
        kReserved.end()) {
        return true;
    }
    constexpr std::array<std::string_view, 3> kSuperscriptDigits{
        "\xc2\xb9", "\xc2\xb2", "\xc2\xb3"};
    if (stem.starts_with("COM") || stem.starts_with("LPT")) {
        const std::string_view suffix{stem.data() + 3U,
                                      stem.size() - 3U};
        return std::find(kSuperscriptDigits.begin(),
                         kSuperscriptDigits.end(),
                         suffix) != kSuperscriptDigits.end();
    }
    return false;
}

bool validRelativePath(std::string_view path) {
    if (path.empty() || path.size() > AvatarPackArchive::kMaximumPathBytes ||
        path.front() == '/' || path.find('\\') != std::string_view::npos ||
        (path.size() >= 2U &&
         ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':')) {
        return false;
    }
    const auto codePoints = decodeUtf8(path);
    if (!codePoints.has_value() ||
        std::any_of(codePoints->begin(), codePoints->end(),
                    [](std::uint32_t codePoint) {
                        return codePoint <= 0x1fU ||
                               (codePoint >= 0x7fU &&
                                codePoint <= 0x9fU);
                    })) {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto component =
            path.substr(start, slash == std::string_view::npos
                                   ? std::string_view::npos
                                   : slash - start);
        if (invalidWindowsComponent(component)) return false;
        if (slash == std::string_view::npos) return true;
        start = slash + 1U;
    }
    return false;
}

#ifdef _WIN32
std::optional<std::wstring> utf8ToWide(std::string_view value) {
    if (value.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            required) != required) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> collisionKey(std::string_view value) {
    const auto wide = utf8ToWide(value);
    if (!wide.has_value()) return std::nullopt;
    const int normalizedLength = NormalizeString(
        NormalizationC, wide->data(), static_cast<int>(wide->size()), nullptr,
        0);
    if (normalizedLength <= 0) return std::nullopt;
    std::wstring normalized(static_cast<std::size_t>(normalizedLength), L'\0');
    const int normalizedWritten =
        NormalizeString(NormalizationC, wide->data(),
                        static_cast<int>(wide->size()), normalized.data(),
                        normalizedLength);
    if (normalizedWritten <= 0) {
        return std::nullopt;
    }
    normalized.resize(static_cast<std::size_t>(normalizedWritten));
    const int foldedLength = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, normalized.data(),
        static_cast<int>(normalized.size()), nullptr, 0, nullptr, nullptr, 0);
    if (foldedLength <= 0) return std::nullopt;
    std::wstring folded(static_cast<std::size_t>(foldedLength), L'\0');
    const int foldedWritten =
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
                      normalized.data(), static_cast<int>(normalized.size()),
                      folded.data(), foldedLength, nullptr, nullptr, 0);
    if (foldedWritten <= 0) {
        return std::nullopt;
    }
    folded.resize(static_cast<std::size_t>(foldedWritten));
    return folded;
}
#else
std::optional<std::string> collisionKey(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (character > 0x7fU) return std::nullopt;
        result.push_back(static_cast<char>(
            character >= 'A' && character <= 'Z'
                ? character + ('a' - 'A')
                : character));
    }
    return result;
}
#endif

bool symlinkLike(const mz_zip_archive_file_stat& stat) noexcept {
    const auto host = static_cast<std::uint8_t>(stat.m_version_made_by >> 8U);
    if (stat.m_is_directory ||
        (stat.m_external_attr & kDosDirectoryAttribute) != 0U ||
        (stat.m_external_attr & kDosReparseAttribute) != 0U) {
        return true;
    }
    if (host == 3U || host == 19U) {
        const auto mode = stat.m_external_attr >> 16U;
        const auto type = mode & kUnixFileTypeMask;
        return type != 0U && type != kUnixRegularFile;
    }
    return false;
}

template <typename String>
bool pathPrefixCollision(std::vector<String> paths) {
    std::sort(paths.begin(), paths.end());
    for (std::size_t index = 1; index < paths.size(); ++index) {
        const auto& previous = paths[index - 1U];
        const auto& current = paths[index];
        if (current.size() > previous.size() &&
            current.compare(0, previous.size(), previous) == 0 &&
            current[previous.size()] ==
                static_cast<typename String::value_type>('/')) {
            return true;
        }
    }
    return false;
}

class ExtractIterator final {
public:
    ExtractIterator(mz_zip_archive& archive, std::uint32_t index)
        : archive_(&archive),
          state_(mz_zip_reader_extract_iter_new(&archive, index, 0U)) {}

    ~ExtractIterator() {
        if (state_ != nullptr) mz_zip_reader_extract_iter_free(state_);
    }

    ExtractIterator(const ExtractIterator&) = delete;
    ExtractIterator& operator=(const ExtractIterator&) = delete;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

    std::size_t read(std::span<std::uint8_t> output) {
        return mz_zip_reader_extract_iter_read(state_, output.data(),
                                               output.size());
    }

    bool finish() {
        auto* state = std::exchange(state_, nullptr);
        return state != nullptr &&
               mz_zip_reader_extract_iter_free(state) != MZ_FALSE;
    }

private:
    mz_zip_archive* archive_{};
    mz_zip_reader_extract_iter_state* state_{};
};

}  // namespace

class AvatarPackArchive::Impl final {
public:
    explicit Impl(std::FILE* file) : file_(file) {
        mz_zip_zero_struct(&archive_);
    }

    ~Impl() {
        if (initialized_) mz_zip_reader_end(&archive_);
        if (file_ != nullptr) std::fclose(file_);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    mz_zip_archive archive_{};
    std::FILE* file_{};
    bool initialized_{false};
};

AvatarPackArchive::AvatarPackArchive(
    std::unique_ptr<Impl> implementation,
    std::vector<AvatarPackArchiveEntry> entries)
    : implementation_(std::move(implementation)),
      entries_(std::move(entries)) {}

AvatarPackArchive::AvatarPackArchive(AvatarPackArchive&&) noexcept = default;
AvatarPackArchive& AvatarPackArchive::operator=(
    AvatarPackArchive&&) noexcept = default;
AvatarPackArchive::~AvatarPackArchive() = default;

Result<AvatarPackArchive>
AvatarPackArchive::open(const std::filesystem::path& packagePath) noexcept {
    try {
        auto opened = openImmutableArchive(packagePath);
        if (!opened.hasValue()) return opened.error();
        auto source = std::move(opened).value();
        auto* file = source.file;
        const auto archiveSize = source.size;
        std::unique_ptr<std::FILE, decltype(&std::fclose)> guardedFile{
            file, &std::fclose};
        if (auto envelope = preflightRawZip(guardedFile.get(), archiveSize);
            !envelope.hasValue()) {
            return envelope.error();
        }
        if (!readFileExact(guardedFile.get(), 0U, std::span<std::uint8_t>{})) {
            return archiveError(ErrorCode::IoFailure,
                                "avatar pack could not be rewound",
                                "avatar.pack.archive.open");
        }
        auto implementation = std::make_unique<Impl>(guardedFile.release());
        if (!mz_zip_reader_init_cfile(&implementation->archive_, file,
                                      archiveSize, 0U)) {
            return invalidArchive("avatar pack ZIP structure is invalid",
                                  "avatar.pack.archive.structure");
        }
        implementation->initialized_ = true;

        const auto count =
            mz_zip_reader_get_num_files(&implementation->archive_);
        if (count > kMaximumEntryCount) {
            return invalidArchive("avatar pack contains too many entries",
                                  "avatar.pack.archive.entry-count");
        }

#ifdef _WIN32
        std::unordered_set<std::wstring> collisionKeys;
        std::vector<std::wstring> normalizedPaths;
#else
        std::unordered_set<std::string> collisionKeys;
        std::vector<std::string> normalizedPaths;
#endif
        std::vector<AvatarPackArchiveEntry> entries;
        entries.reserve(count);
        std::uint64_t compressedTotal = 0;
        std::uint64_t expandedTotal = 0;
        for (mz_uint index = 0; index < count; ++index) {
            const auto nameBytes = mz_zip_reader_get_filename(
                &implementation->archive_, index, nullptr, 0U);
            if (nameBytes <= 1U ||
                nameBytes > static_cast<mz_uint>(kMaximumPathBytes + 1U)) {
                return invalidArchive("avatar pack entry path is invalid",
                                      "avatar.pack.archive.path");
            }
            std::vector<char> name(nameBytes);
            if (mz_zip_reader_get_filename(&implementation->archive_, index,
                                           name.data(),
                                           nameBytes) != nameBytes) {
                return invalidArchive(
                    "avatar pack entry path could not be read",
                    "avatar.pack.archive.path");
            }
            std::string path{name.data(), nameBytes - 1U};
            if (!validRelativePath(path)) {
                return invalidArchive("avatar pack entry path is unsafe",
                                      "avatar.pack.archive.path");
            }
            const auto key = collisionKey(path);
            if (!key.has_value() || !collisionKeys.insert(*key).second) {
                return invalidArchive(
                    "avatar pack contains colliding entry paths",
                    "avatar.pack.archive.path");
            }
            normalizedPaths.push_back(*key);

            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&implementation->archive_, index,
                                         &stat)) {
                return invalidArchive("avatar pack entry metadata is invalid",
                                      "avatar.pack.archive.entry");
            }
            if (stat.m_is_encrypted || !stat.m_is_supported ||
                (stat.m_method != 0U && stat.m_method != MZ_DEFLATED)) {
                return invalidArchive("avatar pack uses unsupported entry "
                                      "protection or compression",
                                      "avatar.pack.archive.method");
            }
            if (symlinkLike(stat)) {
                return invalidArchive(
                    "avatar pack contains a directory or link-like entry",
                    "avatar.pack.archive.entry-type");
            }
            if (stat.m_comp_size > kMaximumEntryBytes ||
                stat.m_uncomp_size > kMaximumEntryBytes) {
                return invalidArchive(
                    "avatar pack entry exceeds its size limit",
                    "avatar.pack.archive.entry-size");
            }
            if (compressedTotal > kMaximumAggregateBytes - stat.m_comp_size ||
                expandedTotal > kMaximumAggregateBytes - stat.m_uncomp_size) {
                return invalidArchive(
                    "avatar pack aggregate size exceeds its limit",
                    "avatar.pack.archive.aggregate-size");
            }
            compressedTotal += stat.m_comp_size;
            expandedTotal += stat.m_uncomp_size;
            entries.push_back({.index = index,
                               .path = std::move(path),
                               .compressedBytes = stat.m_comp_size,
                               .uncompressedBytes = stat.m_uncomp_size});
        }
        if (pathPrefixCollision(std::move(normalizedPaths))) {
            return invalidArchive(
                "avatar pack contains file and directory prefix collisions",
                "avatar.pack.archive.path");
        }
        return AvatarPackArchive{std::move(implementation), std::move(entries)};
    } catch (const std::bad_alloc&) {
        return archiveError(ErrorCode::InsufficientStorage,
                            "avatar pack validation allocation failed",
                            "avatar.pack.archive.allocation");
    } catch (const std::length_error&) {
        return invalidEnvelope(
            "avatar pack ZIP envelope requests an invalid allocation");
    } catch (const std::exception&) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack validation failed safely",
                            "avatar.pack.archive.exception");
    } catch (...) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack validation failed safely",
                            "avatar.pack.archive.exception");
    }
}

const std::vector<AvatarPackArchiveEntry>&
AvatarPackArchive::entries() const noexcept {
    return entries_;
}

Result<std::vector<std::uint8_t>>
AvatarPackArchive::read(const AvatarPackArchiveEntry& entry,
                        std::size_t maximumBytes) noexcept {
    try {
        if (entry.uncompressedBytes > maximumBytes ||
            entry.uncompressedBytes >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            return invalidArchive("avatar pack metadata exceeds its size limit",
                                  "avatar.pack.archive.metadata-size");
        }
        ExtractIterator iterator{implementation_->archive_, entry.index};
        if (!iterator.valid()) {
            return invalidArchive("avatar pack metadata could not be opened",
                                  "avatar.pack.archive.read");
        }
        std::vector<std::uint8_t> result;
        result.reserve(static_cast<std::size_t>(entry.uncompressedBytes));
        std::array<std::uint8_t, 64U * 1024U> buffer{};
        for (;;) {
            const auto count = iterator.read(buffer);
            if (count == 0U) break;
            if (count > maximumBytes || result.size() > maximumBytes - count) {
                return invalidArchive(
                    "avatar pack metadata expanded past its limit",
                    "avatar.pack.archive.metadata-size");
            }
            result.insert(result.end(), buffer.begin(),
                          buffer.begin() + static_cast<std::ptrdiff_t>(count));
        }
        if (!iterator.finish() || result.size() != entry.uncompressedBytes) {
            return invalidArchive(
                "avatar pack metadata failed CRC or size validation",
                "avatar.pack.archive.read");
        }
        return result;
    } catch (const std::bad_alloc&) {
        return archiveError(ErrorCode::InsufficientStorage,
                            "avatar pack metadata allocation failed",
                            "avatar.pack.archive.allocation");
    } catch (const std::length_error&) {
        return invalidArchive(
            "avatar pack metadata requests an invalid allocation",
            "avatar.pack.archive.metadata-size");
    } catch (...) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack metadata read failed safely",
                            "avatar.pack.archive.exception");
    }
}

Result<std::string>
AvatarPackArchive::stream(const AvatarPackArchiveEntry& entry,
                          std::uint64_t maximumExpandedBytes,
                          const ChunkWriter& writer) noexcept {
    try {
        if (entry.uncompressedBytes > maximumExpandedBytes) {
            return invalidArchive("avatar pack entry exceeds extraction limit",
                                  "avatar.pack.archive.entry-size");
        }
        ExtractIterator iterator{implementation_->archive_, entry.index};
        if (!iterator.valid()) {
            return invalidArchive("avatar pack entry could not be opened",
                                  "avatar.pack.archive.read");
        }

        core::Sha256 hash;
        std::uint64_t expanded = 0;
        std::array<std::uint8_t, 64U * 1024U> buffer{};
        for (;;) {
            const auto count = iterator.read(buffer);
            if (count == 0U) break;
            if (count > maximumExpandedBytes ||
                expanded > maximumExpandedBytes - count) {
                return invalidArchive(
                    "avatar pack entry expanded past its limit",
                    "avatar.pack.archive.entry-size");
            }
            const auto chunk =
                std::span<const std::uint8_t>{buffer.data(), count};
            auto written = writer(chunk);
            if (!written.hasValue()) return written.error();
            hash.update(chunk);
            expanded += count;
        }
        if (!iterator.finish() || expanded != entry.uncompressedBytes) {
            return invalidArchive(
                "avatar pack entry failed CRC or expanded-size validation",
                "avatar.pack.archive.read");
        }
        return hash.finish();
    } catch (const std::bad_alloc&) {
        return archiveError(ErrorCode::InsufficientStorage,
                            "avatar pack extraction allocation failed",
                            "avatar.pack.archive.allocation");
    } catch (...) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack extraction failed safely",
                            "avatar.pack.archive.exception");
    }
}

}  // namespace creator::avatar_pack_adapter
