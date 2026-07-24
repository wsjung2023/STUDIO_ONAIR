#include "avatar_pack_adapter/AvatarPackArchive.h"

#include "core/AppError.h"
#include "core/Sha256.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
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
    return std::find(kReserved.begin(), kReserved.end(), stem) !=
           kReserved.end();
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

class ExclusiveOutput final {
public:
    explicit ExclusiveOutput(const std::filesystem::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
        descriptor_ =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                   0666);
#endif
    }

    ~ExclusiveOutput() { closeIgnoringErrors(); }

    ExclusiveOutput(const ExclusiveOutput&) = delete;
    ExclusiveOutput& operator=(const ExclusiveOutput&) = delete;

    [[nodiscard]] bool valid() const noexcept {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }

    bool write(std::span<const std::uint8_t> bytes) {
        std::size_t written = 0;
        while (written < bytes.size()) {
#ifdef _WIN32
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - written,
                std::numeric_limits<DWORD>::max()));
            DWORD count = 0;
            if (!WriteFile(handle_, bytes.data() + written, chunk, &count,
                           nullptr) ||
                count == 0U) {
                return false;
            }
#else
            const auto chunk = std::min<std::size_t>(
                bytes.size() - written,
                static_cast<std::size_t>(
                    std::numeric_limits<ssize_t>::max()));
            const auto count =
                ::write(descriptor_, bytes.data() + written, chunk);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return false;
#endif
            written += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool flushAndClose() {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) return false;
        const auto handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        return CloseHandle(handle) != FALSE;
#else
        if (::fsync(descriptor_) != 0) return false;
        const auto descriptor = std::exchange(descriptor_, -1);
        return ::close(descriptor) == 0;
#endif
    }

private:
    void closeIgnoringErrors() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
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

Result<AvatarPackArchive> AvatarPackArchive::open(
    const std::filesystem::path& packagePath) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(packagePath, error);
    if (error) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack could not be inspected",
                            "avatar.pack.archive.open");
    }
    if (!std::filesystem::exists(status)) {
        return archiveError(ErrorCode::NotFound,
                            "avatar pack does not exist",
                            "avatar.pack.archive.open");
    }
    if (!std::filesystem::is_regular_file(status)) {
        return invalidArchive("avatar pack must be a regular file",
                              "avatar.pack.archive.open");
    }
    const auto archiveSize = std::filesystem::file_size(packagePath, error);
    if (error) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack size could not be read",
                            "avatar.pack.archive.open");
    }

    std::FILE* file = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&file, packagePath.c_str(), L"rb") != 0) file = nullptr;
#else
    file = std::fopen(packagePath.c_str(), "rb");
#endif
    if (file == nullptr) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack could not be opened",
                            "avatar.pack.archive.open");
    }
    auto implementation = std::make_unique<Impl>(file);
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
                                       name.data(), nameBytes) != nameBytes) {
            return invalidArchive("avatar pack entry path could not be read",
                                  "avatar.pack.archive.path");
        }
        std::string path{name.data(), nameBytes - 1U};
        if (!validRelativePath(path)) {
            return invalidArchive("avatar pack entry path is unsafe",
                                  "avatar.pack.archive.path");
        }
        const auto key = collisionKey(path);
        if (!key.has_value() ||
            !collisionKeys.insert(*key).second) {
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
            return invalidArchive(
                "avatar pack uses unsupported entry protection or compression",
                "avatar.pack.archive.method");
        }
        if (symlinkLike(stat)) {
            return invalidArchive(
                "avatar pack contains a directory or link-like entry",
                "avatar.pack.archive.entry-type");
        }
        if (stat.m_comp_size > kMaximumEntryBytes ||
            stat.m_uncomp_size > kMaximumEntryBytes) {
            return invalidArchive("avatar pack entry exceeds its size limit",
                                  "avatar.pack.archive.entry-size");
        }
        if (compressedTotal >
                kMaximumAggregateBytes - stat.m_comp_size ||
            expandedTotal >
                kMaximumAggregateBytes - stat.m_uncomp_size) {
            return invalidArchive(
                "avatar pack aggregate size exceeds its limit",
                "avatar.pack.archive.aggregate-size");
        }
        compressedTotal += stat.m_comp_size;
        expandedTotal += stat.m_uncomp_size;
        entries.push_back(
            {.index = index,
             .path = std::move(path),
             .compressedBytes = stat.m_comp_size,
             .uncompressedBytes = stat.m_uncomp_size});
    }
    if (pathPrefixCollision(std::move(normalizedPaths))) {
        return invalidArchive(
            "avatar pack contains file and directory prefix collisions",
            "avatar.pack.archive.path");
    }
    return AvatarPackArchive{std::move(implementation),
                             std::move(entries)};
}

const std::vector<AvatarPackArchiveEntry>& AvatarPackArchive::entries()
    const noexcept {
    return entries_;
}

Result<std::vector<std::uint8_t>> AvatarPackArchive::read(
    const AvatarPackArchiveEntry& entry, std::size_t maximumBytes) {
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
        if (count > maximumBytes ||
            result.size() > maximumBytes - count) {
            return invalidArchive(
                "avatar pack metadata expanded past its limit",
                "avatar.pack.archive.metadata-size");
        }
        result.insert(result.end(), buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(count));
    }
    if (!iterator.finish() ||
        result.size() != entry.uncompressedBytes) {
        return invalidArchive(
            "avatar pack metadata failed CRC or size validation",
            "avatar.pack.archive.read");
    }
    return result;
}

Result<std::string> AvatarPackArchive::extractToNewFile(
    const AvatarPackArchiveEntry& entry,
    const std::filesystem::path& destination,
    std::uint64_t maximumExpandedBytes) {
    if (entry.uncompressedBytes > maximumExpandedBytes) {
        return invalidArchive("avatar pack entry exceeds extraction limit",
                              "avatar.pack.archive.entry-size");
    }
    std::error_code error;
    const auto parent = destination.parent_path();
    std::filesystem::create_directories(parent, error);
    if (error) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack staging directory could not be created",
                            "avatar.pack.extract.directory");
    }
    ExclusiveOutput output{destination};
    if (!output.valid()) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack staging file could not be created",
                            "avatar.pack.extract.create");
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
        if (!output.write(chunk)) {
            return archiveError(ErrorCode::IoFailure,
                                "avatar pack staging file write failed",
                                "avatar.pack.extract.write");
        }
        hash.update(chunk);
        expanded += count;
    }
    if (!iterator.finish() || expanded != entry.uncompressedBytes) {
        return invalidArchive(
            "avatar pack entry failed CRC or expanded-size validation",
            "avatar.pack.archive.read");
    }
    if (!output.flushAndClose()) {
        return archiveError(ErrorCode::IoFailure,
                            "avatar pack staging file flush or close failed",
                            "avatar.pack.extract.flush");
    }
    return hash.finish();
}

}  // namespace creator::avatar_pack_adapter
