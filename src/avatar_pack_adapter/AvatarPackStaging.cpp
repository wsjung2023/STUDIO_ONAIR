#include "avatar_pack_adapter/AvatarPackStaging.h"

#include "avatar_pack_adapter/AvatarPackArchive.h"
#include "core/AppError.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace creator::avatar_pack_adapter {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

AppError stagingError(std::string message, std::string issueCode,
                      ErrorCode code = ErrorCode::IoFailure) {
    return {code, std::move(message), std::move(issueCode),
            "avatar.validation.io"};
}

AppError cleanupError() {
    return stagingError("avatar pack staging cleanup failed",
                        "avatar.pack.staging.cleanup");
}

AppError allocationError() {
    return stagingError("avatar pack staging allocation failed",
                        "avatar.pack.staging.allocation",
                        ErrorCode::InsufficientStorage);
}

std::string randomDirectoryName() {
    std::array<unsigned char, 16> random{};
    randombytes_buf(random.data(), random.size());
    std::array<char, 33> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), random.data(),
                   random.size());
    return ".avatar-pack-" + std::string{encoded.data()};
}

std::optional<std::vector<std::string>> pathComponents(
    std::string_view relativePath) {
    if (relativePath.empty() || relativePath.front() == '/' ||
        relativePath.front() == '\\' ||
        relativePath.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    std::vector<std::string> result;
    std::size_t offset = 0;
    while (offset < relativePath.size()) {
        const auto separator = relativePath.find('/', offset);
        const auto end = separator == std::string_view::npos
                             ? relativePath.size()
                             : separator;
        const auto component = relativePath.substr(offset, end - offset);
        if (component.empty() || component == "." || component == ".." ||
            component.find('\\') != std::string_view::npos) {
            return std::nullopt;
        }
        result.emplace_back(component);
        if (separator == std::string_view::npos) break;
        offset = separator + 1U;
    }
    return result.empty()
               ? std::nullopt
               : std::optional<std::vector<std::string>>{
                     std::move(result)};
}

#ifdef _WIN32

std::optional<std::wstring> wideComponent(std::string_view value) {
    if (value.empty() ||
        value.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
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

bool closeHandle(HANDLE& handle) noexcept {
    if (handle == INVALID_HANDLE_VALUE) return true;
    const auto closing = std::exchange(handle, INVALID_HANDLE_VALUE);
    return CloseHandle(closing) != FALSE;
}

std::optional<std::wstring> finalHandlePath(HANDLE handle) {
    const auto flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const auto required =
        GetFinalPathNameByHandleW(handle, nullptr, 0U, flags);
    if (required == 0U) return std::nullopt;
    std::wstring result(required, L'\0');
    const auto written = GetFinalPathNameByHandleW(
        handle, result.data(), static_cast<DWORD>(result.size()), flags);
    if (written == 0U || written >= result.size()) return std::nullopt;
    result.resize(written);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 4U && result.back() == L'\\')
        result.pop_back();
    return result;
}

bool equalPath(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    return CompareStringOrdinal(
               left.data(), static_cast<int>(left.size()), right.data(),
               static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool childPath(std::wstring_view parent, std::wstring_view child) noexcept {
    return child.size() > parent.size() &&
           child[parent.size()] == L'\\' &&
           CompareStringOrdinal(
               parent.data(), static_cast<int>(parent.size()), child.data(),
               static_cast<int>(parent.size()), TRUE) == CSTR_EQUAL;
}

bool regularDirectory(HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    return GetFileInformationByHandle(handle, &information) != FALSE &&
           (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (information.dwFileAttributes &
            FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

bool regularNewFile(HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    return GetFileInformationByHandle(handle, &information) != FALSE &&
           (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (information.dwFileAttributes &
            FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
           information.nNumberOfLinks == 1U;
}

HANDLE openDirectoryNoDelete(const std::filesystem::path& path) noexcept {
    return CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

#else

bool closeDescriptor(int& descriptor) noexcept {
    if (descriptor < 0) return true;
    const auto closing = std::exchange(descriptor, -1);
    return ::close(closing) == 0;
}

bool directoryDescriptor(int descriptor, struct stat& information) noexcept {
    return ::fstat(descriptor, &information) == 0 &&
           S_ISDIR(information.st_mode);
}

bool sameObjectAt(int parent, const std::string& name,
                  const struct stat& expected) noexcept {
    struct stat actual {};
    return ::fstatat(parent, name.c_str(), &actual,
                     AT_SYMLINK_NOFOLLOW) == 0 &&
           actual.st_dev == expected.st_dev &&
           actual.st_ino == expected.st_ino;
}

#endif

class FileWriter final {
public:
#ifdef _WIN32
    explicit FileWriter(HANDLE handle) : handle_(handle) {}
#else
    explicit FileWriter(int descriptor) : descriptor_(descriptor) {}
#endif

    FileWriter(FileWriter&& other) noexcept {
#ifdef _WIN32
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
#else
        descriptor_ = std::exchange(other.descriptor_, -1);
#endif
    }

    FileWriter& operator=(FileWriter&&) = delete;
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    ~FileWriter() {
#ifdef _WIN32
        (void)closeHandle(handle_);
#else
        (void)closeDescriptor(descriptor_);
#endif
    }

    bool write(std::span<const std::uint8_t> bytes) noexcept {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
#ifdef _WIN32
            const auto amount = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - offset,
                                      std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (WriteFile(handle_, bytes.data() + offset, amount, &written,
                          nullptr) == FALSE ||
                written == 0U) {
                return false;
            }
#else
            const auto amount = std::min<std::size_t>(
                bytes.size() - offset,
                static_cast<std::size_t>(
                    std::numeric_limits<ssize_t>::max()));
            const auto written =
                ::write(descriptor_, bytes.data() + offset, amount);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) return false;
#endif
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    bool flushAndClose() noexcept {
#ifdef _WIN32
        const bool flushed = FlushFileBuffers(handle_) != FALSE;
        const bool closed = closeHandle(handle_);
#else
        bool flushed = false;
        for (;;) {
            if (::fsync(descriptor_) == 0) {
                flushed = true;
                break;
            }
            if (errno != EINTR) break;
        }
        const bool closed = closeDescriptor(descriptor_);
#endif
        return flushed && closed;
    }

private:
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int descriptor_{-1};
#endif
};

}  // namespace

class AvatarPackStaging::Impl final {
public:
    ~Impl() {
        if (!active_) return;
        try {
            const auto ignored = cleanup();
            (void)ignored;
        } catch (...) {
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Impl() = default;

    Result<void> initialize(const std::filesystem::path& parent) {
#ifdef _WIN32
        parentHandle_ = openDirectoryNoDelete(parent);
        if (parentHandle_ == INVALID_HANDLE_VALUE ||
            !regularDirectory(parentHandle_)) {
            return stagingError(
                "avatar pack staging parent is unavailable",
                "avatar.pack.staging.parent");
        }
        auto parentFinal = finalHandlePath(parentHandle_);
        if (!parentFinal.has_value()) {
            return stagingError(
                "avatar pack staging parent could not be verified",
                "avatar.pack.staging.parent");
        }
        parentFinal_ = std::move(*parentFinal);
        for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
            const auto name = randomDirectoryName();
            const auto wideName = wideComponent(name);
            if (!wideName.has_value()) break;
            const auto expected =
                std::filesystem::path{parentFinal_} / *wideName;
            directories_.push_back(
                {.finalPath = expected,
                 .handle = INVALID_HANDLE_VALUE});
            if (CreateDirectoryW(expected.c_str(), nullptr) == FALSE) {
                const auto error = GetLastError();
                directories_.pop_back();
                if (error == ERROR_ALREADY_EXISTS ||
                    error == ERROR_FILE_EXISTS) {
                    continue;
                }
                return stagingError(
                    "avatar pack staging directory creation failed",
                    "avatar.pack.staging.create");
            }
            auto& root = directories_.back();
            root.handle = openDirectoryNoDelete(expected);
            auto rootFinal = finalHandlePath(root.handle);
            if (root.handle == INVALID_HANDLE_VALUE ||
                !regularDirectory(root.handle) ||
                !rootFinal.has_value() ||
                !equalPath(expected.native(), *rootFinal) ||
                !childPath(parentFinal_, *rootFinal)) {
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.create");
            }
            root.finalPath = std::filesystem::path{*rootFinal};
            displayRoot_ = parent / *wideName;
            directoryIndexes_.emplace("", 0U);
            return core::ok();
        }
#else
        parentDescriptor_ =
            ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                       O_CLOEXEC);
        struct stat parentInformation {};
        if (parentDescriptor_ < 0 ||
            !directoryDescriptor(parentDescriptor_, parentInformation)) {
            return stagingError(
                "avatar pack staging parent is unavailable",
                "avatar.pack.staging.parent");
        }
        for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
            const auto name = randomDirectoryName();
            directories_.push_back(
                {.parentIndex = kNoParent, .name = name, .descriptor = -1});
            if (::mkdirat(parentDescriptor_, name.c_str(), 0700) != 0) {
                directories_.pop_back();
                if (errno == EEXIST) continue;
                return stagingError(
                    "avatar pack staging directory creation failed",
                    "avatar.pack.staging.create");
            }
            auto& root = directories_.back();
            root.descriptor = ::openat(
                parentDescriptor_, name.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat rootInformation {};
            if (root.descriptor < 0 ||
                !directoryDescriptor(root.descriptor, rootInformation) ||
                !sameObjectAt(parentDescriptor_, name, rootInformation)) {
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.create");
            }
            std::error_code error;
            auto absoluteParent = std::filesystem::absolute(parent, error);
            displayRoot_ =
                (error ? parent : absoluteParent) / name;
            directoryIndexes_.emplace("", 0U);
            return core::ok();
        }
#endif
        return stagingError(
            "avatar pack staging directory creation failed",
            "avatar.pack.staging.create");
    }

    Result<FileWriter> createFile(std::string_view relativePath) {
        const auto components = pathComponents(relativePath);
        if (!components.has_value()) {
            return stagingError("avatar pack staging path is invalid",
                                "avatar.pack.staging.path",
                                ErrorCode::InvalidArgument);
        }
        auto parent = ensureParent(*components);
        if (!parent.hasValue()) return parent.error();
#ifdef _WIN32
        const auto leaf = wideComponent(components->back());
        if (!leaf.has_value()) {
            return stagingError("avatar pack staging path is invalid",
                                "avatar.pack.staging.path",
                                ErrorCode::InvalidArgument);
        }
        const auto expected =
            directories_[parent.value()].finalPath / *leaf;
        files_.push_back(expected);
        HANDLE handle = CreateFileW(
            expected.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0,
            nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            files_.pop_back();
            return stagingError(
                "avatar pack staging file creation failed",
                "avatar.pack.staging.file");
        }
        const auto final = finalHandlePath(handle);
        if (!regularNewFile(handle) || !final.has_value() ||
            !equalPath(expected.native(), *final) ||
            !childPath(directories_.front().finalPath.native(), *final)) {
            (void)closeHandle(handle);
            return stagingError(
                "avatar pack staging file verification failed",
                "avatar.pack.staging.file");
        }
        return FileWriter{handle};
#else
        const auto& leaf = components->back();
        const auto parentDescriptor =
            directories_[parent.value()].descriptor;
        files_.push_back(
            {.parentIndex = parent.value(), .name = leaf});
        const int descriptor = ::openat(
            parentDescriptor, leaf.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            files_.pop_back();
            return stagingError(
                "avatar pack staging file creation failed",
                "avatar.pack.staging.file");
        }
        struct stat information {};
        if (::fstat(descriptor, &information) != 0 ||
            !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
            !sameObjectAt(parentDescriptor, leaf, information)) {
            int closing = descriptor;
            (void)closeDescriptor(closing);
            return stagingError(
                "avatar pack staging file verification failed",
                "avatar.pack.staging.file");
        }
        return FileWriter{descriptor};
#endif
    }

    Result<void> writeNewFile(std::string_view relativePath,
                              std::span<const std::uint8_t> bytes) {
        auto created = createFile(relativePath);
        if (!created.hasValue()) return created.error();
        auto writer = std::move(created).value();
        if (!writer.write(bytes)) {
            return stagingError("avatar pack staging file write failed",
                                "avatar.pack.staging.write");
        }
        if (!writer.flushAndClose()) {
            return stagingError(
                "avatar pack staging file flush or close failed",
                "avatar.pack.staging.flush");
        }
        return core::ok();
    }

    Result<std::string> extractNewFile(
        AvatarPackArchive& archive, const AvatarPackArchiveEntry& entry,
        std::uint64_t maximumExpandedBytes) {
        auto created = createFile(entry.path);
        if (!created.hasValue()) return created.error();
        auto writer = std::move(created).value();
        auto streamed = archive.stream(
            entry, maximumExpandedBytes,
            [&writer](std::span<const std::uint8_t> bytes) -> Result<void> {
                if (!writer.write(bytes)) {
                    return stagingError(
                        "avatar pack staging file write failed",
                        "avatar.pack.staging.write");
                }
                return core::ok();
            });
        if (!streamed.hasValue()) return streamed.error();
        if (!writer.flushAndClose()) {
            return stagingError(
                "avatar pack staging file flush or close failed",
                "avatar.pack.staging.flush");
        }
        return std::move(streamed).value();
    }

    Result<void> cleanup() {
        if (!active_) return core::ok();
        bool succeeded = true;
#ifdef _WIN32
        for (auto iterator = files_.rbegin(); iterator != files_.rend();
             ++iterator) {
            if (DeleteFileW(iterator->c_str()) == FALSE) succeeded = false;
        }
        for (auto iterator = directories_.rbegin();
             iterator != directories_.rend(); ++iterator) {
            if (!closeHandle(iterator->handle)) succeeded = false;
            if (RemoveDirectoryW(iterator->finalPath.c_str()) == FALSE)
                succeeded = false;
        }
        if (!closeHandle(parentHandle_)) succeeded = false;
#else
        for (auto iterator = files_.rbegin(); iterator != files_.rend();
             ++iterator) {
            const auto parent =
                directories_[iterator->parentIndex].descriptor;
            if (::unlinkat(parent, iterator->name.c_str(), 0) != 0)
                succeeded = false;
        }
        for (auto index = directories_.size(); index > 0U; --index) {
            auto& directory = directories_[index - 1U];
            if (!closeDescriptor(directory.descriptor)) succeeded = false;
            const auto parent =
                directory.parentIndex == kNoParent
                    ? parentDescriptor_
                    : directories_[directory.parentIndex].descriptor;
            if (::unlinkat(parent, directory.name.c_str(), AT_REMOVEDIR) !=
                0) {
                succeeded = false;
            }
        }
        if (!closeDescriptor(parentDescriptor_)) succeeded = false;
#endif
        active_ = false;
        return succeeded ? core::ok() : Result<void>{cleanupError()};
    }

    Result<std::filesystem::path> finish() {
        if (!active_) {
            return stagingError("avatar pack staging is no longer active",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
        bool succeeded = true;
#ifdef _WIN32
        for (auto iterator = directories_.rbegin();
             iterator != directories_.rend(); ++iterator) {
            if (!closeHandle(iterator->handle)) succeeded = false;
        }
        if (!closeHandle(parentHandle_)) succeeded = false;
#else
        for (auto iterator = directories_.rbegin();
             iterator != directories_.rend(); ++iterator) {
            if (!closeDescriptor(iterator->descriptor)) succeeded = false;
        }
        if (!closeDescriptor(parentDescriptor_)) succeeded = false;
#endif
        if (!succeeded) {
            return stagingError(
                "avatar pack staging handles could not be closed",
                "avatar.pack.staging.finish");
        }
        active_ = false;
        return displayRoot_;
    }

private:
#ifdef _WIN32
    struct DirectoryRecord final {
        std::filesystem::path finalPath;
        HANDLE handle{INVALID_HANDLE_VALUE};
    };
#else
    static constexpr std::size_t kNoParent =
        std::numeric_limits<std::size_t>::max();

    struct DirectoryRecord final {
        std::size_t parentIndex{kNoParent};
        std::string name;
        int descriptor{-1};
    };

    struct FileRecord final {
        std::size_t parentIndex{};
        std::string name;
    };
#endif

    Result<std::size_t> ensureParent(
        const std::vector<std::string>& components) {
        std::size_t current = 0U;
        std::string key;
        for (std::size_t index = 0; index + 1U < components.size();
             ++index) {
            if (!key.empty()) key.push_back('/');
            key += components[index];
            if (const auto found = directoryIndexes_.find(key);
                found != directoryIndexes_.end()) {
                current = found->second;
                continue;
            }
#ifdef _WIN32
            const auto component = wideComponent(components[index]);
            if (!component.has_value()) {
                return stagingError("avatar pack staging path is invalid",
                                    "avatar.pack.staging.path",
                                    ErrorCode::InvalidArgument);
            }
            const auto expected =
                directories_[current].finalPath / *component;
            directories_.push_back(
                {.finalPath = expected,
                 .handle = INVALID_HANDLE_VALUE});
            if (CreateDirectoryW(expected.c_str(), nullptr) == FALSE) {
                directories_.pop_back();
                return stagingError(
                    "avatar pack staging directory creation failed",
                    "avatar.pack.staging.directory");
            }
            auto& created = directories_.back();
            created.handle = openDirectoryNoDelete(expected);
            const auto final = finalHandlePath(created.handle);
            if (created.handle == INVALID_HANDLE_VALUE ||
                !regularDirectory(created.handle) ||
                !final.has_value() ||
                !equalPath(expected.native(), *final) ||
                !childPath(directories_.front().finalPath.native(),
                           *final)) {
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.directory");
            }
            created.finalPath = std::filesystem::path{*final};
#else
            const auto parentDescriptor =
                directories_[current].descriptor;
            directories_.push_back(
                {.parentIndex = current,
                 .name = components[index],
                 .descriptor = -1});
            if (::mkdirat(parentDescriptor, components[index].c_str(),
                          0700) != 0) {
                directories_.pop_back();
                return stagingError(
                    "avatar pack staging directory creation failed",
                    "avatar.pack.staging.directory");
            }
            auto& created = directories_.back();
            created.descriptor = ::openat(
                parentDescriptor, created.name.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat information {};
            if (created.descriptor < 0 ||
                !directoryDescriptor(created.descriptor, information) ||
                !sameObjectAt(parentDescriptor, created.name,
                              information)) {
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.directory");
            }
#endif
            current = directories_.size() - 1U;
            directoryIndexes_.emplace(key, current);
        }
        return current;
    }

    bool active_{true};
    std::filesystem::path displayRoot_;
    std::unordered_map<std::string, std::size_t> directoryIndexes_;
    std::vector<DirectoryRecord> directories_;
#ifdef _WIN32
    HANDLE parentHandle_{INVALID_HANDLE_VALUE};
    std::wstring parentFinal_;
    std::vector<std::filesystem::path> files_;
#else
    int parentDescriptor_{-1};
    std::vector<FileRecord> files_;
#endif
};

AvatarPackStaging::AvatarPackStaging(
    std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

AvatarPackStaging::AvatarPackStaging(AvatarPackStaging&&) noexcept = default;
AvatarPackStaging& AvatarPackStaging::operator=(
    AvatarPackStaging&&) noexcept = default;
AvatarPackStaging::~AvatarPackStaging() = default;

Result<AvatarPackStaging> AvatarPackStaging::create(
    const std::filesystem::path& parent) {
    std::unique_ptr<Impl> implementation;
    try {
        implementation = std::make_unique<Impl>();
        auto initialized = implementation->initialize(parent);
        if (!initialized.hasValue()) {
            const auto original = initialized.error();
            auto cleaned = implementation->cleanup();
            return cleaned.hasValue() ? Result<AvatarPackStaging>{original}
                                      : Result<AvatarPackStaging>{
                                            cleaned.error()};
        }
        return AvatarPackStaging{std::move(implementation)};
    } catch (const std::bad_alloc&) {
        if (implementation) {
            try {
                auto cleaned = implementation->cleanup();
                if (!cleaned.hasValue()) return cleaned.error();
            } catch (...) {
                return cleanupError();
            }
        }
        return allocationError();
    } catch (const std::exception&) {
        if (implementation) {
            try {
                auto cleaned = implementation->cleanup();
                if (!cleaned.hasValue()) return cleaned.error();
            } catch (...) {
                return cleanupError();
            }
        }
        return stagingError("avatar pack staging failed safely",
                            "avatar.pack.staging.exception");
    }
}

Result<void> AvatarPackStaging::writeNewFile(
    std::string_view relativePath,
    std::span<const std::uint8_t> bytes) {
    try {
        return implementation_->writeNewFile(relativePath, bytes);
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (const std::exception&) {
        return stagingError("avatar pack staging write failed safely",
                            "avatar.pack.staging.exception");
    }
}

Result<std::string> AvatarPackStaging::extractNewFile(
    AvatarPackArchive& archive, const AvatarPackArchiveEntry& entry,
    std::uint64_t maximumExpandedBytes) {
    try {
        return implementation_->extractNewFile(
            archive, entry, maximumExpandedBytes);
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (const std::exception&) {
        return stagingError("avatar pack extraction failed safely",
                            "avatar.pack.staging.exception");
    }
}

Result<void> AvatarPackStaging::cleanup() {
    try {
        return implementation_->cleanup();
    } catch (...) {
        return cleanupError();
    }
}

Result<std::filesystem::path> AvatarPackStaging::finish() {
    try {
        return implementation_->finish();
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (const std::exception&) {
        return stagingError("avatar pack staging finish failed safely",
                            "avatar.pack.staging.exception");
    }
}

}  // namespace creator::avatar_pack_adapter
