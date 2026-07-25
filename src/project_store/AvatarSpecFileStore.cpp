#include "project_store/AvatarSpecFileStore.h"

#include "avatar/AvatarSpecCodec.h"
#include "core/AppError.h"
#include "core/Uuid.h"
#include "domain/PortablePath.h"
#include "project_store/internal/AvatarSpecStorageAuthority.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <winternl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace creator::project_store {
namespace {

namespace fs = std::filesystem;

using avatar::AvatarId;
using avatar::AvatarSpec;
using avatar::AvatarSpecCodec;
using core::AppError;
using core::ErrorCode;
using core::Result;

constexpr std::size_t kMaximumFileSize = 8U * 1024U * 1024U;
constexpr std::size_t kIoChunkSize = 64U * 1024U;

AppError unsafeError(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message),
            "avatar.spec.store.path", "avatar.validation.path"};
}

AppError ioError(std::string message) {
    return {ErrorCode::IoFailure, std::move(message),
            "avatar.spec.store.io", "avatar.validation.io"};
}

AppError parseError(std::string message) {
    return {ErrorCode::ParseFailure, std::move(message),
            "avatar.spec.store.parse", "avatar.validation.json"};
}

AppError notFoundError() {
    return {ErrorCode::NotFound, "avatar spec file does not exist",
            "avatar.spec.store.not-found", "avatar.validation.not-found"};
}

bool recoveryEligible(const AppError& error) noexcept {
    return error.code() == ErrorCode::ParseFailure ||
           error.code() == ErrorCode::IoFailure ||
           error.code() == ErrorCode::NotFound;
}

#ifdef _WIN32

bool directChildName(std::wstring_view name) noexcept {
    return !name.empty() && name != L"." && name != L".." &&
           name.find(L'/') == name.npos && name.find(L'\\') == name.npos &&
           name.find(L':') == name.npos && name.find(L'\0') == name.npos;
}

struct RelativeHandleResult final {
    HANDLE handle{INVALID_HANDLE_VALUE};
    NTSTATUS status{};
    ULONG information{};
};

RelativeHandleResult openRelative(
    HANDLE parent, std::wstring_view name, ACCESS_MASK access,
    ULONG shareAccess, ULONG disposition, ULONG options,
    ULONG fileAttributes = FILE_ATTRIBUTE_NORMAL) noexcept {
    if (!directChildName(name) ||
        name.size() > static_cast<std::size_t>(
                          std::numeric_limits<USHORT>::max() /
                          sizeof(wchar_t))) {
        return {};
    }
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto address =
        module == nullptr ? nullptr : GetProcAddress(module, "NtCreateFile");
    if (address == nullptr) return {};
    const auto ntCreateFile =
        reinterpret_cast<decltype(&NtCreateFile)>(address);
    UNICODE_STRING objectName{};
    objectName.Buffer = const_cast<PWSTR>(name.data());
    objectName.Length =
        static_cast<USHORT>(name.size() * sizeof(wchar_t));
    objectName.MaximumLength = objectName.Length;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &objectName, OBJ_DONT_REPARSE,
                               parent, nullptr);
    IO_STATUS_BLOCK ioStatus{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto status = ntCreateFile(
        &handle, access, &attributes, &ioStatus, nullptr, fileAttributes,
        shareAccess, disposition, options, nullptr, 0U);
    if (status < 0) {
        if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
        return {.handle = INVALID_HANDLE_VALUE, .status = status};
    }
    return {.handle = handle, .status = status,
            .information = static_cast<ULONG>(ioStatus.Information)};
}

DWORD win32Status(NTSTATUS status) noexcept {
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto address = module == nullptr
                             ? nullptr
                             : GetProcAddress(module, "RtlNtStatusToDosError");
    if (address == nullptr) return ERROR_GEN_FAILURE;
    using Converter = ULONG(WINAPI*)(NTSTATUS);
    return static_cast<DWORD>(
        reinterpret_cast<Converter>(address)(status));
}

bool missingStatus(NTSTATUS status) noexcept {
    const DWORD code = win32Status(status);
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

bool reparseStatus(NTSTATUS status) noexcept {
    const DWORD code = win32Status(status);
    return code == ERROR_CANT_ACCESS_FILE ||
           code == ERROR_REPARSE_TAG_INVALID ||
           code == ERROR_REPARSE_TAG_MISMATCH;
}

bool renameRelative(HANDLE file, HANDLE parent,
                    std::wstring_view target) noexcept {
    struct RenameInformation final {
        BOOLEAN replaceIfExists{};
        HANDLE rootDirectory{};
        ULONG fileNameLength{};
        WCHAR fileName[1]{};
    };
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto address = module == nullptr
                             ? nullptr
                             : GetProcAddress(module, "NtSetInformationFile");
    if (address == nullptr ||
        target.size() >
            static_cast<std::size_t>(std::numeric_limits<ULONG>::max() /
                                     sizeof(wchar_t))) {
        return false;
    }
    using Setter = NTSTATUS(NTAPI*)(
        HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
    constexpr std::size_t kMaximumRenameBytes = 512U;
    const std::size_t bytes =
        offsetof(RenameInformation, fileName) +
        target.size() * sizeof(wchar_t);
    if (bytes > kMaximumRenameBytes) return false;
    alignas(void*) std::array<std::byte, kMaximumRenameBytes> storage{};
    auto* rename =
        reinterpret_cast<RenameInformation*>(storage.data());
    rename->replaceIfExists = TRUE;
    rename->rootDirectory = parent;
    rename->fileNameLength =
        static_cast<ULONG>(target.size() * sizeof(wchar_t));
    std::memcpy(rename->fileName, target.data(), rename->fileNameLength);
    IO_STATUS_BLOCK status{};
    return reinterpret_cast<Setter>(address)(
               file, &status, rename, static_cast<ULONG>(bytes),
               static_cast<FILE_INFORMATION_CLASS>(10)) >= 0;
}

struct WindowsIdentity final {
    DWORD volume{};
    DWORD high{};
    DWORD low{};
};

std::optional<WindowsIdentity> identityOf(HANDLE handle,
                                          bool requireDirectory) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (handle == INVALID_HANDLE_VALUE ||
        GetFileInformationByHandle(handle, &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
        requireDirectory !=
            ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)) {
        return std::nullopt;
    }
    return WindowsIdentity{information.dwVolumeSerialNumber,
                           information.nFileIndexHigh,
                           information.nFileIndexLow};
}

bool sameIdentity(const WindowsIdentity& left,
                  const WindowsIdentity& right) noexcept {
    return left.volume == right.volume && left.high == right.high &&
           left.low == right.low;
}

std::string narrowAscii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character < 0 || character > 0x7f) {
            result.push_back('?');
        } else {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

#else

bool sameIdentity(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

class ScopedDescriptor final {
public:
    explicit ScopedDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {}
    ~ScopedDescriptor() {
        if (descriptor_ >= 0) (void)::close(descriptor_);
    }
    ScopedDescriptor(ScopedDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}
    ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
        if (this == &other) return *this;
        if (descriptor_ >= 0) (void)::close(descriptor_);
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }
    ScopedDescriptor(const ScopedDescriptor&) = delete;
    ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

private:
    int descriptor_;
};

#endif

class RootAuthority;

class ChildAuthority final {
public:
#ifdef _WIN32
    ChildAuthority(const RootAuthority* root, std::string name, HANDLE handle,
                   WindowsIdentity identity)
        : root_(root), name_(std::move(name)), handle_(handle),
          identity_(identity) {}
#else
    ChildAuthority(const RootAuthority* root, std::string name, int descriptor,
                   struct stat identity)
        : root_(root), name_(std::move(name)), descriptor_(descriptor),
          identity_(identity) {}
#endif
    ChildAuthority(ChildAuthority&& other) noexcept;
    ChildAuthority& operator=(ChildAuthority&& other) noexcept;
    ~ChildAuthority();
    ChildAuthority(const ChildAuthority&) = delete;
    ChildAuthority& operator=(const ChildAuthority&) = delete;

    [[nodiscard]] Result<std::string> read(std::string_view fileName) const;
    [[nodiscard]] Result<void> write(std::string_view fileName,
                                     std::string_view contents) const;
    [[nodiscard]] Result<void> revalidate() const;

private:
    const RootAuthority* root_{};
    std::string name_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
    WindowsIdentity identity_{};
#else
    int descriptor_{-1};
    struct stat identity_ {};
#endif
};

class RootAuthority final {
public:
    static Result<RootAuthority> open(fs::path path) noexcept;
    RootAuthority(RootAuthority&& other) noexcept;
    RootAuthority& operator=(RootAuthority&& other) noexcept;
    ~RootAuthority();
    RootAuthority(const RootAuthority&) = delete;
    RootAuthority& operator=(const RootAuthority&) = delete;

    [[nodiscard]] Result<void> revalidate() const;
    [[nodiscard]] Result<ChildAuthority> child(std::string_view name,
                                                bool create) const;
    [[nodiscard]] Result<std::vector<std::string>> listChildren() const;

#ifdef _WIN32
    [[nodiscard]] HANDLE handle() const noexcept { return handle_; }
#else
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
#endif

private:
    friend class ChildAuthority;
#ifdef _WIN32
    RootAuthority(fs::path path, HANDLE handle, WindowsIdentity identity)
        : path_(std::move(path)), handle_(handle), identity_(identity) {}
#else
    RootAuthority(fs::path path, int descriptor, struct stat identity)
        : path_(std::move(path)), descriptor_(descriptor),
          identity_(identity) {}
#endif

    [[nodiscard]] Result<void> rejectAlias(std::string_view name) const;
#ifdef _WIN32
    [[nodiscard]] Result<void> exactChildEntryMatches(
        std::string_view name, const WindowsIdentity& identity) const;
#else
    [[nodiscard]] Result<void> exactChildEntryMatches(
        std::string_view name, const struct stat& identity) const;
#endif

    fs::path path_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
    WindowsIdentity identity_{};
#else
    int descriptor_{-1};
    struct stat identity_ {};
#endif
};

RootAuthority::RootAuthority(RootAuthority&& other) noexcept
    : path_(std::move(other.path_))
#ifdef _WIN32
      , handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
      identity_(other.identity_)
#else
      , descriptor_(std::exchange(other.descriptor_, -1)),
      identity_(other.identity_)
#endif
{}

RootAuthority& RootAuthority::operator=(RootAuthority&& other) noexcept {
    if (this == &other) return *this;
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) (void)CloseHandle(handle_);
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
#else
    if (descriptor_ >= 0) (void)::close(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, -1);
#endif
    path_ = std::move(other.path_);
    identity_ = other.identity_;
    return *this;
}

RootAuthority::~RootAuthority() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) (void)CloseHandle(handle_);
#else
    if (descriptor_ >= 0) (void)::close(descriptor_);
#endif
}

Result<RootAuthority> RootAuthority::open(fs::path path) noexcept {
    try {
        std::error_code error;
        path = fs::absolute(path, error).lexically_normal();
        if (error || path.empty()) return unsafeError("avatar root path is invalid");
#ifdef _WIN32
        HANDLE handle = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        auto identity = identityOf(handle, true);
        if (!identity.has_value()) {
            if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
            return unsafeError("avatar root must be a plain directory");
        }
        return RootAuthority{std::move(path), handle, *identity};
#else
        const int descriptor =
            ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat identity {};
        if (descriptor < 0 || ::fstat(descriptor, &identity) != 0 ||
            !S_ISDIR(identity.st_mode) || identity.st_nlink == 0) {
            if (descriptor >= 0) (void)::close(descriptor);
            return unsafeError("avatar root must be a plain directory");
        }
        return RootAuthority{std::move(path), descriptor, identity};
#endif
    } catch (...) {
        return unsafeError("avatar root path is invalid");
    }
}

Result<void> RootAuthority::revalidate() const {
#ifdef _WIN32
    auto retained = identityOf(handle_, true);
    HANDLE current = CreateFileW(
        path_.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    auto currentIdentity = identityOf(current, true);
    if (current != INVALID_HANDLE_VALUE) (void)CloseHandle(current);
    if (!retained.has_value() || !currentIdentity.has_value() ||
        !sameIdentity(*retained, identity_) ||
        !sameIdentity(*currentIdentity, identity_)) {
        return unsafeError("avatar root identity changed");
    }
#else
    struct stat retained {};
    struct stat named {};
    if (descriptor_ < 0 || ::fstat(descriptor_, &retained) != 0 ||
        retained.st_nlink == 0 || !S_ISDIR(retained.st_mode) ||
        !sameIdentity(retained, identity_) ||
        ::lstat(path_.c_str(), &named) != 0 || !S_ISDIR(named.st_mode) ||
        !sameIdentity(named, identity_)) {
        return unsafeError("avatar root identity changed");
    }
#endif
    return core::ok();
}

Result<std::vector<std::string>> RootAuthority::listChildren() const {
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
    std::vector<std::string> names;
#ifdef _WIN32
    fs::path pattern = path_ / "*";
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return ioError("avatar root could not be enumerated");
        }
    } else {
        do {
            const std::wstring_view wide{data.cFileName};
            if (wide == L"." || wide == L"..") continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
                (void)FindClose(search);
                return unsafeError("avatar root contains a reparse point");
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U)
                continue;
            names.push_back(narrowAscii(wide));
        } while (FindNextFileW(search, &data) != FALSE);
        const DWORD reason = GetLastError();
        (void)FindClose(search);
        if (reason != ERROR_NO_MORE_FILES) {
            return ioError("avatar root enumeration failed");
        }
    }
#else
    const int duplicate = ::openat(
        descriptor_, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR* directory = duplicate < 0 ? nullptr : ::fdopendir(duplicate);
    if (directory == nullptr) {
        if (duplicate >= 0) (void)::close(duplicate);
        return ioError("avatar root could not be enumerated");
    }
    errno = 0;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string name{entry->d_name};
        if (name == "." || name == "..") continue;
        struct stat information {};
        if (::fstatat(descriptor_, name.c_str(), &information,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            (void)::closedir(directory);
            return ioError("avatar child could not be inspected");
        }
        if (S_ISLNK(information.st_mode)) {
            (void)::closedir(directory);
            return unsafeError("avatar root contains a symbolic link");
        }
        if (!S_ISDIR(information.st_mode)) continue;
        names.push_back(name);
    }
    const int reason = errno;
    (void)::closedir(directory);
    if (reason != 0) return ioError("avatar root enumeration failed");
#endif
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
    return names;
}

Result<void> RootAuthority::rejectAlias(std::string_view name) const {
    auto children = listChildren();
    if (!children.hasValue()) return children.error();
    const std::string requestedKey = domain::portablePathAliasKey(name);
    for (const auto& child : children.value()) {
        if (child != name &&
            domain::portablePathAliasKey(child) == requestedKey) {
            return unsafeError("avatar directory has a non-canonical alias");
        }
    }
    return core::ok();
}

#ifdef _WIN32
Result<void> RootAuthority::exactChildEntryMatches(
    std::string_view name, const WindowsIdentity& identity) const {
    const std::wstring wideName = fs::path{name}.wstring();
    const auto current = openRelative(
        handle_, wideName, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
    const auto currentIdentity = identityOf(current.handle, true);
    std::array<wchar_t, 1024> finalPath{};
    const DWORD finalLength =
        current.handle == INVALID_HANDLE_VALUE
            ? 0U
            : GetFinalPathNameByHandleW(
                  current.handle, finalPath.data(),
                  static_cast<DWORD>(finalPath.size()), FILE_NAME_NORMALIZED);
    const fs::path finalName =
        finalLength == 0U || finalLength >= finalPath.size()
            ? fs::path{}
            : fs::path{std::wstring_view{finalPath.data(), finalLength}}
                  .filename();
    if (current.handle != INVALID_HANDLE_VALUE)
        (void)CloseHandle(current.handle);
    if (!currentIdentity.has_value() ||
        !sameIdentity(*currentIdentity, identity) ||
        finalName.wstring() != wideName) {
        return unsafeError("avatar directory spelling or identity changed");
    }
    if (auto aliases = rejectAlias(name); !aliases.hasValue())
        return aliases.error();
    return revalidate();
}
#else
Result<void> RootAuthority::exactChildEntryMatches(
    std::string_view name, const struct stat& identity) const {
    const int duplicate = ::openat(
        descriptor_, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    DIR* directory = duplicate < 0 ? nullptr : ::fdopendir(duplicate);
    if (directory == nullptr) {
        if (duplicate >= 0) (void)::close(duplicate);
        return ioError("avatar root could not be enumerated");
    }
    bool foundExact = false;
    const std::string requestedKey = domain::portablePathAliasKey(name);
    errno = 0;
    while (const dirent* entry = ::readdir(directory)) {
        const std::string child{entry->d_name};
        if (child == "." || child == "..") continue;
        if (domain::portablePathAliasKey(child) != requestedKey) continue;
        struct stat information {};
        if (::fstatat(descriptor_, child.c_str(), &information,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            (void)::closedir(directory);
            return ioError("avatar child could not be inspected");
        }
        if (child != name || !S_ISDIR(information.st_mode) ||
            S_ISLNK(information.st_mode)) {
            (void)::closedir(directory);
            return unsafeError("avatar directory has a non-canonical alias");
        }
        if (!sameIdentity(information, identity)) {
            (void)::closedir(directory);
            return unsafeError("avatar directory identity changed");
        }
        foundExact = true;
    }
    const int reason = errno;
    (void)::closedir(directory);
    if (reason != 0) return ioError("avatar root enumeration failed");
    if (!foundExact)
        return unsafeError("avatar directory exact name disappeared");
    return revalidate();
}
#endif

Result<ChildAuthority> RootAuthority::child(std::string_view name,
                                            bool create) const {
    if (!domain::isPortableLowercasePathComponent(name)) {
        return unsafeError("avatar id is not a portable path component");
    }
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
    if (auto aliases = rejectAlias(name); !aliases.hasValue())
        return aliases.error();
#ifdef _WIN32
    const std::wstring wideName = fs::path{name}.wstring();
    const auto opened = openRelative(
        handle_, wideName,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES |
            FILE_WRITE_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        create ? FILE_OPEN_IF : FILE_OPEN,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
    if (opened.handle == INVALID_HANDLE_VALUE) {
        if (!create && missingStatus(opened.status)) return notFoundError();
        if (reparseStatus(opened.status))
            return unsafeError("avatar directory is a reparse point");
        return ioError("avatar directory could not be opened");
    }
    auto identity = identityOf(opened.handle, true);
    if (!identity.has_value()) {
        (void)CloseHandle(opened.handle);
        return unsafeError("avatar path must be a plain directory");
    }
    std::array<wchar_t, 1024> finalPath{};
    const DWORD finalLength = GetFinalPathNameByHandleW(
        opened.handle, finalPath.data(), static_cast<DWORD>(finalPath.size()),
        FILE_NAME_NORMALIZED);
    const fs::path finalName =
        finalLength == 0U || finalLength >= finalPath.size()
            ? fs::path{}
            : fs::path{std::wstring_view{finalPath.data(), finalLength}}
                  .filename();
    if (finalName.wstring() != wideName) {
        (void)CloseHandle(opened.handle);
        return unsafeError("avatar directory spelling is not canonical");
    }
    if (create && FlushFileBuffers(handle_) == FALSE) {
        (void)CloseHandle(opened.handle);
        return ioError("avatar root directory could not be flushed");
    }
    if (auto exact = exactChildEntryMatches(name, *identity);
        !exact.hasValue()) {
        (void)CloseHandle(opened.handle);
        return exact.error();
    }
    return ChildAuthority{this, std::string{name}, opened.handle, *identity};
#else
    const std::string childName{name};
    struct stat named {};
    if (::fstatat(descriptor_, childName.c_str(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (!create || errno != ENOENT) {
            return errno == ENOENT ? Result<ChildAuthority>{notFoundError()}
                                   : Result<ChildAuthority>{ioError(
                                         "avatar directory could not be inspected")};
        }
        if (::mkdirat(descriptor_, childName.c_str(), 0700) != 0 &&
            errno != EEXIST) {
            return ioError("avatar directory could not be created");
        }
    } else if (!S_ISDIR(named.st_mode) || S_ISLNK(named.st_mode)) {
        return unsafeError("avatar path must be a plain directory");
    }
    if (create && ::fsync(descriptor_) != 0) {
        return ioError("avatar root directory could not be flushed");
    }
    const int childDescriptor =
        ::openat(descriptor_, childName.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat opened {};
    struct stat current {};
    if (childDescriptor < 0 || ::fstat(childDescriptor, &opened) != 0 ||
        opened.st_nlink == 0 || !S_ISDIR(opened.st_mode) ||
        ::fstatat(descriptor_, childName.c_str(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameIdentity(opened, current)) {
        if (childDescriptor >= 0) (void)::close(childDescriptor);
        return unsafeError("avatar directory binding is unsafe");
    }
    if (auto exact = exactChildEntryMatches(name, opened);
        !exact.hasValue()) {
        (void)::close(childDescriptor);
        return exact.error();
    }
    return ChildAuthority{this, childName, childDescriptor, opened};
#endif
}

ChildAuthority::ChildAuthority(ChildAuthority&& other) noexcept
    : root_(other.root_), name_(std::move(other.name_))
#ifdef _WIN32
      , handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
      identity_(other.identity_)
#else
      , descriptor_(std::exchange(other.descriptor_, -1)),
      identity_(other.identity_)
#endif
{}

ChildAuthority& ChildAuthority::operator=(ChildAuthority&& other) noexcept {
    if (this == &other) return *this;
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) (void)CloseHandle(handle_);
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
#else
    if (descriptor_ >= 0) (void)::close(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, -1);
#endif
    root_ = other.root_;
    name_ = std::move(other.name_);
    identity_ = other.identity_;
    return *this;
}

ChildAuthority::~ChildAuthority() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) (void)CloseHandle(handle_);
#else
    if (descriptor_ >= 0) (void)::close(descriptor_);
#endif
}

Result<void> ChildAuthority::revalidate() const {
    if (root_ == nullptr) return unsafeError("avatar child authority is absent");
    if (auto valid = root_->revalidate(); !valid.hasValue()) return valid.error();
#ifdef _WIN32
    auto retained = identityOf(handle_, true);
    const std::wstring wideName = fs::path{name_}.wstring();
    const auto current = openRelative(
        root_->handle(), wideName, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
    auto currentIdentity = identityOf(current.handle, true);
    if (current.handle != INVALID_HANDLE_VALUE) (void)CloseHandle(current.handle);
    if (!retained.has_value() || !currentIdentity.has_value() ||
        !sameIdentity(*retained, identity_) ||
        !sameIdentity(*currentIdentity, identity_)) {
        return unsafeError("avatar directory identity changed");
    }
#else
    struct stat retained {};
    struct stat named {};
    if (descriptor_ < 0 || ::fstat(descriptor_, &retained) != 0 ||
        retained.st_nlink == 0 || !S_ISDIR(retained.st_mode) ||
        !sameIdentity(retained, identity_) ||
        ::fstatat(root_->descriptor(), name_.c_str(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !sameIdentity(named, identity_)) {
        return unsafeError("avatar directory identity changed");
    }
#endif
    return root_->exactChildEntryMatches(name_, identity_);
}

#ifdef _WIN32
Result<void> finalizeReadIdentity(
    HANDLE retained, HANDLE child, std::wstring_view name,
    const WindowsIdentity& initialIdentity, std::uint64_t initialSize,
    std::size_t bytesRead) {
    BY_HANDLE_FILE_INFORMATION retainedInformation{};
    LARGE_INTEGER finalSize{};
    const auto retainedIdentity = identityOf(retained, false);
    if (!retainedIdentity.has_value() ||
        GetFileInformationByHandle(retained, &retainedInformation) == FALSE ||
        retainedInformation.nNumberOfLinks != 1U) {
        return unsafeError("avatar spec retained file became unsafe");
    }
    const auto named = openRelative(
        child, name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
    if (named.handle == INVALID_HANDLE_VALUE) {
        if (reparseStatus(named.status))
            return unsafeError("avatar spec named file became a reparse point");
        return ioError("avatar spec named file changed while reading");
    }
    BY_HANDLE_FILE_INFORMATION namedInformation{};
    const auto namedIdentity = identityOf(named.handle, false);
    const bool namedSafe =
        namedIdentity.has_value() &&
        GetFileInformationByHandle(named.handle, &namedInformation) != FALSE &&
        namedInformation.nNumberOfLinks == 1U;
    (void)CloseHandle(named.handle);
    if (!namedSafe)
        return unsafeError("avatar spec named file became unsafe");
    if (!sameIdentity(*retainedIdentity, initialIdentity) ||
        !sameIdentity(*namedIdentity, initialIdentity) ||
        GetFileSizeEx(retained, &finalSize) == FALSE ||
        finalSize.QuadPart < 0 ||
        static_cast<std::uint64_t>(finalSize.QuadPart) != initialSize ||
        static_cast<std::uint64_t>(finalSize.QuadPart) != bytesRead) {
        return ioError("avatar spec file changed while reading");
    }
    return core::ok();
}
#else
Result<void> finalizeReadIdentity(
    int retained, int child, std::string_view name,
    const struct stat& initialIdentity, std::size_t bytesRead) {
    struct stat retainedInformation {};
    struct stat namedInformation {};
    if (::fstat(retained, &retainedInformation) != 0) {
        return ioError("avatar spec retained file could not be inspected");
    }
    if (!S_ISREG(retainedInformation.st_mode) ||
        retainedInformation.st_nlink != 1) {
        return unsafeError("avatar spec retained file became unsafe");
    }
    if (::fstatat(child, std::string{name}.c_str(), &namedInformation,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        return ioError("avatar spec named file changed while reading");
    }
    if (!S_ISREG(namedInformation.st_mode) ||
        S_ISLNK(namedInformation.st_mode) ||
        namedInformation.st_nlink != 1) {
        return unsafeError("avatar spec named file became unsafe");
    }
    if (!sameIdentity(retainedInformation, initialIdentity) ||
        !sameIdentity(namedInformation, initialIdentity) ||
        retainedInformation.st_size != initialIdentity.st_size ||
        retainedInformation.st_size < 0 ||
        static_cast<std::uint64_t>(retainedInformation.st_size) !=
            bytesRead) {
        return ioError("avatar spec file changed while reading");
    }
    return core::ok();
}
#endif

Result<std::string> ChildAuthority::read(std::string_view fileName) const {
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
#ifdef _WIN32
    const std::wstring wideName = fs::path{fileName}.wstring();
    const auto opened = openRelative(
        handle_, wideName, FILE_GENERIC_READ | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
    if (opened.handle == INVALID_HANDLE_VALUE) {
        if (missingStatus(opened.status)) return notFoundError();
        if (reparseStatus(opened.status))
            return unsafeError("avatar spec file is a reparse point");
        return ioError("avatar spec file could not be opened");
    }
    const auto identity = identityOf(opened.handle, false);
    BY_HANDLE_FILE_INFORMATION information{};
    LARGE_INTEGER initialSize{};
    if (!identity.has_value() ||
        GetFileInformationByHandle(opened.handle, &information) == FALSE ||
        information.nNumberOfLinks != 1U ||
        GetFileSizeEx(opened.handle, &initialSize) == FALSE) {
        (void)CloseHandle(opened.handle);
        return unsafeError("avatar spec file identity is unsafe");
    }
    if (initialSize.QuadPart < 0 ||
        static_cast<std::uint64_t>(initialSize.QuadPart) > kMaximumFileSize) {
        (void)CloseHandle(opened.handle);
        return parseError("avatar spec file exceeds 8 MiB");
    }
    std::string contents;
    contents.reserve(static_cast<std::size_t>(initialSize.QuadPart));
    std::array<char, kIoChunkSize> buffer{};
    for (;;) {
        DWORD count = 0U;
        if (ReadFile(opened.handle, buffer.data(),
                     static_cast<DWORD>(buffer.size()), &count, nullptr) == FALSE) {
            (void)CloseHandle(opened.handle);
            return ioError("avatar spec file could not be read");
        }
        if (count == 0U) break;
        contents.append(buffer.data(), count);
        if (contents.size() > kMaximumFileSize) {
            (void)CloseHandle(opened.handle);
            return parseError("avatar spec file exceeds 8 MiB");
        }
    }
    auto finalIdentity = finalizeReadIdentity(
        opened.handle, handle_, wideName, *identity,
        static_cast<std::uint64_t>(initialSize.QuadPart), contents.size());
    (void)CloseHandle(opened.handle);
    if (!finalIdentity.hasValue()) return finalIdentity.error();
#else
    const std::string name{fileName};
    const int opened = ::openat(
        descriptor_, name.c_str(),
        O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (opened < 0) {
        if (errno == ENOENT) return notFoundError();
        if (errno == ELOOP) return unsafeError("avatar spec file is a symbolic link");
        return ioError("avatar spec file could not be opened");
    }
    ScopedDescriptor file{opened};
    struct stat initial {};
    if (::fstat(file.get(), &initial) != 0) {
        return ioError("avatar spec file identity could not be read");
    }
    if (!S_ISREG(initial.st_mode) || initial.st_nlink != 1) {
        return unsafeError("avatar spec file identity is unsafe");
    }
    if (initial.st_size < 0 ||
        static_cast<std::uint64_t>(initial.st_size) > kMaximumFileSize) {
        return parseError("avatar spec file exceeds 8 MiB");
    }
    std::string contents;
    contents.reserve(static_cast<std::size_t>(initial.st_size));
    std::array<char, kIoChunkSize> buffer{};
    for (;;) {
        const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return ioError("avatar spec file could not be read");
        if (count == 0) break;
        contents.append(buffer.data(), static_cast<std::size_t>(count));
        if (contents.size() > kMaximumFileSize) {
            return parseError("avatar spec file exceeds 8 MiB");
        }
    }
    if (auto finalIdentity = finalizeReadIdentity(
            file.get(), descriptor_, name, initial, contents.size());
        !finalIdentity.hasValue()) {
        return finalIdentity.error();
    }
#endif
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
    return contents;
}

AppError withCleanupFailure(const AppError& primary) {
    return {primary.code(), primary.message() + "; temporary cleanup failed",
            primary.issueCode(), primary.messageKey()};
}

#ifdef _WIN32
Result<void> inspectPromotedTarget(
    HANDLE child, std::wstring_view name,
    const WindowsIdentity& expectedIdentity) {
    const auto promoted = openRelative(
        child, name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
    if (promoted.handle == INVALID_HANDLE_VALUE) {
        if (reparseStatus(promoted.status))
            return unsafeError("promoted avatar spec is a reparse point");
        return ioError("promoted avatar spec could not be opened");
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const auto identity = identityOf(promoted.handle, false);
    const bool safe =
        identity.has_value() &&
        GetFileInformationByHandle(promoted.handle, &information) != FALSE &&
        information.nNumberOfLinks == 1U &&
        sameIdentity(*identity, expectedIdentity);
    (void)CloseHandle(promoted.handle);
    if (!safe)
        return unsafeError("promoted avatar spec identity is unsafe");
    return core::ok();
}
#else
Result<void> inspectPromotedTarget(
    int child, std::string_view name,
    const struct stat& expectedIdentity) {
    const std::string target{name};
    const int opened = ::openat(
        child, target.c_str(),
        O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (opened < 0) {
        if (errno == ELOOP)
            return unsafeError("promoted avatar spec is a symbolic link");
        return ioError("promoted avatar spec could not be opened");
    }
    ScopedDescriptor promoted{opened};
    struct stat information {};
    if (::fstat(promoted.get(), &information) != 0) {
        return ioError("promoted avatar spec could not be inspected");
    }
    if (!S_ISREG(information.st_mode) || information.st_nlink != 1 ||
        !sameIdentity(information, expectedIdentity)) {
        return unsafeError("promoted avatar spec identity is unsafe");
    }
    return core::ok();
}
#endif

Result<void> ChildAuthority::write(std::string_view fileName,
                                   std::string_view contents) const {
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
    const std::string target{fileName};
    const std::string temporary =
        ".avatar.part-" + core::generateUuidV4();
#ifdef _WIN32
    const std::wstring wideTarget = fs::path{target}.wstring();
    const std::wstring wideTemporary = fs::path{temporary}.wstring();
    const auto inspectTarget = [&]() -> Result<void> {
        const auto existing = openRelative(
            handle_, wideTarget, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                FILE_OPEN_REPARSE_POINT);
        if (existing.handle != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information{};
            const bool safe =
                identityOf(existing.handle, false).has_value() &&
                GetFileInformationByHandle(existing.handle, &information) !=
                    FALSE &&
                information.nNumberOfLinks == 1U;
            (void)CloseHandle(existing.handle);
            if (!safe)
                return unsafeError("avatar spec target identity is unsafe");
        } else if (!missingStatus(existing.status)) {
            return reparseStatus(existing.status)
                       ? Result<void>{unsafeError(
                             "avatar spec target is a reparse point")}
                       : Result<void>{ioError(
                             "avatar spec target could not be inspected")};
        }
        return core::ok();
    };
    if (auto inspected = inspectTarget(); !inspected.hasValue())
        return inspected.error();
    const auto created = openRelative(
        handle_, wideTemporary,
        FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE,
        0U, FILE_CREATE,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
    if (created.handle == INVALID_HANDLE_VALUE)
        return ioError("avatar spec temporary file could not be created");
    HANDLE temporaryHandle = created.handle;
    auto discardTemporary = [&](const AppError& failure) -> AppError {
        FILE_DISPOSITION_INFO disposition{TRUE};
        const bool removed =
            SetFileInformationByHandle(
                temporaryHandle, FileDispositionInfo, &disposition,
                static_cast<DWORD>(sizeof(disposition))) != FALSE;
        const bool closed = CloseHandle(temporaryHandle) != FALSE;
        temporaryHandle = INVALID_HANDLE_VALUE;
        return removed && closed ? failure : withCleanupFailure(failure);
    };
    BY_HANDLE_FILE_INFORMATION temporaryInformation{};
    const auto temporaryIdentity = identityOf(temporaryHandle, false);
    if (!temporaryIdentity.has_value() ||
        GetFileInformationByHandle(temporaryHandle,
                                   &temporaryInformation) == FALSE ||
        temporaryInformation.nNumberOfLinks != 1U) {
        return discardTemporary(
            unsafeError("avatar spec temporary file identity is unsafe"));
    }
    std::size_t written = 0U;
    while (written < contents.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            contents.size() - written,
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD count = 0U;
        if (WriteFile(temporaryHandle, contents.data() + written, chunk,
                      &count, nullptr) == FALSE ||
            count == 0U) {
            return discardTemporary(
                ioError("avatar spec temporary file could not be written"));
        }
        written += count;
    }
    if (FlushFileBuffers(temporaryHandle) == FALSE) {
        return discardTemporary(
            ioError("avatar spec temporary file could not be flushed"));
    }
    if (auto valid = revalidate(); !valid.hasValue()) {
        return discardTemporary(valid.error());
    }
    if (auto inspected = inspectTarget(); !inspected.hasValue()) {
        return discardTemporary(inspected.error());
    }
    if (!renameRelative(temporaryHandle, handle_, wideTarget)) {
        return discardTemporary(
            ioError("avatar spec target could not be replaced"));
    }
    if (CloseHandle(temporaryHandle) == FALSE) {
        temporaryHandle = INVALID_HANDLE_VALUE;
        return ioError("promoted avatar spec handle could not be closed");
    }
    temporaryHandle = INVALID_HANDLE_VALUE;
    if (FlushFileBuffers(handle_) == FALSE) {
        return ioError("avatar spec directory could not be flushed");
    }
    if (auto promoted = inspectPromotedTarget(
            handle_, wideTarget, *temporaryIdentity);
        !promoted.hasValue()) {
        return promoted.error();
    }
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
#else
    struct stat existing {};
    if (::fstatat(descriptor_, target.c_str(), &existing,
                  AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode) || existing.st_nlink != 1)
            return unsafeError("avatar spec target identity is unsafe");
    } else if (errno != ENOENT) {
        return ioError("avatar spec target could not be inspected");
    }
    const int created =
        ::openat(descriptor_, temporary.c_str(),
                 O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (created < 0)
        return ioError("avatar spec temporary file could not be created");
    ScopedDescriptor temporaryFile{created};
    auto cleanup = [&](const AppError& failure) -> AppError {
        if (::unlinkat(descriptor_, temporary.c_str(), 0) != 0 &&
            errno != ENOENT) {
            return withCleanupFailure(failure);
        }
        return failure;
    };
    struct stat temporaryIdentity {};
    if (::fstat(temporaryFile.get(), &temporaryIdentity) != 0 ||
        !S_ISREG(temporaryIdentity.st_mode) ||
        temporaryIdentity.st_nlink != 1) {
        return cleanup(
            unsafeError("avatar spec temporary file identity is unsafe"));
    }
    std::size_t written = 0U;
    while (written < contents.size()) {
        const ssize_t count =
            ::write(temporaryFile.get(), contents.data() + written,
                    contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            return cleanup(
                ioError("avatar spec temporary file could not be written"));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(temporaryFile.get()) != 0) {
        return cleanup(
            ioError("avatar spec temporary file could not be flushed"));
    }
    if (auto valid = revalidate(); !valid.hasValue()) {
        return cleanup(valid.error());
    }
    if (::fstatat(descriptor_, target.c_str(), &existing,
                  AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(existing.st_mode) || existing.st_nlink != 1) {
            return cleanup(
                unsafeError("avatar spec target identity is unsafe"));
        }
    } else if (errno != ENOENT) {
        return cleanup(
            ioError("avatar spec target could not be inspected"));
    }
    if (::renameat(descriptor_, temporary.c_str(),
                   descriptor_, target.c_str()) != 0) {
        return cleanup(
            ioError("avatar spec target could not be replaced"));
    }
    if (::fsync(descriptor_) != 0) {
        return ioError("avatar spec directory could not be flushed");
    }
    if (auto promoted = inspectPromotedTarget(
            descriptor_, target, temporaryIdentity);
        !promoted.hasValue()) {
        return promoted.error();
    }
    if (auto valid = revalidate(); !valid.hasValue()) return valid.error();
#endif
    return core::ok();
}

Result<AvatarSpec> decodeSpec(std::string_view contents,
                              const AvatarId& expectedId) {
    try {
        auto decoded =
            AvatarSpecCodec{}.fromJson(nlohmann::json::parse(contents));
        if (!decoded.hasValue()) return decoded.error();
        if (decoded.value().avatarId() != expectedId)
            return parseError("avatar spec id does not match its directory");
        return decoded;
    } catch (const nlohmann::json::exception& error) {
        return parseError("avatar spec JSON could not be parsed: " +
                          std::string{error.what()});
    }
}

Result<AvatarSpec> readDecoded(const ChildAuthority& child,
                               std::string_view fileName,
                               const AvatarId& expectedId) {
    auto contents = child.read(fileName);
    if (!contents.hasValue()) return contents.error();
    return decodeSpec(contents.value(), expectedId);
}

Result<std::string> encodeSpec(const AvatarSpec& spec) {
    try {
        std::string contents = AvatarSpecCodec{}.toJson(spec).dump(2);
        if (contents.size() > kMaximumFileSize) {
            return unsafeError("serialized avatar spec exceeds 8 MiB");
        }
        return contents;
    } catch (const nlohmann::json::exception& error) {
        return parseError("avatar spec JSON could not be serialized: " +
                          std::string{error.what()});
    }
}

}  // namespace

class AvatarSpecFileStore::Impl final {
public:
    explicit Impl(fs::path rootPath) {
        auto opened = RootAuthority::open(std::move(rootPath));
        if (opened.hasValue())
            root_.emplace(std::move(opened).value());
        else
            openError_ = opened.error();
    }

    [[nodiscard]] Result<RootAuthority*> root() {
        if (!root_.has_value()) return *openError_;
        return &*root_;
    }

    [[nodiscard]] Result<const RootAuthority*> root() const {
        if (!root_.has_value()) return *openError_;
        return &*root_;
    }

private:
    std::optional<RootAuthority> root_;
    std::optional<AppError> openError_;
};

AvatarSpecFileStore::AvatarSpecFileStore(fs::path avatarsRoot)
    : implementation_(std::make_unique<Impl>(std::move(avatarsRoot))) {}

AvatarSpecFileStore::~AvatarSpecFileStore() = default;

Result<void> AvatarSpecFileStore::save(const AvatarSpec& spec) {
    auto contents = encodeSpec(spec);
    if (!contents.hasValue()) return contents.error();
    auto root = implementation_->root();
    if (!root.hasValue()) return root.error();
    auto child = root.value()->child(spec.avatarId().value(), true);
    if (!child.hasValue()) return child.error();

    auto primary = readDecoded(child.value(), "avatar.json", spec.avatarId());
    if (primary.hasValue()) {
        auto previous = encodeSpec(primary.value());
        if (!previous.hasValue()) return previous.error();
        if (auto saved = child.value().write("avatar.last-good.json",
                                             previous.value());
            !saved.hasValue()) {
            return saved.error();
        }
    } else {
        if (!recoveryEligible(primary.error())) return primary.error();
        auto backup = readDecoded(child.value(), "avatar.last-good.json",
                                  spec.avatarId());
        if (!backup.hasValue()) {
            if (!recoveryEligible(backup.error())) return backup.error();
            if (backup.error().code() != ErrorCode::NotFound)
                return backup.error();
            if (auto saved = child.value().write("avatar.last-good.json",
                                                 contents.value());
                !saved.hasValue()) {
                return saved.error();
            }
        }
    }
    return child.value().write("avatar.json", contents.value());
}

Result<AvatarSpec> AvatarSpecFileStore::load(const AvatarId& id) const {
    if (!domain::isPortableLowercasePathComponent(id.value()))
        return unsafeError("avatar id is not a portable path component");
    auto root = implementation_->root();
    if (!root.hasValue()) return root.error();
    auto child = root.value()->child(id.value(), false);
    if (!child.hasValue()) return child.error();
    auto primary = readDecoded(child.value(), "avatar.json", id);
    if (primary.hasValue()) return primary;
    if (!recoveryEligible(primary.error())) return primary.error();
    auto backup = readDecoded(child.value(), "avatar.last-good.json", id);
    if (backup.hasValue()) return backup;
    if (!recoveryEligible(backup.error())) return backup.error();
    return primary.error();
}

Result<std::vector<AvatarId>> AvatarSpecFileStore::list() const {
    auto root = implementation_->root();
    if (!root.hasValue()) return root.error();
    auto names = root.value()->listChildren();
    if (!names.hasValue()) return names.error();
    std::vector<AvatarId> result;
    for (const auto& name : names.value()) {
        if (!domain::isPortableLowercasePathComponent(name)) continue;
        auto id = AvatarId::create(name);
        if (!id.hasValue()) continue;
        auto child = root.value()->child(name, false);
        if (!child.hasValue()) return child.error();
        auto primary = readDecoded(child.value(), "avatar.json", id.value());
        if (primary.hasValue()) {
            result.push_back(std::move(id).value());
            continue;
        }
        if (!recoveryEligible(primary.error())) return primary.error();
        auto backup =
            readDecoded(child.value(), "avatar.last-good.json", id.value());
        if (backup.hasValue()) {
            result.push_back(std::move(id).value());
        } else if (!recoveryEligible(backup.error())) {
            return backup.error();
        }
    }
    std::sort(result.begin(), result.end());
    if (auto valid = root.value()->revalidate(); !valid.hasValue())
        return valid.error();
    return result;
}

namespace internal {

Result<void> ensureAvatarDirectoryDurably(
    const fs::path& projectDirectory, std::string_view childName) noexcept {
    if (!domain::isPortableLowercasePathComponent(childName))
        return unsafeError("project avatars path is not portable");
    auto root = RootAuthority::open(projectDirectory);
    if (!root.hasValue()) return root.error();
    auto child = root.value().child(childName, true);
    if (!child.hasValue()) return child.error();
    return child.value().revalidate();
}

}  // namespace internal

}  // namespace creator::project_store
