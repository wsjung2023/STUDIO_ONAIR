#include "project_store/AvatarSpecFileStore.h"

#include "avatar/AvatarSpecCodec.h"
#include "core/AppError.h"
#include "project_store/internal/DurableFile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
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

constexpr std::uintmax_t kMaximumFileSize = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumAvatarIdBytes = 128U;

AppError pathError(std::string message) {
    return AppError{ErrorCode::InvalidArgument, std::move(message),
                    "avatar.spec.store.path", "avatar.validation.path"};
}

AppError ioError(std::string message) {
    return AppError{ErrorCode::IoFailure, std::move(message),
                    "avatar.spec.store.io", "avatar.validation.io"};
}

bool isReservedWindowsName(std::string_view value) {
    static constexpr std::array<std::string_view, 22> reserved{
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4",
        "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
        "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    return std::find(reserved.begin(), reserved.end(), value) != reserved.end();
}

Result<void> validateAvatarId(std::string_view value) {
    if (value.empty() || value.size() > kMaximumAvatarIdBytes ||
        value == "." || value == ".." || isReservedWindowsName(value)) {
        return pathError("avatar id is not a safe portable directory name");
    }
    const bool valid = std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
    });
    if (!valid) {
        return pathError("avatar id is not a safe portable directory name");
    }
    return core::ok();
}

Result<fs::file_status> inspect(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return fs::file_status{fs::file_type::not_found};
    }
    if (error) {
        return ioError("avatar spec path could not be inspected (code " +
                       std::to_string(error.value()) + ")");
    }
    return status;
}

Result<void> rejectReparsePoint(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return ioError("avatar spec path attributes could not be inspected (code " +
                       std::to_string(GetLastError()) + ")");
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return pathError("avatar spec paths must not contain reparse points");
    }
#else
    static_cast<void>(path);
#endif
    return core::ok();
}

Result<AvatarSpecDirectoryIdentity> directoryIdentity(const fs::path& path) {
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return ioError("avatar root identity could not be opened");
    }
    BY_HANDLE_FILE_INFORMATION info{};
    const bool valid = GetFileInformationByHandle(handle, &info) != FALSE;
    CloseHandle(handle);
    if (!valid || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return pathError("avatar root is not a plain directory");
    }
    return AvatarSpecDirectoryIdentity{
        (static_cast<std::uint64_t>(info.dwVolumeSerialNumber) << 32U) |
            info.nFileIndexHigh,
        info.nFileIndexLow};
#else
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        return ioError("avatar root identity could not be inspected");
    }
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
        return pathError("avatar root is not a plain directory");
    }
    return AvatarSpecDirectoryIdentity{
        static_cast<std::uint64_t>(info.st_dev),
        static_cast<std::uint64_t>(info.st_ino)};
#endif
}

bool sameIdentity(const AvatarSpecDirectoryIdentity& left,
                  const AvatarSpecDirectoryIdentity& right) {
    return left.first == right.first && left.second == right.second;
}

Result<void> validateRoot(
    const fs::path& root,
    const std::optional<AvatarSpecDirectoryIdentity>& expected,
    int rootDescriptor, void* rootHandle) {
    auto status = inspect(root);
    if (!status.hasValue()) return status.error();
    if (fs::is_symlink(status.value()) || !fs::is_directory(status.value())) {
        return pathError("avatar root must be an existing plain directory");
    }
    if (auto reparse = rejectReparsePoint(root); !reparse.hasValue()) {
        return reparse.error();
    }
    auto current = directoryIdentity(root);
    if (!current.hasValue()) return current.error();
    if (!expected || !sameIdentity(*expected, current.value())) {
        return pathError("avatar root identity changed");
    }
#ifndef _WIN32
    static_cast<void>(rootHandle);
    struct stat held {};
    if (rootDescriptor < 0 || ::fstat(rootDescriptor, &held) != 0 ||
        !S_ISDIR(held.st_mode) || held.st_nlink == 0 ||
        static_cast<std::uint64_t>(held.st_dev) != expected->first ||
        static_cast<std::uint64_t>(held.st_ino) != expected->second) {
        return pathError("avatar root identity changed");
    }
#else
    static_cast<void>(rootDescriptor);
    const auto heldHandle = static_cast<HANDLE>(rootHandle);
    BY_HANDLE_FILE_INFORMATION held{};
    if (heldHandle == nullptr || heldHandle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(heldHandle, &held)) {
        return pathError("avatar root identity changed");
    }
    const AvatarSpecDirectoryIdentity heldIdentity{
        (static_cast<std::uint64_t>(held.dwVolumeSerialNumber) << 32U) |
            held.nFileIndexHigh,
        held.nFileIndexLow};
    if ((held.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (held.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !sameIdentity(*expected, heldIdentity)) {
        return pathError("avatar root identity changed");
    }
#endif
    return core::ok();
}

Result<void> validateContainedDirectory(
    const fs::path& root, const fs::path& directory,
    const std::optional<AvatarSpecDirectoryIdentity>& rootIdentity,
    int rootDescriptor, void* rootHandle, bool create) {
    if (auto validRoot =
            validateRoot(root, rootIdentity, rootDescriptor, rootHandle);
        !validRoot.hasValue()) {
        return validRoot.error();
    }
    auto status = inspect(directory);
    if (!status.hasValue()) return status.error();
    if (status.value().type() == fs::file_type::not_found && create) {
        std::error_code error;
        if (!fs::create_directory(directory, error) && error) {
            return ioError("avatar directory could not be created (code " +
                           std::to_string(error.value()) + ")");
        }
        status = inspect(directory);
        if (!status.hasValue()) return status.error();
    }
    if (fs::is_symlink(status.value()) || !fs::is_directory(status.value())) {
        return pathError("avatar path must be a plain directory");
    }
    if (auto reparse = rejectReparsePoint(directory); !reparse.hasValue()) {
        return reparse.error();
    }
    std::error_code error;
    const fs::path canonicalRoot = fs::canonical(root, error);
    if (error) return ioError("avatar root could not be resolved");
    const fs::path canonicalDirectory = fs::canonical(directory, error);
    if (error) return ioError("avatar directory could not be resolved");
    if (canonicalDirectory.parent_path() != canonicalRoot) {
        return pathError("avatar directory resolves outside the avatar root");
    }
    return validateRoot(root, rootIdentity, rootDescriptor, rootHandle);
}

Result<bool> validateOptionalRegularFile(const fs::path& path) {
    auto status = inspect(path);
    if (!status.hasValue()) return status.error();
    if (status.value().type() == fs::file_type::not_found) return false;
    if (fs::is_symlink(status.value()) || !fs::is_regular_file(status.value())) {
        return pathError("avatar spec file must be a plain regular file");
    }
    if (auto reparse = rejectReparsePoint(path); !reparse.hasValue()) {
        return reparse.error();
    }
    std::error_code error;
    const auto links = fs::hard_link_count(path, error);
    if (error) return ioError("avatar spec file links could not be inspected");
    if (links != 1U) {
        return pathError("avatar spec files must not have hard links");
    }
    return true;
}

Result<AvatarSpec> readSpec(const fs::path& path, const AvatarId& expectedId) {
    auto regular = validateOptionalRegularFile(path);
    if (!regular.hasValue()) return regular.error();
    if (!regular.value()) {
        return AppError{ErrorCode::NotFound, "avatar spec file does not exist"};
    }
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error) return ioError("avatar spec file size could not be read");
    if (size > kMaximumFileSize) {
        return AppError{ErrorCode::ParseFailure, "avatar spec file exceeds 8 MiB"};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) return ioError("avatar spec file could not be opened");
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (input.gcount() != static_cast<std::streamsize>(contents.size()) ||
        (!input && !input.eof())) {
        return ioError("avatar spec file could not be read completely");
    }
    try {
        auto decoded = AvatarSpecCodec{}.fromJson(nlohmann::json::parse(contents));
        if (!decoded.hasValue()) return decoded.error();
        if (decoded.value().avatarId() != expectedId) {
            return AppError{ErrorCode::ParseFailure,
                            "avatar spec id does not match its directory"};
        }
        return decoded;
    } catch (const nlohmann::json::exception& error) {
        return AppError{ErrorCode::ParseFailure,
                        "avatar spec JSON could not be parsed: " +
                            std::string{error.what()}};
    }
}

Result<AvatarSpec> readContainedSpec(
    const fs::path& root, const fs::path& directory, const fs::path& path,
    const AvatarId& expectedId,
    const std::optional<AvatarSpecDirectoryIdentity>& rootIdentity,
    int rootDescriptor, void* rootHandle) {
    if (auto contained =
            validateContainedDirectory(root, directory, rootIdentity,
                                       rootDescriptor, rootHandle, false);
        !contained.hasValue()) {
        return contained.error();
    }
    return readSpec(path, expectedId);
}

Result<void> writeSpec(
    const fs::path& root, const fs::path& directory, const fs::path& target,
    std::string_view contents,
    const std::optional<AvatarSpecDirectoryIdentity>& rootIdentity,
    int rootDescriptor, void* rootHandle) {
    if (auto contained =
            validateContainedDirectory(root, directory, rootIdentity,
                                       rootDescriptor, rootHandle, false);
        !contained.hasValue()) {
        return contained.error();
    }
    auto existing = validateOptionalRegularFile(target);
    if (!existing.hasValue()) return existing.error();
    if (auto validRoot =
            validateRoot(root, rootIdentity, rootDescriptor, rootHandle);
        !validRoot.hasValue()) {
        return validRoot.error();
    }
    return internal::writeFileDurably(target, contents);
}

}  // namespace

AvatarSpecFileStore::AvatarSpecFileStore(fs::path avatarsRoot)
    : avatarsRoot_(std::move(avatarsRoot)) {
    auto identity = directoryIdentity(avatarsRoot_);
    if (identity.hasValue()) {
#ifdef _WIN32
        HANDLE handle = CreateFileW(
            avatarsRoot_.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return;
        BY_HANDLE_FILE_INFORMATION held{};
        if (!GetFileInformationByHandle(handle, &held)) {
            CloseHandle(handle);
            return;
        }
        const AvatarSpecDirectoryIdentity heldIdentity{
            (static_cast<std::uint64_t>(held.dwVolumeSerialNumber) << 32U) |
                held.nFileIndexHigh,
            held.nFileIndexLow};
        if ((held.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (held.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            !sameIdentity(identity.value(), heldIdentity)) {
            CloseHandle(handle);
            return;
        }
        rootHandle_ = handle;
#else
        rootDescriptor_ =
            ::open(avatarsRoot_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (rootDescriptor_ < 0) return;
        struct stat held {};
        if (::fstat(rootDescriptor_, &held) != 0 || !S_ISDIR(held.st_mode) ||
            held.st_nlink == 0 ||
            static_cast<std::uint64_t>(held.st_dev) != identity.value().first ||
            static_cast<std::uint64_t>(held.st_ino) != identity.value().second) {
            ::close(rootDescriptor_);
            rootDescriptor_ = -1;
            return;
        }
#endif
        rootIdentity_ = identity.value();
    }
}

AvatarSpecFileStore::~AvatarSpecFileStore() {
#ifdef _WIN32
    if (rootHandle_ != nullptr) CloseHandle(static_cast<HANDLE>(rootHandle_));
#else
    if (rootDescriptor_ >= 0) ::close(rootDescriptor_);
#endif
}

Result<void> AvatarSpecFileStore::save(const AvatarSpec& spec) {
    if (auto validId = validateAvatarId(spec.avatarId().value());
        !validId.hasValue()) {
        return validId.error();
    }
    const fs::path directory = avatarsRoot_ / spec.avatarId().value();
    if (auto contained =
            validateContainedDirectory(avatarsRoot_, directory, rootIdentity_,
                                       rootDescriptor_, rootHandle_, true);
        !contained.hasValue()) {
        return contained.error();
    }

    std::string contents;
    try {
        contents = AvatarSpecCodec{}.toJson(spec).dump(2);
    } catch (const nlohmann::json::exception& error) {
        return AppError{ErrorCode::ParseFailure,
                        "avatar spec JSON could not be serialized: " +
                            std::string{error.what()}};
    }
    const fs::path primary = directory / "avatar.json";
    const fs::path lastGood = directory / "avatar.last-good.json";

    auto previous = readContainedSpec(avatarsRoot_, directory, primary,
                                      spec.avatarId(), rootIdentity_,
                                      rootDescriptor_, rootHandle_);
    if (previous.hasValue()) {
        const std::string previousContents =
            AvatarSpecCodec{}.toJson(previous.value()).dump(2);
        if (auto saved = writeSpec(avatarsRoot_, directory, lastGood,
                                   previousContents, rootIdentity_,
                                   rootDescriptor_, rootHandle_);
            !saved.hasValue()) {
            return saved.error();
        }
    } else {
        auto backup = readContainedSpec(avatarsRoot_, directory, lastGood,
                                        spec.avatarId(), rootIdentity_,
                                        rootDescriptor_, rootHandle_);
        if (!backup.hasValue() && backup.error().code() == ErrorCode::NotFound) {
            if (auto saved = writeSpec(avatarsRoot_, directory, lastGood,
                                       contents, rootIdentity_, rootDescriptor_,
                                       rootHandle_);
                !saved.hasValue()) {
                return saved.error();
            }
        } else if (!backup.hasValue()) {
            return backup.error();
        }
    }
    return writeSpec(avatarsRoot_, directory, primary, contents, rootIdentity_,
                     rootDescriptor_, rootHandle_);
}

Result<AvatarSpec> AvatarSpecFileStore::load(const AvatarId& id) const {
    if (auto validId = validateAvatarId(id.value()); !validId.hasValue()) {
        return validId.error();
    }
    const fs::path directory = avatarsRoot_ / id.value();
    if (auto contained =
            validateContainedDirectory(avatarsRoot_, directory, rootIdentity_,
                                       rootDescriptor_, rootHandle_, false);
        !contained.hasValue()) {
        return contained.error();
    }
    auto primary = readContainedSpec(avatarsRoot_, directory,
                                     directory / "avatar.json", id,
                                     rootIdentity_, rootDescriptor_, rootHandle_);
    if (primary.hasValue()) return primary;
    auto backup = readContainedSpec(avatarsRoot_, directory,
                                    directory / "avatar.last-good.json", id,
                                    rootIdentity_, rootDescriptor_, rootHandle_);
    if (backup.hasValue()) return backup;
    return primary.error();
}

Result<std::vector<AvatarId>> AvatarSpecFileStore::list() const {
    if (auto validRoot =
            validateRoot(avatarsRoot_, rootIdentity_, rootDescriptor_,
                         rootHandle_);
        !validRoot.hasValue()) {
        return validRoot.error();
    }
    std::vector<AvatarId> result;
    std::error_code error;
    for (fs::directory_iterator iterator{avatarsRoot_, error};
         !error && iterator != fs::directory_iterator{}; iterator.increment(error)) {
        const fs::path directory = iterator->path();
        const std::string name = directory.filename().string();
        auto validId = validateAvatarId(name);
        if (!validId.hasValue()) continue;
        if (auto contained =
                validateContainedDirectory(avatarsRoot_, directory, rootIdentity_,
                                           rootDescriptor_, rootHandle_, false);
            !contained.hasValue()) {
            return contained.error();
        }
        auto id = AvatarId::create(name);
        if (!id.hasValue()) continue;
        auto primary = readContainedSpec(avatarsRoot_, directory,
                                         directory / "avatar.json", id.value(),
                                         rootIdentity_, rootDescriptor_,
                                         rootHandle_);
        if (!primary.hasValue()) {
            auto backup = readContainedSpec(
                avatarsRoot_, directory, directory / "avatar.last-good.json",
                id.value(), rootIdentity_, rootDescriptor_, rootHandle_);
            if (!backup.hasValue()) continue;
        }
        result.push_back(std::move(id).value());
    }
    if (error) return ioError("avatar directory could not be listed");
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace creator::project_store
