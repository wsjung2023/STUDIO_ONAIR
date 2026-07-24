#include "avatar_pack_adapter/CatalogRootAuthority.h"

#include "core/AppError.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Aclapi.h>
#include <Windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace creator::avatar_pack_adapter::detail {
namespace {

namespace fs = std::filesystem;
using core::AppError;
using core::ErrorCode;
using core::Result;

AppError authorityError(std::string issue) {
    return {ErrorCode::IoFailure,
            "avatar catalog root authority check failed",
            std::move(issue), "avatar.catalog.error"};
}

template <typename Character>
bool directChildName(std::basic_string_view<Character> name) noexcept {
    const auto dot = static_cast<Character>('.');
    return !name.empty() &&
           !(name.size() == 1U && name.front() == dot) &&
           !(name.size() == 2U && name.front() == dot &&
             name.back() == dot) &&
           name.find(static_cast<Character>('/')) == name.npos &&
           name.find(static_cast<Character>('\\')) == name.npos &&
           name.find(static_cast<Character>(':')) == name.npos &&
           name.find(static_cast<Character>('\0')) == name.npos;
}

#ifdef _WIN32
std::optional<std::vector<std::uint8_t>> currentUserSid() {
    HANDLE token = INVALID_HANDLE_VALUE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        return std::nullopt;
    DWORD required = 0U;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0U, &required);
    if (required == 0U || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return std::nullopt;
    }
    std::vector<std::uint8_t> information(required);
    if (GetTokenInformation(token, TokenUser, information.data(), required,
                            &required) == FALSE) {
        CloseHandle(token);
        return std::nullopt;
    }
    CloseHandle(token);
    const auto* user =
        reinterpret_cast<const TOKEN_USER*>(information.data());
    const auto sidLength = GetLengthSid(user->User.Sid);
    if (sidLength == 0U) return std::nullopt;
    std::vector<std::uint8_t> sid(sidLength);
    if (CopySid(sidLength, sid.data(), user->User.Sid) == FALSE)
        return std::nullopt;
    return sid;
}

bool trustedPrivateHandle(HANDLE handle) {
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
    auto currentUser = currentUserSid();
    if (!currentUser.has_value() || owner == nullptr ||
        IsValidSid(owner) == FALSE ||
        EqualSid(owner, currentUser->data()) == FALSE || dacl == nullptr) {
        if (descriptor != nullptr) (void)LocalFree(descriptor);
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
        (void)LocalFree(descriptor);
        return false;
    }

    ULONG entryCount = 0U;
    PEXPLICIT_ACCESSW entries = nullptr;
    if (GetExplicitEntriesFromAclW(dacl, &entryCount, &entries) !=
        ERROR_SUCCESS) {
        (void)LocalFree(descriptor);
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
        auto* sid = reinterpret_cast<PSID>(entry.Trustee.ptstrName);
        if (IsValidSid(sid) == FALSE ||
            (EqualSid(sid, currentUser->data()) == FALSE &&
             EqualSid(sid, systemSid.data()) == FALSE &&
             EqualSid(sid, administratorsSid.data()) == FALSE)) {
            trusted = false;
            break;
        }
    }
    if (entries != nullptr) (void)LocalFree(entries);
    (void)LocalFree(descriptor);
    return trusted;
}

HANDLE openRetainedDirectory(const fs::path& path) {
    return CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE | READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

struct RelativeHandleResult final {
    HANDLE handle{INVALID_HANDLE_VALUE};
    NTSTATUS status{};
    ULONG information{};
};

RelativeHandleResult openRelative(
    HANDLE parent,
    std::wstring_view name,
    ACCESS_MASK access,
    ULONG shareAccess,
    ULONG disposition,
    ULONG options,
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
    return {.handle = handle,
            .status = status,
            .information = static_cast<ULONG>(ioStatus.Information)};
}

RelativeHandleResult openRelativeDirectory(
    HANDLE parent,
    std::wstring_view name,
    ULONG disposition = FILE_OPEN) noexcept {
    return openRelative(
        parent, name,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE | READ_CONTROL | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, disposition,
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
            FILE_OPEN_REPARSE_POINT);
}

bool trustedDirectoryHandle(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION information{};
    return handle != INVALID_HANDLE_VALUE &&
           GetFileInformationByHandle(handle, &information) != FALSE &&
           (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ==
               0U &&
           trustedPrivateHandle(handle);
}

bool sameObject(const BY_HANDLE_FILE_INFORMATION& left,
                const BY_HANDLE_FILE_INFORMATION& right) noexcept {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
           left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
}
#else
bool trustedDirectoryStat(const struct stat& information) noexcept {
    return S_ISDIR(information.st_mode) &&
           information.st_uid == ::geteuid() &&
           (information.st_mode & 0077) == 0 &&
           (information.st_mode & 0700) == 0700;
}

bool sameObject(const struct stat& left,
                const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
#endif

}  // namespace

class CatalogOperationLock::Impl final {
public:
#ifdef _WIN32
    explicit Impl(HANDLE handle) : handle_(handle) {}

    ~Impl() {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        if (locked_)
            (void)UnlockFileEx(handle_, 0U, MAXDWORD, MAXDWORD, &overlap_);
        (void)CloseHandle(handle_);
    }

    HANDLE handle_{INVALID_HANDLE_VALUE};
    OVERLAPPED overlap_{};
    bool locked_{false};
#else
    explicit Impl(int descriptor) : descriptor_(descriptor) {}

    ~Impl() {
        if (descriptor_ < 0) return;
        (void)::flock(descriptor_, LOCK_UN);
        (void)::close(descriptor_);
    }

    int descriptor_{-1};
#endif
};

CatalogOperationLock::CatalogOperationLock(
    std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
CatalogOperationLock::CatalogOperationLock(
    CatalogOperationLock&&) noexcept = default;
CatalogOperationLock& CatalogOperationLock::operator=(
    CatalogOperationLock&&) noexcept = default;
CatalogOperationLock::~CatalogOperationLock() = default;

class CatalogRootAuthority::Impl final {
public:
    fs::path rootPath_;

#ifdef _WIN32
    Impl(fs::path rootPath, std::wstring rootName, HANDLE parentHandle,
         HANDLE rootHandle,
         BY_HANDLE_FILE_INFORMATION rootIdentity)
        : rootPath_(std::move(rootPath)),
          rootName_(std::move(rootName)),
          parentHandle_(parentHandle),
          rootHandle_(rootHandle),
          rootIdentity_(rootIdentity) {}

    ~Impl() {
        if (rootHandle_ != INVALID_HANDLE_VALUE)
            (void)CloseHandle(rootHandle_);
        if (parentHandle_ != INVALID_HANDLE_VALUE)
            (void)CloseHandle(parentHandle_);
    }

    std::wstring rootName_;
    HANDLE parentHandle_{INVALID_HANDLE_VALUE};
    HANDLE rootHandle_{INVALID_HANDLE_VALUE};
    BY_HANDLE_FILE_INFORMATION rootIdentity_{};
    mutable std::optional<BY_HANDLE_FILE_INFORMATION> lockIdentity_;
#else
    Impl(fs::path rootPath, std::string rootName, int parentDescriptor,
         int rootDescriptor, struct stat rootIdentity)
        : rootPath_(std::move(rootPath)),
          rootName_(std::move(rootName)),
          parentDescriptor_(parentDescriptor),
          rootDescriptor_(rootDescriptor),
          rootIdentity_(rootIdentity) {}

    ~Impl() {
        if (lockAnchorDescriptor_ >= 0)
            (void)::close(lockAnchorDescriptor_);
        if (rootDescriptor_ >= 0) (void)::close(rootDescriptor_);
        if (parentDescriptor_ >= 0) (void)::close(parentDescriptor_);
    }

    std::string rootName_;
    int parentDescriptor_{-1};
    int rootDescriptor_{-1};
    struct stat rootIdentity_ {};
    mutable std::optional<struct stat> lockIdentity_;
    mutable int lockAnchorDescriptor_{-1};
#endif
};

CatalogRootAuthority::CatalogRootAuthority(
    std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
CatalogRootAuthority::CatalogRootAuthority(
    CatalogRootAuthority&&) noexcept = default;
CatalogRootAuthority& CatalogRootAuthority::operator=(
    CatalogRootAuthority&&) noexcept = default;
CatalogRootAuthority::~CatalogRootAuthority() = default;

Result<CatalogRootAuthority> CatalogRootAuthority::open(
    fs::path rootPath) noexcept {
    try {
        std::error_code error;
        rootPath = fs::absolute(rootPath, error).lexically_normal();
        if (error || rootPath.empty() || !rootPath.has_filename())
            return authorityError("avatar.catalog.root");
        const auto parentPath = rootPath.parent_path();
#ifdef _WIN32
        const auto rootName = rootPath.filename().wstring();
        if (parentPath.empty() ||
            !directChildName(std::wstring_view{rootName}))
            return authorityError("avatar.catalog.root");
#else
        const auto rootName = rootPath.filename().string();
        if (parentPath.empty() ||
            !directChildName(std::string_view{rootName}))
            return authorityError("avatar.catalog.root");
#endif
#ifdef _WIN32
        const HANDLE parentHandle = openRetainedDirectory(parentPath);
        if (!trustedDirectoryHandle(parentHandle)) {
            if (parentHandle != INVALID_HANDLE_VALUE)
                (void)CloseHandle(parentHandle);
            return authorityError("avatar.catalog.parent");
        }
        const auto opened =
            openRelativeDirectory(parentHandle, rootName, FILE_OPEN_IF);
        const bool created = opened.information == FILE_CREATED;
        const HANDLE rootHandle = opened.handle;
        BY_HANDLE_FILE_INFORMATION rootIdentity{};
        if (!trustedDirectoryHandle(rootHandle) ||
            GetFileInformationByHandle(rootHandle, &rootIdentity) == FALSE) {
            if (rootHandle != INVALID_HANDLE_VALUE)
                (void)CloseHandle(rootHandle);
            (void)CloseHandle(parentHandle);
            return authorityError("avatar.catalog.root");
        }
        if (created && FlushFileBuffers(parentHandle) == FALSE) {
            (void)CloseHandle(rootHandle);
            (void)CloseHandle(parentHandle);
            return authorityError("avatar.catalog.parent-flush");
        }
        return CatalogRootAuthority{std::make_unique<Impl>(
            std::move(rootPath), rootName, parentHandle, rootHandle,
            rootIdentity)};
#else
        const int parentDescriptor =
            ::open(parentPath.c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat parentInformation {};
        if (parentDescriptor < 0 ||
            ::fstat(parentDescriptor, &parentInformation) != 0 ||
            !trustedDirectoryStat(parentInformation)) {
            if (parentDescriptor >= 0) (void)::close(parentDescriptor);
            return authorityError("avatar.catalog.parent");
        }
        struct stat rootInformation {};
        bool created = false;
        if (::fstatat(parentDescriptor, rootName.c_str(), &rootInformation,
                      AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno != ENOENT ||
                ::mkdirat(parentDescriptor, rootName.c_str(), 0700) != 0) {
                (void)::close(parentDescriptor);
                return authorityError("avatar.catalog.root");
            }
            created = true;
        } else if (!trustedDirectoryStat(rootInformation)) {
            (void)::close(parentDescriptor);
            return authorityError("avatar.catalog.root");
        }
        if (created && ::fsync(parentDescriptor) != 0) {
            (void)::close(parentDescriptor);
            return authorityError("avatar.catalog.parent-flush");
        }
        const int rootDescriptor =
            ::openat(parentDescriptor, rootName.c_str(),
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (rootDescriptor < 0 ||
            ::fstat(rootDescriptor, &rootInformation) != 0 ||
            !trustedDirectoryStat(rootInformation)) {
            if (rootDescriptor >= 0) (void)::close(rootDescriptor);
            (void)::close(parentDescriptor);
            return authorityError("avatar.catalog.root");
        }
        return CatalogRootAuthority{std::make_unique<Impl>(
            std::move(rootPath), rootName, parentDescriptor, rootDescriptor,
            rootInformation)};
#endif
    } catch (...) {
        return authorityError("avatar.catalog.root");
    }
}

const fs::path& CatalogRootAuthority::rootPath() const noexcept {
    return implementation_->rootPath_;
}

Result<void> CatalogRootAuthority::revalidate() const noexcept {
    try {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION retained{};
        if (!trustedDirectoryHandle(implementation_->parentHandle_) ||
            !trustedDirectoryHandle(implementation_->rootHandle_) ||
            GetFileInformationByHandle(implementation_->rootHandle_,
                                       &retained) == FALSE ||
            !sameObject(retained, implementation_->rootIdentity_)) {
            return authorityError("avatar.catalog.root-replaced");
        }
        const auto opened = openRelativeDirectory(
            implementation_->parentHandle_, implementation_->rootName_);
        const HANDLE current = opened.handle;
        BY_HANDLE_FILE_INFORMATION currentIdentity{};
        const bool matches =
            trustedDirectoryHandle(current) &&
            GetFileInformationByHandle(current, &currentIdentity) != FALSE &&
            sameObject(currentIdentity, implementation_->rootIdentity_);
        if (current != INVALID_HANDLE_VALUE) (void)CloseHandle(current);
        if (!matches)
            return authorityError("avatar.catalog.root-replaced");
#else
        struct stat parentInformation {};
        struct stat retained {};
        struct stat current {};
        if (::fstat(implementation_->parentDescriptor_,
                    &parentInformation) != 0 ||
            !trustedDirectoryStat(parentInformation) ||
            ::fstat(implementation_->rootDescriptor_, &retained) != 0 ||
            !trustedDirectoryStat(retained) ||
            !sameObject(retained, implementation_->rootIdentity_) ||
            ::fstatat(implementation_->parentDescriptor_,
                      implementation_->rootName_.c_str(), &current,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            !trustedDirectoryStat(current) ||
            !sameObject(current, implementation_->rootIdentity_)) {
            return authorityError("avatar.catalog.root-replaced");
        }
#endif
        return core::ok();
    } catch (...) {
        return authorityError("avatar.catalog.root-replaced");
    }
}

Result<CatalogOperationLock> CatalogRootAuthority::lock() const noexcept {
    try {
        auto valid = revalidate();
        if (!valid.hasValue()) return valid.error();
#ifdef _WIN32
        const auto opened = openRelative(
            implementation_->rootHandle_, L"catalog.lock",
            FILE_GENERIC_READ | FILE_GENERIC_WRITE | READ_CONTROL |
                SYNCHRONIZE,
            0U, FILE_OPEN_IF,
            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT |
                FILE_OPEN_REPARSE_POINT,
            FILE_ATTRIBUTE_HIDDEN);
        const HANDLE handle = opened.handle;
        if (handle == INVALID_HANDLE_VALUE) {
            constexpr NTSTATUS kSharingViolation =
                static_cast<NTSTATUS>(-1'073'741'757L);
            constexpr NTSTATUS kLockNotGranted =
                static_cast<NTSTATUS>(-1'073'741'739L);
            return authorityError(
                opened.status == kSharingViolation ||
                        opened.status == kLockNotGranted
                    ? "avatar.catalog.lock.busy"
                    : "avatar.catalog.lock");
        }
        BY_HANDLE_FILE_INFORMATION information{};
        if (GetFileInformationByHandle(handle, &information) == FALSE ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) !=
                0U ||
            information.nNumberOfLinks != 1U ||
            !trustedPrivateHandle(handle)) {
            (void)CloseHandle(handle);
            return authorityError("avatar.catalog.lock");
        }
        auto lock = std::make_unique<CatalogOperationLock::Impl>(handle);
        if (LockFileEx(
                handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0U, MAXDWORD, MAXDWORD, &lock->overlap_) == FALSE) {
            const auto reason = GetLastError();
            return authorityError(reason == ERROR_LOCK_VIOLATION
                                      ? "avatar.catalog.lock.busy"
                                      : "avatar.catalog.lock");
        }
        lock->locked_ = true;
        if (implementation_->lockIdentity_.has_value() &&
            !sameObject(*implementation_->lockIdentity_, information)) {
            return authorityError("avatar.catalog.lock-replaced");
        }
        if (!implementation_->lockIdentity_.has_value())
            implementation_->lockIdentity_ = information;
        return CatalogOperationLock{std::move(lock)};
#else
        if (implementation_->lockAnchorDescriptor_ >= 0) {
            struct stat retainedLock {};
            if (!implementation_->lockIdentity_.has_value() ||
                ::fstat(implementation_->lockAnchorDescriptor_,
                        &retainedLock) != 0 ||
                retainedLock.st_nlink != 1 ||
                !sameObject(retainedLock,
                            *implementation_->lockIdentity_)) {
                return authorityError("avatar.catalog.lock-replaced");
            }
        }
        const int descriptor =
            ::openat(implementation_->rootDescriptor_, "catalog.lock",
                     O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat information {};
        if (descriptor < 0 || ::fstat(descriptor, &information) != 0 ||
            !S_ISREG(information.st_mode) ||
            information.st_uid != ::geteuid() ||
            (information.st_mode & 0077) != 0) {
            if (descriptor >= 0) (void)::close(descriptor);
            return authorityError("avatar.catalog.lock");
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const auto reason = errno;
            (void)::close(descriptor);
            return authorityError(reason == EWOULDBLOCK || reason == EAGAIN
                                      ? "avatar.catalog.lock.busy"
                                      : "avatar.catalog.lock");
        }
        struct stat anchored {};
        if (information.st_nlink != 1 ||
            ::fstatat(implementation_->rootDescriptor_, "catalog.lock",
                      &anchored, AT_SYMLINK_NOFOLLOW) != 0 ||
            !sameObject(information, anchored)) {
            (void)::flock(descriptor, LOCK_UN);
            (void)::close(descriptor);
            return authorityError("avatar.catalog.lock-replaced");
        }
        if (implementation_->lockIdentity_.has_value() &&
            !sameObject(*implementation_->lockIdentity_, information)) {
            (void)::flock(descriptor, LOCK_UN);
            (void)::close(descriptor);
            return authorityError("avatar.catalog.lock-replaced");
        }
        if (!implementation_->lockIdentity_.has_value()) {
            const int anchor = ::openat(
                implementation_->rootDescriptor_, "catalog.lock",
                O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            struct stat anchorInformation {};
            if (anchor < 0 ||
                ::fstat(anchor, &anchorInformation) != 0 ||
                anchorInformation.st_nlink != 1 ||
                !sameObject(anchorInformation, information)) {
                if (anchor >= 0) (void)::close(anchor);
                (void)::flock(descriptor, LOCK_UN);
                (void)::close(descriptor);
                return authorityError("avatar.catalog.lock-replaced");
            }
            implementation_->lockIdentity_ = information;
            implementation_->lockAnchorDescriptor_ = anchor;
        }
        return CatalogOperationLock{
            std::make_unique<CatalogOperationLock::Impl>(descriptor)};
#endif
    } catch (...) {
        return authorityError("avatar.catalog.lock");
    }
}

Result<void> CatalogRootAuthority::ensurePrivateChild(
    std::string_view name) const noexcept {
    if (!directChildName(name))
        return authorityError("avatar.catalog.directory");
    auto valid = revalidate();
    if (!valid.hasValue()) return valid.error();
#ifdef _WIN32
    const auto childName = fs::path{name}.wstring();
    const auto opened = openRelativeDirectory(
        implementation_->rootHandle_, childName, FILE_OPEN_IF);
    const bool created = opened.information == FILE_CREATED;
    const HANDLE handle = opened.handle;
    const bool trusted = trustedDirectoryHandle(handle);
    if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
    if (!trusted) return authorityError("avatar.catalog.directory");
    if (created && FlushFileBuffers(implementation_->rootHandle_) == FALSE)
        return authorityError("avatar.catalog.root-flush");
#else
    const std::string childName{name};
    struct stat information {};
    bool created = false;
    if (::fstatat(implementation_->rootDescriptor_, childName.c_str(),
                  &information, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT ||
            ::mkdirat(implementation_->rootDescriptor_, childName.c_str(),
                      0700) != 0) {
            return authorityError("avatar.catalog.directory");
        }
        created = true;
    } else if (!trustedDirectoryStat(information)) {
        return authorityError("avatar.catalog.directory");
    }
    const int childDescriptor =
        ::openat(implementation_->rootDescriptor_, childName.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const bool trusted =
        childDescriptor >= 0 &&
        ::fstat(childDescriptor, &information) == 0 &&
        trustedDirectoryStat(information);
    if (childDescriptor >= 0) (void)::close(childDescriptor);
    if (!trusted) return authorityError("avatar.catalog.directory");
    if (created && ::fsync(implementation_->rootDescriptor_) != 0)
        return authorityError("avatar.catalog.root-flush");
#endif
    return core::ok();
}

bool isTrustedPrivateDirectory(const fs::path& path) noexcept {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const bool trusted = trustedDirectoryHandle(handle);
    if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
    return trusted;
#else
    struct stat information {};
    return ::lstat(path.c_str(), &information) == 0 &&
           trustedDirectoryStat(information);
#endif
}

Result<void> ensurePrivateDirectoryChild(
    const fs::path& parent, std::string_view childName) noexcept {
    if (!directChildName(childName))
        return authorityError("avatar.catalog.directory");
#ifdef _WIN32
    const HANDLE parentHandle = openRetainedDirectory(parent);
    if (!trustedDirectoryHandle(parentHandle)) {
        if (parentHandle != INVALID_HANDLE_VALUE)
            (void)CloseHandle(parentHandle);
        return authorityError("avatar.catalog.directory");
    }
    const auto wideChildName = fs::path{childName}.wstring();
    const auto opened = openRelativeDirectory(
        parentHandle, wideChildName, FILE_OPEN_IF);
    const HANDLE childHandle = opened.handle;
    const bool trusted = trustedDirectoryHandle(childHandle);
    if (childHandle != INVALID_HANDLE_VALUE) (void)CloseHandle(childHandle);
    const bool durable =
        opened.information != FILE_CREATED ||
        FlushFileBuffers(parentHandle) != FALSE;
    (void)CloseHandle(parentHandle);
    return trusted && durable ? core::ok()
                   : Result<void>{authorityError(
                         "avatar.catalog.directory")};
#else
    const int parentDescriptor =
        ::open(parent.c_str(),
               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat information {};
    if (parentDescriptor < 0 ||
        ::fstat(parentDescriptor, &information) != 0 ||
        !trustedDirectoryStat(information)) {
        if (parentDescriptor >= 0) (void)::close(parentDescriptor);
        return authorityError("avatar.catalog.directory");
    }
    const std::string child{childName};
    bool created = false;
    if (::fstatat(parentDescriptor, child.c_str(), &information,
                  AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT ||
            ::mkdirat(parentDescriptor, child.c_str(), 0700) != 0) {
            (void)::close(parentDescriptor);
            return authorityError("avatar.catalog.directory");
        }
        created = true;
    } else if (!trustedDirectoryStat(information)) {
        (void)::close(parentDescriptor);
        return authorityError("avatar.catalog.directory");
    }
    const int childDescriptor =
        ::openat(parentDescriptor, child.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const bool trusted =
        childDescriptor >= 0 &&
        ::fstat(childDescriptor, &information) == 0 &&
        trustedDirectoryStat(information);
    if (childDescriptor >= 0) (void)::close(childDescriptor);
    const bool durable = !created || ::fsync(parentDescriptor) == 0;
    (void)::close(parentDescriptor);
    return trusted && durable ? core::ok()
                   : Result<void>{authorityError(
                         "avatar.catalog.directory")};
#endif
}

}  // namespace creator::avatar_pack_adapter::detail
