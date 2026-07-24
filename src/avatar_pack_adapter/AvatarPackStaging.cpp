#include "avatar_pack_adapter/AvatarPackStaging.h"

#include "avatar_pack_adapter/AvatarPackArchive.h"
#include "avatar_pack_adapter/AvatarPackPromotionInternal.h"
#include "core/AppError.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Aclapi.h>
#include <Windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif
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

AppError promotionError(
    ErrorCode code = ErrorCode::IoFailure) {
    return stagingError(
        code == ErrorCode::AlreadyExists
            ? "avatar pack promotion destination already exists"
            : "avatar pack staging promotion failed",
        "avatar.pack.staging.promote", code);
}

[[maybe_unused]] AppError promotionErrorAt(std::string_view stage) {
    return stagingError(
        "avatar pack staging promotion failed at " + std::string{stage},
        "avatar.pack.staging.promote", ErrorCode::IoFailure);
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

class ScopedHandle final {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() { (void)close(); }

    ScopedHandle(ScopedHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this == &other) return *this;
        (void)close();
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        return *this;
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] bool close() noexcept { return closeHandle(handle_); }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class ScopedFindHandle final {
public:
    explicit ScopedFindHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedFindHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) (void)FindClose(handle_);
    }

    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

struct WindowsPathLess final {
    bool operator()(const std::wstring& left,
                    const std::wstring& right) const noexcept {
        return CompareStringOrdinal(
                   left.data(), static_cast<int>(left.size()), right.data(),
                   static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

bool regularDirectory(HANDLE handle) noexcept;

std::optional<std::vector<std::uint8_t>> currentUserSid() {
    ScopedHandle token;
    HANDLE rawToken = INVALID_HANDLE_VALUE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) ==
        FALSE) {
        return std::nullopt;
    }
    token = ScopedHandle{rawToken};
    DWORD required = 0U;
    (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0U,
                              &required);
    if (required == 0U || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return std::nullopt;
    std::vector<std::uint8_t> information(required);
    if (GetTokenInformation(token.get(), TokenUser, information.data(),
                            required, &required) == FALSE) {
        return std::nullopt;
    }
    const auto* user =
        reinterpret_cast<const TOKEN_USER*>(information.data());
    const auto sidLength = GetLengthSid(user->User.Sid);
    if (sidLength == 0U) return std::nullopt;
    std::vector<std::uint8_t> sid(sidLength);
    if (CopySid(sidLength, sid.data(), user->User.Sid) == FALSE)
        return std::nullopt;
    return sid;
}

bool trustedPrivateDirectory(HANDLE handle) {
    if (!regularDirectory(handle)) return false;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(
            handle, SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &dacl, nullptr, &descriptor) !=
        ERROR_SUCCESS) {
        return false;
    }
    const auto releaseDescriptor = [&] {
        if (descriptor != nullptr) (void)LocalFree(descriptor);
    };
    auto currentUser = currentUserSid();
    if (!currentUser.has_value() || owner == nullptr ||
        IsValidSid(owner) == FALSE ||
        EqualSid(owner, currentUser->data()) == FALSE || dacl == nullptr) {
        releaseDescriptor();
        return false;
    }

    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> systemSid{};
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> administratorsSid{};
    DWORD systemBytes = static_cast<DWORD>(systemSid.size());
    DWORD administratorsBytes =
        static_cast<DWORD>(administratorsSid.size());
    if (CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid.data(),
                           &systemBytes) == FALSE ||
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                           administratorsSid.data(),
                           &administratorsBytes) == FALSE) {
        releaseDescriptor();
        return false;
    }

    ULONG entryCount = 0U;
    PEXPLICIT_ACCESSW entries = nullptr;
    if (GetExplicitEntriesFromAclW(dacl, &entryCount, &entries) !=
        ERROR_SUCCESS) {
        releaseDescriptor();
        return false;
    }
    bool trusted = true;
    constexpr DWORD kDangerousRights =
        FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
        FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC |
        WRITE_OWNER;
    constexpr GENERIC_MAPPING kFileMapping{
        FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE,
        FILE_ALL_ACCESS};
    for (ULONG index = 0U; index < entryCount; ++index) {
        const auto& entry = entries[index];
        if (entry.grfAccessMode != GRANT_ACCESS &&
            entry.grfAccessMode != SET_ACCESS) {
            continue;
        }
        DWORD rights = entry.grfAccessPermissions;
        auto mapping = kFileMapping;
        MapGenericMask(&rights, &mapping);
        if ((rights & kDangerousRights) == 0U) continue;
        if (entry.Trustee.TrusteeForm != TRUSTEE_IS_SID ||
            entry.Trustee.ptstrName == nullptr) {
            trusted = false;
            break;
        }
        auto* sid = reinterpret_cast<PSID>(
            entry.Trustee.ptstrName);
        if (IsValidSid(sid) == FALSE ||
            (EqualSid(sid, currentUser->data()) == FALSE &&
             EqualSid(sid, systemSid.data()) == FALSE &&
             EqualSid(sid, administratorsSid.data()) == FALSE)) {
            trusted = false;
            break;
        }
    }
    if (entries != nullptr) (void)LocalFree(entries);
    releaseDescriptor();
    return trusted;
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

std::wstring renamePath(const std::filesystem::path& path) {
    const auto value = path.native();
    constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
    constexpr std::wstring_view kExtendedUncPrefix = L"\\\\?\\UNC\\";
    if (value.starts_with(kExtendedUncPrefix)) {
        return L"\\\\" + value.substr(kExtendedUncPrefix.size());
    }
    if (value.starts_with(kExtendedPrefix)) {
        return value.substr(kExtendedPrefix.size());
    }
    return value;
}

struct ObjectIdentity final {
    DWORD volumeSerial{};
    DWORD indexHigh{};
    DWORD indexLow{};

    friend bool operator==(const ObjectIdentity&,
                           const ObjectIdentity&) = default;
};

ObjectIdentity identityOf(
    const BY_HANDLE_FILE_INFORMATION& information) noexcept {
    return {.volumeSerial = information.dwVolumeSerialNumber,
            .indexHigh = information.nFileIndexHigh,
            .indexLow = information.nFileIndexLow};
}

std::optional<BY_HANDLE_FILE_INFORMATION> handleInformation(
    HANDLE handle) noexcept {
    BY_HANDLE_FILE_INFORMATION information{};
    if (handle == INVALID_HANDLE_VALUE ||
        GetFileInformationByHandle(handle, &information) == FALSE) {
        return std::nullopt;
    }
    return information;
}

bool regularDirectory(HANDLE handle) noexcept {
    const auto information = handleInformation(handle);
    return information.has_value() &&
           (information->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (information->dwFileAttributes &
            FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

bool regularNewFile(HANDLE handle) noexcept {
    const auto information = handleInformation(handle);
    return information.has_value() &&
           (information->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (information->dwFileAttributes &
            FILE_ATTRIBUTE_REPARSE_POINT) == 0U &&
           information->nNumberOfLinks == 1U;
}

HANDLE openDirectoryNoDelete(const std::filesystem::path& path) noexcept {
    return CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

HANDLE openDirectoryForIdentity(
    const std::filesystem::path& path) noexcept {
    return CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

#else

bool closeDescriptor(int& descriptor) noexcept {
    if (descriptor < 0) return true;
    const auto closing = std::exchange(descriptor, -1);
    return ::close(closing) == 0;
}

class ScopedDescriptor final {
public:
    ScopedDescriptor() = default;
    explicit ScopedDescriptor(int descriptor) noexcept
        : descriptor_(descriptor) {}
    ~ScopedDescriptor() { (void)close(); }

    ScopedDescriptor(ScopedDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
        if (this == &other) return *this;
        (void)close();
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }

    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }
    [[nodiscard]] bool close() noexcept {
        return closeDescriptor(descriptor_);
    }

private:
    int descriptor_{-1};
};

class ScopedDirectoryStream final {
public:
    explicit ScopedDirectoryStream(DIR* stream) noexcept : stream_(stream) {}
    ~ScopedDirectoryStream() { (void)close(); }

    ScopedDirectoryStream(const ScopedDirectoryStream&) = delete;
    ScopedDirectoryStream& operator=(const ScopedDirectoryStream&) = delete;

    [[nodiscard]] DIR* get() const noexcept { return stream_; }
    [[nodiscard]] bool close() noexcept {
        if (stream_ == nullptr) return true;
        auto* closing = std::exchange(stream_, nullptr);
        return ::closedir(closing) == 0;
    }

private:
    DIR* stream_{nullptr};
};

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

bool sameObject(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool trustedPrivateDirectory(
    const struct stat& information) noexcept {
    return S_ISDIR(information.st_mode) &&
           information.st_uid == ::geteuid() &&
           (information.st_mode & 0077U) == 0U;
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
            !trustedPrivateDirectory(parentHandle_)) {
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
                 .handle = INVALID_HANDLE_VALUE,
                 .identity = std::nullopt});
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
            const auto rootInformation = handleInformation(root.handle);
            if (root.handle == INVALID_HANDLE_VALUE ||
                !regularDirectory(root.handle) ||
                !rootFinal.has_value() ||
                !rootInformation.has_value() ||
                !equalPath(expected.native(), *rootFinal) ||
                !childPath(parentFinal_, *rootFinal)) {
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.create");
            }
            root.finalPath = std::filesystem::path{*rootFinal};
            root.identity = identityOf(*rootInformation);
            directoryIndexes_.emplace("", 0U);
            return core::ok();
        }
#else
        parentDescriptor_ =
            ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                       O_CLOEXEC);
        struct stat parentInformation {};
        if (parentDescriptor_ < 0 ||
            !directoryDescriptor(parentDescriptor_, parentInformation) ||
            !trustedPrivateDirectory(parentInformation)) {
            return stagingError(
                "avatar pack staging parent is not private",
                "avatar.pack.staging.parent");
        }
        parentIdentity_ = parentInformation;
        for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
            const auto name = randomDirectoryName();
            directories_.push_back(
                {.parentIndex = kNoParent,
                 .name = name,
                 .relativePath = {},
                 .descriptor = -1,
                 .identity = std::nullopt});
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
            root.identity = rootInformation;
            directoryIndexes_.emplace("", 0U);
            return core::ok();
        }
#endif
        return stagingError(
            "avatar pack staging directory creation failed",
            "avatar.pack.staging.create");
    }

    Result<FileWriter> createFile(std::string_view relativePath) {
        if (!active_ || sealed_) {
            return stagingError("avatar pack staging is not writable",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
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
        files_.push_back({.relativePath = std::string{relativePath},
                          .finalPath = expected,
                          .identity = std::nullopt});
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
        const auto information = handleInformation(handle);
        if (!regularNewFile(handle) || !final.has_value() ||
            !information.has_value() ||
            !equalPath(expected.native(), *final) ||
            !childPath(directories_.front().finalPath.native(), *final)) {
            (void)closeHandle(handle);
            return stagingError(
                "avatar pack staging file verification failed",
                "avatar.pack.staging.file");
        }
        files_.back().identity = identityOf(*information);
        return FileWriter{handle};
#else
        const auto& leaf = components->back();
        int parentDescriptor = openDirectoryRelative(
            directories_[parent.value()].relativePath);
        if (parentDescriptor < 0) {
            return stagingError(
                "avatar pack staging directory verification failed",
                "avatar.pack.staging.directory");
        }
        files_.push_back(
            {.parentIndex = parent.value(),
             .name = leaf,
             .relativePath = std::string{relativePath},
             .identity = std::nullopt});
        const int descriptor = ::openat(
            parentDescriptor, leaf.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            (void)closeDescriptor(parentDescriptor);
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
            (void)closeDescriptor(parentDescriptor);
            return stagingError(
                "avatar pack staging file verification failed",
                "avatar.pack.staging.file");
        }
        files_.back().identity = information;
        if (!closeDescriptor(parentDescriptor)) {
            int closing = descriptor;
            (void)closeDescriptor(closing);
            return stagingError(
                "avatar pack staging directory close failed",
                "avatar.pack.staging.identity");
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

    Result<void> seal() {
        if (!active_ || sealed_ || directories_.empty()) {
            return stagingError("avatar pack staging cannot be sealed",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
#ifdef _WIN32
        if (!pathMatches(directories_.front())) {
            return stagingError("avatar pack staging root identity changed",
                                "avatar.pack.staging.identity");
        }
        for (auto& file : files_) {
            HANDLE readHandle = CreateFileW(
                file.finalPath.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                nullptr);
            const auto information = handleInformation(readHandle);
            const auto final = finalHandlePath(readHandle);
            LARGE_INTEGER size{};
            if (readHandle == INVALID_HANDLE_VALUE ||
                !regularNewFile(readHandle) || !information.has_value() ||
                !file.identity.has_value() ||
                identityOf(*information) != *file.identity ||
                !final.has_value() ||
                !equalPath(file.finalPath.native(), *final) ||
                !childPath(directories_.front().finalPath.native(), *final) ||
                GetFileSizeEx(readHandle, &size) == FALSE ||
                size.QuadPart < 0) {
                (void)closeHandle(readHandle);
                return stagingError("avatar pack staging file identity changed",
                                    "avatar.pack.staging.identity");
            }
            crypto_hash_sha256_state hash{};
            crypto_hash_sha256_init(&hash);
            std::array<unsigned char, 64U * 1024U> buffer{};
            std::uint64_t total = 0U;
            for (;;) {
                DWORD received = 0U;
                if (ReadFile(readHandle, buffer.data(),
                             static_cast<DWORD>(buffer.size()), &received,
                             nullptr) == FALSE) {
                    (void)closeHandle(readHandle);
                    return stagingError(
                        "avatar pack staging file read failed",
                        "avatar.pack.staging.read");
                }
                if (received == 0U) break;
                crypto_hash_sha256_update(&hash, buffer.data(), received);
                total += received;
            }
            const auto after = handleInformation(readHandle);
            LARGE_INTEGER afterSize{};
            if (!after.has_value() ||
                identityOf(*after) != *file.identity ||
                GetFileSizeEx(readHandle, &afterSize) == FALSE ||
                afterSize.QuadPart < 0 ||
                total != static_cast<std::uint64_t>(afterSize.QuadPart) ||
                afterSize.QuadPart != size.QuadPart ||
                !closeHandle(readHandle)) {
                return stagingError("avatar pack staging file identity changed",
                                    "avatar.pack.staging.identity");
            }
            file.sealedSize = total;
            crypto_hash_sha256_final(&hash, file.sealedHash.data());
        }
        for (std::size_t index = 1U; index < directories_.size(); ++index) {
            if (!closeHandle(directories_[index].handle)) {
                return stagingError(
                    "avatar pack staging directory close failed",
                    "avatar.pack.staging.identity");
            }
        }
#else
        const auto& root = directories_.front();
        if (!root.identity.has_value() ||
            !sameObjectAt(parentDescriptor_, root.name, *root.identity)) {
            return stagingError("avatar pack staging root identity changed",
                                "avatar.pack.staging.identity");
        }
        for (auto& file : files_) {
            int parent = openDirectoryRelative(
                directories_[file.parentIndex].relativePath);
            int readDescriptor = ::openat(
                parent, file.name.c_str(),
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            struct stat information{};
            if (readDescriptor < 0 ||
                ::fstat(readDescriptor, &information) != 0 ||
                !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
                !file.identity.has_value() ||
                !sameObject(information, *file.identity) ||
                !sameObjectAt(parent, file.name, information)) {
                (void)closeDescriptor(readDescriptor);
                (void)closeDescriptor(parent);
                return stagingError("avatar pack staging file identity changed",
                                    "avatar.pack.staging.identity");
            }
            if (!closeDescriptor(parent)) {
                (void)closeDescriptor(readDescriptor);
                return stagingError(
                    "avatar pack staging directory close failed",
                    "avatar.pack.staging.identity");
            }
            crypto_hash_sha256_state hash{};
            crypto_hash_sha256_init(&hash);
            std::array<unsigned char, 64U * 1024U> buffer{};
            std::uint64_t total = 0U;
            for (;;) {
                const auto received =
                    ::read(readDescriptor, buffer.data(), buffer.size());
                if (received < 0 && errno == EINTR) continue;
                if (received < 0) {
                    (void)closeDescriptor(readDescriptor);
                    return stagingError(
                        "avatar pack staging file read failed",
                        "avatar.pack.staging.read");
                }
                if (received == 0) break;
                crypto_hash_sha256_update(
                    &hash, buffer.data(),
                    static_cast<unsigned long long>(received));
                total += static_cast<std::uint64_t>(received);
            }
            struct stat after {};
            if (::fstat(readDescriptor, &after) != 0 ||
                !sameObject(after, *file.identity) ||
                after.st_size < 0 ||
                total != static_cast<std::uint64_t>(after.st_size) ||
                after.st_size != information.st_size ||
                !closeDescriptor(readDescriptor)) {
                return stagingError("avatar pack staging file identity changed",
                                    "avatar.pack.staging.identity");
            }
            file.sealedSize = total;
            crypto_hash_sha256_final(&hash, file.sealedHash.data());
        }
#endif
        sealed_ = true;
        return core::ok();
    }

    Result<bool> exists(std::string_view relativePath) const {
        auto found = findReadableFile(relativePath);
        if (!found.hasValue()) return found.error();
        if (found.value() == nullptr) return false;
        const auto* file = found.value();
        if (file->sealedSize >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return stagingError(
                "avatar pack staging file exceeds the read limit",
                "avatar.pack.staging.read-limit",
                ErrorCode::InvalidArgument);
        }
        auto verified =
            read(relativePath, static_cast<std::size_t>(file->sealedSize));
        if (!verified.hasValue()) return verified.error();
        return true;
    }

    Result<std::vector<std::uint8_t>> read(std::string_view relativePath,
                                           std::size_t maximumBytes) const {
        auto found = findReadableFile(relativePath);
        if (!found.hasValue()) return found.error();
        const auto* file = found.value();
        if (file == nullptr) {
            return stagingError("avatar pack staging file is unavailable",
                                "avatar.pack.staging.not-found",
                                ErrorCode::NotFound);
        }
#ifdef _WIN32
        HANDLE readHandle = CreateFileW(
            file->finalPath.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        const auto information = handleInformation(readHandle);
        const auto final = finalHandlePath(readHandle);
        LARGE_INTEGER size{};
        if (readHandle == INVALID_HANDLE_VALUE ||
            !regularNewFile(readHandle) || !information.has_value() ||
            !file->identity.has_value() ||
            identityOf(*information) != *file->identity ||
            !final.has_value() ||
            !equalPath(file->finalPath.native(), *final) ||
            !childPath(directories_.front().finalPath.native(), *final) ||
            GetFileSizeEx(readHandle, &size) == FALSE ||
            size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) !=
                file->sealedSize ||
            static_cast<std::uint64_t>(size.QuadPart) >
                static_cast<std::uint64_t>(maximumBytes) ||
            static_cast<std::uint64_t>(size.QuadPart) >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            (void)closeHandle(readHandle);
            return stagingError(
                file->sealedSize >
                        static_cast<std::uint64_t>(maximumBytes)
                    ? "avatar pack staging file exceeds the read limit"
                    : "avatar pack staging file identity changed",
                file->sealedSize >
                        static_cast<std::uint64_t>(maximumBytes)
                    ? "avatar.pack.staging.read-limit"
                    : "avatar.pack.staging.identity",
                file->sealedSize >
                        static_cast<std::uint64_t>(maximumBytes)
                    ? ErrorCode::InvalidArgument
                    : ErrorCode::IoFailure);
        }
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0U;
        while (offset < bytes.size()) {
            const auto amount = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD received = 0U;
            if (ReadFile(readHandle, bytes.data() + offset, amount,
                         &received, nullptr) == FALSE ||
                received == 0U) {
                (void)closeHandle(readHandle);
                return stagingError("avatar pack staging file read failed",
                                    "avatar.pack.staging.read");
            }
            offset += static_cast<std::size_t>(received);
        }
        const auto after = handleInformation(readHandle);
        LARGE_INTEGER afterSize{};
        std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
        crypto_hash_sha256(hash.data(), bytes.data(),
                           static_cast<unsigned long long>(bytes.size()));
        if (!after.has_value() ||
            identityOf(*after) != *file->identity ||
            GetFileSizeEx(readHandle, &afterSize) == FALSE ||
            afterSize.QuadPart != size.QuadPart ||
            sodium_memcmp(hash.data(), file->sealedHash.data(),
                          hash.size()) != 0 ||
            !closeHandle(readHandle)) {
            return stagingError("avatar pack staging file identity changed",
                                "avatar.pack.staging.identity");
        }
#else
        int parent = openDirectoryRelative(
            directories_[file->parentIndex].relativePath);
        if (parent < 0) {
            return stagingError("avatar pack staging file read failed",
                                "avatar.pack.staging.read");
        }
        int readDescriptor = ::openat(
            parent, file->name.c_str(),
            O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat information{};
        if (readDescriptor < 0 ||
            ::fstat(readDescriptor, &information) != 0 ||
            !S_ISREG(information.st_mode) || information.st_nlink != 1 ||
            !file->identity.has_value() ||
            !sameObject(information, *file->identity) ||
            !sameObjectAt(parent, file->name, information) ||
            information.st_size < 0 ||
            static_cast<std::uint64_t>(information.st_size) !=
                file->sealedSize ||
            static_cast<std::uint64_t>(information.st_size) >
                static_cast<std::uint64_t>(maximumBytes) ||
            static_cast<std::uint64_t>(information.st_size) >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
            (void)closeDescriptor(readDescriptor);
            (void)closeDescriptor(parent);
            return stagingError(
                file->sealedSize >
                        static_cast<std::uint64_t>(maximumBytes)
                    ? "avatar pack staging file exceeds the read limit"
                    : "avatar pack staging file identity changed",
                file->sealedSize >
                        static_cast<std::uint64_t>(maximumBytes)
                    ? "avatar.pack.staging.read-limit"
                    : "avatar.pack.staging.identity",
                file->sealedSize >
                        static_cast<std::uint64_t>(maximumBytes)
                    ? ErrorCode::InvalidArgument
                    : ErrorCode::IoFailure);
        }
        (void)closeDescriptor(parent);
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(information.st_size));
        std::size_t offset = 0U;
        while (offset < bytes.size()) {
            const auto amount = std::min<std::size_t>(
                bytes.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto received =
                ::pread(readDescriptor, bytes.data() + offset, amount,
                        static_cast<off_t>(offset));
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) {
                (void)closeDescriptor(readDescriptor);
                return stagingError("avatar pack staging file read failed",
                                    "avatar.pack.staging.read");
            }
            offset += static_cast<std::size_t>(received);
        }
        struct stat after {};
        std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
        crypto_hash_sha256(hash.data(), bytes.data(),
                           static_cast<unsigned long long>(bytes.size()));
        if (::fstat(readDescriptor, &after) != 0 ||
            !sameObject(after, *file->identity) ||
            after.st_size != information.st_size ||
            sodium_memcmp(hash.data(), file->sealedHash.data(),
                          hash.size()) != 0 ||
            !closeDescriptor(readDescriptor)) {
            return stagingError("avatar pack staging file identity changed",
                                "avatar.pack.staging.identity");
        }
#endif
        return bytes;
    }

    Result<void> cleanup() {
        if (!active_) return core::ok();
        if (!rootIdentityMatches()) {
            closeAuthority();
            active_ = false;
            return cleanupError();
        }
        bool succeeded = true;
#ifdef _WIN32
        for (auto iterator = files_.rbegin(); iterator != files_.rend();
             ++iterator) {
            ScopedHandle current{CreateFileW(
                iterator->finalPath.c_str(),
                FILE_READ_ATTRIBUTES | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr)};
            const auto information = handleInformation(current.get());
            const auto final = finalHandlePath(current.get());
            FILE_DISPOSITION_INFO disposition{TRUE};
            const bool matches =
                regularNewFile(current.get()) && information.has_value() &&
                iterator->identity.has_value() &&
                identityOf(*information) == *iterator->identity &&
                final.has_value() &&
                equalPath(iterator->finalPath.native(), *final) &&
                childPath(directories_.front().finalPath.native(), *final);
            const bool disposed =
                matches &&
                SetFileInformationByHandle(
                    current.get(), FileDispositionInfo, &disposition,
                    sizeof(disposition)) != FALSE;
            const bool closed = current.close();
            if (!matches || !disposed || !closed) {
                succeeded = false;
                break;
            }
        }
        if (succeeded) {
            for (auto iterator = directories_.rbegin();
                 iterator != directories_.rend(); ++iterator) {
                ScopedHandle current;
                if (iterator->handle != INVALID_HANDLE_VALUE) {
                    current = ScopedHandle{std::exchange(
                        iterator->handle, INVALID_HANDLE_VALUE)};
                } else {
                    current = ScopedHandle{CreateFileW(
                        iterator->finalPath.c_str(),
                        FILE_READ_ATTRIBUTES | DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT |
                            FILE_FLAG_BACKUP_SEMANTICS,
                        nullptr)};
                }
                const auto information =
                    handleInformation(current.get());
                const auto final = finalHandlePath(current.get());
                FILE_DISPOSITION_INFO disposition{TRUE};
                const bool matches =
                    regularDirectory(current.get()) &&
                    information.has_value() &&
                    iterator->identity.has_value() &&
                    identityOf(*information) == *iterator->identity &&
                    final.has_value() &&
                    equalPath(iterator->finalPath.native(), *final);
                const bool marked =
                    matches &&
                    SetFileInformationByHandle(
                        current.get(), FileDispositionInfo, &disposition,
                        sizeof(disposition)) != FALSE;
                const bool closed = current.close();
                if (!marked || !closed) {
                    succeeded = false;
                    break;
                }
            }
        }
        if (!closeAuthority()) succeeded = false;
#else
        struct stat parentInformation {};
        if (!parentIdentity_.has_value() ||
            ::fstat(parentDescriptor_, &parentInformation) != 0 ||
            !sameObject(parentInformation, *parentIdentity_) ||
            !trustedPrivateDirectory(parentInformation)) {
            closeAuthority();
            active_ = false;
            return cleanupError();
        }
        for (auto iterator = files_.rbegin(); iterator != files_.rend();
             ++iterator) {
            int parent = openDirectoryRelative(
                directories_[iterator->parentIndex].relativePath);
            struct stat information {};
            const bool matches =
                parent >= 0 && iterator->identity.has_value() &&
                ::fstatat(parent, iterator->name.c_str(), &information,
                          AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(information.st_mode) && information.st_nlink == 1 &&
                sameObject(information, *iterator->identity);
            if (!matches ||
                ::unlinkat(parent, iterator->name.c_str(), 0) != 0) {
                succeeded = false;
            }
            (void)closeDescriptor(parent);
            if (!succeeded) break;
        }
        if (succeeded) {
            for (auto index = directories_.size(); index > 0U; --index) {
                auto& directory = directories_[index - 1U];
                int parent =
                    directory.parentIndex == kNoParent
                        ? ::dup(parentDescriptor_)
                        : openDirectoryRelative(
                              directories_[directory.parentIndex]
                                  .relativePath);
                struct stat information {};
                const bool matches =
                    parent >= 0 && directory.identity.has_value() &&
                    ::fstatat(parent, directory.name.c_str(), &information,
                              AT_SYMLINK_NOFOLLOW) == 0 &&
                    S_ISDIR(information.st_mode) &&
                    sameObject(information, *directory.identity);
                if (!matches ||
                    ::unlinkat(parent, directory.name.c_str(),
                               AT_REMOVEDIR) != 0) {
                    succeeded = false;
                }
                (void)closeDescriptor(parent);
                if (!closeDescriptor(directory.descriptor))
                    succeeded = false;
                if (!succeeded) break;
            }
        }
        if (!closeAuthority()) succeeded = false;
#endif
        active_ = false;
        return succeeded ? core::ok() : Result<void>{cleanupError()};
    }

    Result<PromotionOutcome> promoteTo(
        const std::filesystem::path& finalPath) {
        if (!active_ || !sealed_ || directories_.empty() ||
            finalPath.empty() || finalPath.filename().empty() ||
            finalPath.filename() == "." || finalPath.filename() == "..") {
            return stagingError("avatar pack staging cannot be promoted",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
        if (!rootIdentityMatches()) {
            return promotionError();
        }
#ifdef _WIN32
        HANDLE destinationParent = CreateFileW(
            finalPath.parent_path().c_str(),
            GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr);
        const auto destinationParentFinal =
            finalHandlePath(destinationParent);
        if (!trustedPrivateDirectory(destinationParent) ||
            !destinationParentFinal.has_value()) {
            (void)closeHandle(destinationParent);
            return promotionErrorAt("destination parent");
        }
        const auto destination =
            std::filesystem::path{*destinationParentFinal} /
            finalPath.filename();
        if (GetFileAttributesW(destination.c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            (void)closeHandle(destinationParent);
            return promotionError(ErrorCode::AlreadyExists);
        }
        const auto name = renamePath(destination);
        if (name.size() >
            (std::numeric_limits<DWORD>::max() / sizeof(wchar_t))) {
            (void)closeHandle(destinationParent);
            return promotionErrorAt("rename");
        }
        std::vector<std::uint8_t> renameBytes(
            offsetof(FILE_RENAME_INFO, FileName) +
            (name.size() + 1U) * sizeof(wchar_t));
        auto* rename =
            reinterpret_cast<FILE_RENAME_INFO*>(renameBytes.data());
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength =
            static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
        auto verifiedFiles = verifyWindowsTreeForPromotion();
        if (!verifiedFiles.hasValue()) {
            (void)closeHandle(destinationParent);
            return verifiedFiles.error();
        }
        bool filesClosed = true;
        for (auto& file : verifiedFiles.value()) {
            const bool closed = file.close();
            filesClosed = closed && filesClosed;
        }
        if (!filesClosed) {
            (void)closeHandle(destinationParent);
            return promotionErrorAt("integrity handles");
        }
        auto& root = directories_.front();
        if (SetFileInformationByHandle(
                root.handle, FileRenameInfo, rename,
                static_cast<DWORD>(renameBytes.size())) == FALSE) {
            const auto code = GetLastError();
            (void)closeHandle(destinationParent);
            return promotionError(
                code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS
                    ? ErrorCode::AlreadyExists
                    : ErrorCode::IoFailure);
        }
        active_ = false;
        const auto outcome = detail::confirmPromotionDurability(
            [this] { return FlushFileBuffers(parentHandle_) != FALSE; },
            [destinationParent] {
                return FlushFileBuffers(destinationParent) != FALSE;
            });
        (void)closeHandle(destinationParent);
#else
        struct stat parentInformation {};
        if (!parentIdentity_.has_value() ||
            ::fstat(parentDescriptor_, &parentInformation) != 0 ||
            !sameObject(parentInformation, *parentIdentity_) ||
            !trustedPrivateDirectory(parentInformation)) {
            return promotionError();
        }
        const auto destinationName = finalPath.filename().string();
        const int destinationParent = ::open(
            finalPath.parent_path().c_str(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat destinationInformation {};
        if (destinationParent < 0 ||
            !directoryDescriptor(destinationParent,
                                 destinationInformation) ||
            !trustedPrivateDirectory(destinationInformation)) {
            int closing = destinationParent;
            (void)closeDescriptor(closing);
            return promotionError();
        }
        struct stat existing {};
        if (::fstatat(destinationParent, destinationName.c_str(), &existing,
                      AT_SYMLINK_NOFOLLOW) == 0) {
            int closing = destinationParent;
            (void)closeDescriptor(closing);
            return promotionError(ErrorCode::AlreadyExists);
        }
        auto verified = verifyPosixTreeForPromotion();
        if (!verified.hasValue()) {
            int closing = destinationParent;
            (void)closeDescriptor(closing);
            return verified.error();
        }
        if (::fsync(directories_.front().descriptor) != 0) {
            int closing = destinationParent;
            (void)closeDescriptor(closing);
            return promotionErrorAt("staging root durability");
        }
#ifdef __linux__
        constexpr unsigned int kRenameNoReplace = 1U;
        const auto renamed = ::syscall(
            SYS_renameat2, parentDescriptor_,
            directories_.front().name.c_str(), destinationParent,
            destinationName.c_str(), kRenameNoReplace);
#elif defined(__APPLE__)
        const auto renamed = ::renameatx_np(
            parentDescriptor_, directories_.front().name.c_str(),
            destinationParent, destinationName.c_str(), RENAME_EXCL);
#else
        errno = ENOTSUP;
        const auto renamed = -1;
#endif
        if (renamed != 0) {
            const auto code = errno;
            int closing = destinationParent;
            (void)closeDescriptor(closing);
            return promotionError(
                code == EEXIST ? ErrorCode::AlreadyExists
                               : ErrorCode::IoFailure);
        }
        active_ = false;
        const auto outcome = detail::confirmPromotionDurability(
            [this] { return ::fsync(parentDescriptor_) == 0; },
            [destinationParent] {
                return ::fsync(destinationParent) == 0;
            });
        int closing = destinationParent;
        (void)closeDescriptor(closing);
#endif
        closeAuthority();
        return outcome;
    }

private:
#ifdef _WIN32
    struct DirectoryRecord final {
        std::filesystem::path finalPath;
        HANDLE handle{INVALID_HANDLE_VALUE};
        std::optional<ObjectIdentity> identity;
    };

    struct FileRecord final {
        std::string relativePath;
        std::filesystem::path finalPath;
        std::optional<ObjectIdentity> identity;
        std::uint64_t sealedSize{};
        std::array<unsigned char, crypto_hash_sha256_BYTES> sealedHash{};
    };
#else
    static constexpr std::size_t kNoParent =
        std::numeric_limits<std::size_t>::max();

    struct DirectoryRecord final {
        std::size_t parentIndex{kNoParent};
        std::string name;
        std::string relativePath;
        int descriptor{-1};
        std::optional<struct stat> identity;
    };

    struct FileRecord final {
        std::size_t parentIndex{};
        std::string name;
        std::string relativePath;
        std::optional<struct stat> identity;
        std::uint64_t sealedSize{};
        std::array<unsigned char, crypto_hash_sha256_BYTES> sealedHash{};
    };
#endif

    Result<const FileRecord*>
    findReadableFile(std::string_view relativePath) const {
        if (!active_ || !sealed_) {
            return stagingError("avatar pack staging is not readable",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
        if (!pathComponents(relativePath).has_value()) {
            return stagingError("avatar pack staging path is invalid",
                                "avatar.pack.staging.path",
                                ErrorCode::InvalidArgument);
        }
        const auto found = std::find_if(
            files_.begin(), files_.end(), [relativePath](const auto& file) {
                return file.relativePath == relativePath;
            });
        return found == files_.end() ? nullptr : &*found;
    }

#ifdef _WIN32
    bool pathMatches(const DirectoryRecord& directory) const {
        HANDLE current =
            openDirectoryForIdentity(directory.finalPath);
        const auto information = handleInformation(current);
        const auto final = finalHandlePath(current);
        const bool matches =
            regularDirectory(current) && information.has_value() &&
            directory.identity.has_value() &&
            identityOf(*information) == *directory.identity &&
            final.has_value() &&
            equalPath(directory.finalPath.native(), *final) &&
            (equalPath(directories_.front().finalPath.native(), *final) ||
             childPath(directories_.front().finalPath.native(), *final));
        (void)closeHandle(current);
        return matches;
    }

    bool pathMatches(const FileRecord& file) const {
        HANDLE current = CreateFileW(
            file.finalPath.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const auto information = handleInformation(current);
        const auto final = finalHandlePath(current);
        const bool matches =
            regularNewFile(current) && information.has_value() &&
            file.identity.has_value() &&
            identityOf(*information) == *file.identity && final.has_value() &&
            equalPath(file.finalPath.native(), *final) &&
            childPath(directories_.front().finalPath.native(), *final);
        (void)closeHandle(current);
        return matches;
    }
#endif

    bool rootIdentityMatches() const {
        if (directories_.empty()) return true;
#ifdef _WIN32
        return pathMatches(directories_.front());
#else
        const auto& root = directories_.front();
        return root.identity.has_value() &&
               sameObjectAt(parentDescriptor_, root.name, *root.identity);
#endif
    }

#ifdef _WIN32
    bool verifyPromotionFile(HANDLE handle,
                             const FileRecord& file) const {
        const auto information = handleInformation(handle);
        const auto final = finalHandlePath(handle);
        LARGE_INTEGER size{};
        if (!regularNewFile(handle) || !information.has_value() ||
            !file.identity.has_value() ||
            identityOf(*information) != *file.identity ||
            !final.has_value() ||
            !equalPath(file.finalPath.native(), *final) ||
            !childPath(directories_.front().finalPath.native(), *final) ||
            GetFileSizeEx(handle, &size) == FALSE || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != file.sealedSize) {
            return false;
        }
        LARGE_INTEGER beginning{};
        if (SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) ==
            FALSE) {
            return false;
        }
        crypto_hash_sha256_state hash{};
        if (crypto_hash_sha256_init(&hash) != 0) return false;
        std::array<unsigned char, 64U * 1024U> buffer{};
        std::uint64_t total = 0U;
        for (;;) {
            DWORD received = 0U;
            if (ReadFile(handle, buffer.data(),
                         static_cast<DWORD>(buffer.size()), &received,
                         nullptr) == FALSE) {
                return false;
            }
            if (received == 0U) break;
            if (crypto_hash_sha256_update(&hash, buffer.data(), received) !=
                0) {
                return false;
            }
            total += received;
        }
        std::array<unsigned char, crypto_hash_sha256_BYTES> actualHash{};
        if (crypto_hash_sha256_final(&hash, actualHash.data()) != 0)
            return false;
        const auto after = handleInformation(handle);
        LARGE_INTEGER afterSize{};
        return after.has_value() &&
               identityOf(*after) == *file.identity &&
               GetFileSizeEx(handle, &afterSize) != FALSE &&
               afterSize.QuadPart == size.QuadPart &&
               total == file.sealedSize &&
               sodium_memcmp(actualHash.data(), file.sealedHash.data(),
                             actualHash.size()) == 0 &&
               FlushFileBuffers(handle) != FALSE;
    }

    Result<std::vector<ScopedHandle>>
    verifyWindowsTreeForPromotion() const {
        struct ExpectedNode final {
            bool directory{};
            std::size_t index{};
            bool seen{};
        };

        std::map<std::wstring, ExpectedNode, WindowsPathLess> expected;
        for (std::size_t index = 1U; index < directories_.size(); ++index) {
            const auto [iterator, inserted] = expected.emplace(
                directories_[index].finalPath.native(),
                ExpectedNode{.directory = true, .index = index});
            (void)iterator;
            if (!inserted) return promotionErrorAt("topology");
        }
        for (std::size_t index = 0U; index < files_.size(); ++index) {
            const auto [iterator, inserted] = expected.emplace(
                files_[index].finalPath.native(),
                ExpectedNode{.directory = false, .index = index});
            (void)iterator;
            if (!inserted) return promotionErrorAt("topology");
        }

        std::vector<ScopedHandle> verifiedFiles;
        verifiedFiles.reserve(files_.size());
        auto enumerate = [&](bool verifyContents) {
            for (auto& [path, node] : expected) {
                (void)path;
                node.seen = false;
            }
            std::vector<std::filesystem::path> pending{
                directories_.front().finalPath};
            while (!pending.empty()) {
                auto directory = std::move(pending.back());
                pending.pop_back();
                WIN32_FIND_DATAW data{};
                const auto pattern = directory / L"*";
                ScopedFindHandle found{
                    FindFirstFileW(pattern.c_str(), &data)};
                if (found.get() == INVALID_HANDLE_VALUE) {
                    if (GetLastError() == ERROR_FILE_NOT_FOUND) continue;
                    return false;
                }
                for (;;) {
                    const std::wstring_view name{data.cFileName};
                    if (name != L"." && name != L"..") {
                        const auto actual = directory / data.cFileName;
                        const bool isDirectory =
                            (data.dwFileAttributes &
                             FILE_ATTRIBUTE_DIRECTORY) != 0U;
                        const bool isReparse =
                            (data.dwFileAttributes &
                             FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
                        const auto expectedNode =
                            expected.find(actual.native());
                        if (isReparse ||
                            expectedNode == expected.end() ||
                            expectedNode->second.seen ||
                            expectedNode->second.directory != isDirectory) {
                            return false;
                        }
                        auto& node = expectedNode->second;
                        node.seen = true;
                        if (isDirectory) {
                            ScopedHandle current{CreateFileW(
                                actual.c_str(),
                                verifyContents
                                    ? GENERIC_READ | GENERIC_WRITE |
                                          FILE_READ_ATTRIBUTES
                                    : FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT |
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr)};
                            const auto information =
                                handleInformation(current.get());
                            const auto final =
                                finalHandlePath(current.get());
                            const auto& record =
                                directories_[node.index];
                            if (!regularDirectory(current.get()) ||
                                !information.has_value() ||
                                !record.identity.has_value() ||
                                identityOf(*information) !=
                                    *record.identity ||
                                !final.has_value() ||
                                !equalPath(record.finalPath.native(),
                                           *final) ||
                                (verifyContents &&
                                 FlushFileBuffers(current.get()) == FALSE)) {
                                return false;
                            }
                            pending.push_back(actual);
                        } else if (verifyContents) {
                            ScopedHandle current{CreateFileW(
                                actual.c_str(),
                                GENERIC_READ | GENERIC_WRITE |
                                    FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT |
                                    FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr)};
                            if (!current.valid() ||
                                !verifyPromotionFile(
                                    current.get(),
                                    files_[node.index])) {
                                return false;
                            }
                            verifiedFiles.push_back(std::move(current));
                        }
                    }
                    if (FindNextFileW(found.get(), &data) == FALSE) {
                        if (GetLastError() != ERROR_NO_MORE_FILES)
                            return false;
                        break;
                    }
                }
            }
            return std::all_of(
                expected.begin(), expected.end(),
                [](const auto& item) { return item.second.seen; });
        };

        if (!rootIdentityMatches() ||
            FlushFileBuffers(directories_.front().handle) == FALSE ||
            !enumerate(true) || verifiedFiles.size() != files_.size() ||
            !enumerate(false) || !rootIdentityMatches()) {
            return promotionErrorAt("integrity");
        }
        return verifiedFiles;
    }
#else
    Result<void> verifyPosixTreeForPromotion() const {
        struct ExpectedNode final {
            bool directory{};
            std::size_t index{};
            bool seen{};
        };

        std::map<std::string, ExpectedNode> expected;
        for (std::size_t index = 1U; index < directories_.size(); ++index) {
            const auto [iterator, inserted] = expected.emplace(
                directories_[index].relativePath,
                ExpectedNode{.directory = true, .index = index});
            (void)iterator;
            if (!inserted) return promotionErrorAt("topology");
        }
        for (std::size_t index = 0U; index < files_.size(); ++index) {
            const auto [iterator, inserted] = expected.emplace(
                files_[index].relativePath,
                ExpectedNode{.directory = false, .index = index});
            (void)iterator;
            if (!inserted) return promotionErrorAt("topology");
        }

        auto enumerate = [&](bool verifyContents) {
            for (auto& [path, node] : expected) {
                (void)path;
                node.seen = false;
            }
            std::vector<std::string> pending{std::string{}};
            while (!pending.empty()) {
                auto currentPath = std::move(pending.back());
                pending.pop_back();
                ScopedDescriptor current{
                    openDirectoryRelative(currentPath)};
                if (current.get() < 0) return false;
                ScopedDirectoryStream stream{
                    ::fdopendir(current.get())};
                if (stream.get() == nullptr) return false;
                (void)current.release();
                for (;;) {
                    errno = 0;
                    auto* entry = ::readdir(stream.get());
                    if (entry == nullptr) {
                        if (errno != 0) return false;
                        break;
                    }
                    const std::string_view name{entry->d_name};
                    if (name == "." || name == "..") continue;
                    const auto relative =
                        currentPath.empty()
                            ? std::string{name}
                            : currentPath + "/" + std::string{name};
                    const auto expectedNode = expected.find(relative);
                    struct stat information {};
                    if (expectedNode == expected.end() ||
                        expectedNode->second.seen ||
                        ::fstatat(::dirfd(stream.get()), entry->d_name,
                                  &information,
                                  AT_SYMLINK_NOFOLLOW) != 0) {
                        return false;
                    }
                    const bool isDirectory =
                        S_ISDIR(information.st_mode);
                    const bool isFile = S_ISREG(information.st_mode);
                    auto& node = expectedNode->second;
                    if ((!isDirectory && !isFile) ||
                        node.directory != isDirectory) {
                        return false;
                    }
                    node.seen = true;
                    if (isDirectory) {
                        const auto& record = directories_[node.index];
                        ScopedDescriptor child{::openat(
                            ::dirfd(stream.get()), entry->d_name,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                O_CLOEXEC)};
                        struct stat opened {};
                        if (child.get() < 0 ||
                            !record.identity.has_value() ||
                            !directoryDescriptor(child.get(), opened) ||
                            !sameObject(opened, information) ||
                            !sameObject(opened, *record.identity) ||
                            (verifyContents &&
                             ::fsync(child.get()) != 0)) {
                            return false;
                        }
                        pending.push_back(relative);
                        continue;
                    }
                    const auto& record = files_[node.index];
                    ScopedDescriptor file{::openat(
                        ::dirfd(stream.get()), entry->d_name,
                        O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
                    struct stat opened {};
                    if (file.get() < 0 ||
                        !record.identity.has_value() ||
                        ::fstat(file.get(), &opened) != 0 ||
                        !S_ISREG(opened.st_mode) ||
                        opened.st_nlink != 1 ||
                        opened.st_size < 0 ||
                        !sameObject(opened, information) ||
                        !sameObject(opened, *record.identity) ||
                        static_cast<std::uint64_t>(opened.st_size) !=
                            record.sealedSize) {
                        return false;
                    }
                    if (!verifyContents) continue;
                    crypto_hash_sha256_state hash {};
                    if (crypto_hash_sha256_init(&hash) != 0)
                        return false;
                    std::array<unsigned char, 64U * 1024U> buffer {};
                    std::uint64_t total = 0U;
                    for (;;) {
                        const auto received = ::read(
                            file.get(), buffer.data(), buffer.size());
                        if (received < 0 && errno == EINTR) continue;
                        if (received < 0) return false;
                        if (received == 0) break;
                        if (crypto_hash_sha256_update(
                                &hash, buffer.data(),
                                static_cast<unsigned long long>(
                                    received)) != 0) {
                            return false;
                        }
                        total += static_cast<std::uint64_t>(received);
                    }
                    std::array<unsigned char,
                               crypto_hash_sha256_BYTES>
                        actualHash {};
                    struct stat after {};
                    if (crypto_hash_sha256_final(
                            &hash, actualHash.data()) != 0 ||
                        ::fstat(file.get(), &after) != 0 ||
                        !sameObject(after, *record.identity) ||
                        after.st_size != opened.st_size ||
                        total != record.sealedSize ||
                        sodium_memcmp(actualHash.data(),
                                      record.sealedHash.data(),
                                      actualHash.size()) != 0 ||
                        ::fsync(file.get()) != 0) {
                        return false;
                    }
                }
                if (!stream.close()) return false;
            }
            return std::all_of(
                expected.begin(), expected.end(),
                [](const auto& item) { return item.second.seen; });
        };

        if (!enumerate(true) || !enumerate(false) ||
            !rootIdentityMatches()) {
            return promotionErrorAt("integrity");
        }
        return core::ok();
    }
#endif

    bool closeAuthority() noexcept {
        bool succeeded = true;
#ifdef _WIN32
        for (auto& directory : directories_)
            if (!closeHandle(directory.handle)) succeeded = false;
        if (!closeHandle(parentHandle_)) succeeded = false;
#else
        for (auto& directory : directories_)
            if (!closeDescriptor(directory.descriptor)) succeeded = false;
        if (!closeDescriptor(parentDescriptor_)) succeeded = false;
#endif
        return succeeded;
    }

#ifndef _WIN32
    int openDirectoryRelative(std::string_view relativePath) const noexcept {
        if (directories_.empty() ||
            directories_.front().descriptor < 0) {
            return -1;
        }
        int current = ::dup(directories_.front().descriptor);
        struct stat rootInformation {};
        if (current < 0 ||
            !directories_.front().identity.has_value() ||
            !directoryDescriptor(current, rootInformation) ||
            !sameObject(rootInformation,
                        *directories_.front().identity)) {
            (void)closeDescriptor(current);
            return -1;
        }
        if (relativePath.empty()) return current;
        const auto components = pathComponents(relativePath);
        if (!components.has_value()) {
            (void)closeDescriptor(current);
            return -1;
        }
        std::string key;
        for (const auto& component : *components) {
            if (!key.empty()) key.push_back('/');
            key += component;
            const auto expected = directoryIndexes_.find(key);
            if (expected == directoryIndexes_.end() ||
                !directories_[expected->second].identity.has_value()) {
                (void)closeDescriptor(current);
                return -1;
            }
            int child = ::openat(
                current, component.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat information {};
            const bool matches =
                child >= 0 &&
                directoryDescriptor(child, information) &&
                sameObject(
                    information,
                    *directories_[expected->second].identity);
            (void)closeDescriptor(current);
            current = child;
            if (!matches) {
                (void)closeDescriptor(current);
                return -1;
            }
        }
        return current;
    }
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
                 .handle = INVALID_HANDLE_VALUE,
                 .identity = std::nullopt});
            if (CreateDirectoryW(expected.c_str(), nullptr) == FALSE) {
                directories_.pop_back();
                return stagingError(
                    "avatar pack staging directory creation failed",
                    "avatar.pack.staging.directory");
            }
            auto& created = directories_.back();
            created.handle = openDirectoryNoDelete(expected);
            const auto final = finalHandlePath(created.handle);
            const auto information = handleInformation(created.handle);
            if (created.handle == INVALID_HANDLE_VALUE ||
                !regularDirectory(created.handle) ||
                !final.has_value() ||
                !information.has_value() ||
                !equalPath(expected.native(), *final) ||
                !childPath(directories_.front().finalPath.native(),
                           *final)) {
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.directory");
            }
            created.finalPath = std::filesystem::path{*final};
            created.identity = identityOf(*information);
#else
            int parentDescriptor = openDirectoryRelative(
                directories_[current].relativePath);
            if (parentDescriptor < 0) {
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.directory");
            }
            directories_.push_back(
                {.parentIndex = current,
                 .name = components[index],
                 .relativePath = key,
                 .descriptor = -1,
                 .identity = std::nullopt});
            if (::mkdirat(parentDescriptor, components[index].c_str(),
                          0700) != 0) {
                directories_.pop_back();
                (void)closeDescriptor(parentDescriptor);
                return stagingError(
                    "avatar pack staging directory creation failed",
                    "avatar.pack.staging.directory");
            }
            auto& created = directories_.back();
            int createdDescriptor = ::openat(
                parentDescriptor, created.name.c_str(),
                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat information {};
            if (createdDescriptor < 0 ||
                !directoryDescriptor(createdDescriptor, information) ||
                !sameObjectAt(parentDescriptor, created.name,
                              information)) {
                (void)closeDescriptor(createdDescriptor);
                (void)closeDescriptor(parentDescriptor);
                return stagingError(
                    "avatar pack staging directory verification failed",
                    "avatar.pack.staging.directory");
            }
            created.identity = information;
            const bool childClosed =
                closeDescriptor(createdDescriptor);
            const bool parentClosed =
                closeDescriptor(parentDescriptor);
            if (!childClosed || !parentClosed) {
                return stagingError(
                    "avatar pack staging directory close failed",
                    "avatar.pack.staging.identity");
            }
#endif
            current = directories_.size() - 1U;
            directoryIndexes_.emplace(key, current);
        }
        return current;
    }

    bool active_{true};
    bool sealed_{false};
    std::unordered_map<std::string, std::size_t> directoryIndexes_;
    std::vector<DirectoryRecord> directories_;
#ifdef _WIN32
    HANDLE parentHandle_{INVALID_HANDLE_VALUE};
    std::wstring parentFinal_;
    std::vector<FileRecord> files_;
#else
    int parentDescriptor_{-1};
    std::optional<struct stat> parentIdentity_;
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
    } catch (...) {
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
    } catch (...) {
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
    } catch (...) {
        return stagingError("avatar pack extraction failed safely",
                            "avatar.pack.staging.exception");
    }
}

Result<bool>
AvatarPackStaging::exists(std::string_view relativePath) const noexcept {
    try {
        if (!implementation_) {
            return stagingError("avatar pack staging is no longer active",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
        return implementation_->exists(relativePath);
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (...) {
        return stagingError("avatar pack staging lookup failed safely",
                            "avatar.pack.staging.exception");
    }
}

Result<std::vector<std::uint8_t>>
AvatarPackStaging::read(std::string_view relativePath,
                        std::size_t maximumBytes) const noexcept {
    try {
        if (!implementation_) {
            return stagingError("avatar pack staging is no longer active",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
        return implementation_->read(relativePath, maximumBytes);
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (...) {
        return stagingError("avatar pack staging read failed safely",
                            "avatar.pack.staging.exception");
    }
}

Result<void> AvatarPackStaging::cleanup() noexcept {
    try {
        if (!implementation_) return core::ok();
        return implementation_->cleanup();
    } catch (...) {
        return cleanupError();
    }
}

Result<PromotionOutcome> AvatarPackStaging::promoteTo(
    const std::filesystem::path& finalPath) && noexcept {
    try {
        if (!implementation_) {
            return stagingError("avatar pack staging is no longer active",
                                "avatar.pack.staging.state",
                                ErrorCode::InvalidState);
        }
        return implementation_->promoteTo(finalPath);
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (...) {
        return promotionError();
    }
}

Result<void> AvatarPackStaging::seal() {
    try {
        return implementation_->seal();
    } catch (const std::bad_alloc&) {
        return allocationError();
    } catch (...) {
        return stagingError("avatar pack staging seal failed safely",
                            "avatar.pack.staging.exception");
    }
}

}  // namespace creator::avatar_pack_adapter
