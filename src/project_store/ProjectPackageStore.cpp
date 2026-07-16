#include "project_store/ProjectPackageStore.h"

#include "core/AppError.h"
#include "core/Uuid.h"
#include "project_store/JsonProjectStore.h"
#include "project_store/SqliteProjectDatabase.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <stdio.h>
#endif
#endif

namespace creator::project_store {
namespace {

namespace fs = std::filesystem;

using core::AppError;
using core::ErrorCode;
using core::Result;

struct ValidatedDatabase final {
    ProjectPackage package;
    SqliteProjectDatabase database;
};

AppError filesystemError(std::string_view operation, const std::error_code& error) {
    return AppError{ErrorCode::IoFailure,
                    "project package " + std::string{operation} + " failed (code " +
                        std::to_string(error.value()) + ")"};
}

Result<fs::path> validatedRelativeDatabasePath(std::string_view value) {
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        return AppError{ErrorCode::InvalidArgument,
                        "manifest database path must be a non-empty relative path"};
    }
    std::string portable{value};
    std::replace(portable.begin(), portable.end(), '\\', '/');
    if (portable.front() == '/' ||
        (portable.size() >= 2 && portable[1] == ':')) {
        return AppError{ErrorCode::InvalidArgument, "manifest database path must be relative"};
    }
    try {
        std::u8string utf8;
        utf8.reserve(portable.size());
        for (const unsigned char byte : portable) {
            utf8.push_back(static_cast<char8_t>(byte));
        }
        const fs::path normalized = fs::path{utf8}.lexically_normal();
        if (normalized.empty() || normalized == fs::path{"."} || normalized.is_absolute() ||
            normalized.has_root_directory() || normalized.has_root_name()) {
            return AppError{ErrorCode::InvalidArgument,
                            "manifest database path must resolve inside the package"};
        }
        for (const auto& component : normalized) {
            if (component == fs::path{".."}) {
                return AppError{ErrorCode::InvalidArgument,
                                "manifest database path must not escape the package"};
            }
        }
        return normalized;
    } catch (const std::system_error&) {
        return AppError{ErrorCode::InvalidArgument,
                        "manifest database path is not valid UTF-8"};
    }
}

bool isPathInside(const fs::path& parent, const fs::path& child) {
    auto parentPart = parent.begin();
    auto childPart = child.begin();
    for (; parentPart != parent.end(); ++parentPart, ++childPart) {
        if (childPart == child.end() || *parentPart != *childPart) return false;
    }
    return true;
}

Result<fs::path> validatedExistingDatabase(const fs::path& packagePath,
                                           const fs::path& relativePath) {
    const fs::path candidate = packagePath / relativePath;
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(candidate, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        status.type() == fs::file_type::not_found) {
        return AppError{ErrorCode::NotFound, "project package database was not found"};
    }
    if (ec) return filesystemError("inspect database", ec);
    if (fs::is_symlink(status)) {
        return AppError{ErrorCode::InvalidArgument,
                        "project package database must not be a symbolic link"};
    }
    if (!fs::is_regular_file(status)) {
        return AppError{ErrorCode::InvalidArgument,
                        "project package database must be a regular file"};
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return filesystemError("inspect database attributes",
                               std::error_code{static_cast<int>(GetLastError()),
                                               std::system_category()});
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "project package database must not be a reparse point"};
    }
#endif
    const auto links = fs::hard_link_count(candidate, ec);
    if (ec) return filesystemError("inspect database links", ec);
    if (links != 1) {
        return AppError{ErrorCode::InvalidArgument,
                        "project package database must not have hard links"};
    }
    const fs::path resolvedPackage = fs::canonical(packagePath, ec);
    if (ec) return filesystemError("resolve package", ec);
    const fs::path resolvedDatabase = fs::canonical(candidate, ec);
    if (ec) return filesystemError("resolve database", ec);
    if (!isPathInside(resolvedPackage, resolvedDatabase)) {
        return AppError{ErrorCode::InvalidArgument,
                        "project package database resolves outside the package"};
    }
    return resolvedDatabase;
}

Result<ValidatedDatabase> openValidatedDatabase(const fs::path& packagePath) {
    JsonProjectStore manifests;
    auto loaded = manifests.load(packagePath);
    if (!loaded.hasValue()) return loaded.error();
    auto databasePath = validatedRelativeDatabasePath(loaded.value().database);
    if (!databasePath.hasValue()) return databasePath.error();
    auto existingDatabase = validatedExistingDatabase(packagePath, databasePath.value());
    if (!existingDatabase.hasValue()) return existingDatabase.error();
    auto database = SqliteProjectDatabase::open(existingDatabase.value(),
                                                loaded.value().projectId);
    if (!database.hasValue()) return database.error();
    return ValidatedDatabase{
        .package = ProjectPackage{.path = packagePath, .manifest = loaded.value()},
        .database = std::move(database).value(),
    };
}

fs::path stagingPathFor(const fs::path& target) {
    fs::path staging = target;
    staging += ".creating-";
    staging += core::generateUuidV4();
    return staging;
}

fs::path resolvedParent(const fs::path& path, std::error_code& error) {
    const fs::path parent = path.parent_path().empty() ? fs::path{"."} : path.parent_path();
    return fs::weakly_canonical(parent, error);
}

void removeGeneratedStaging(const fs::path& staging, const fs::path& target) noexcept {
    std::error_code stagingError;
    std::error_code targetError;
    const fs::path stagingParent = resolvedParent(staging, stagingError);
    const fs::path targetParent = resolvedParent(target, targetError);
    if (stagingError || targetError || stagingParent != targetParent) return;

    fs::path expectedPrefix = target.filename();
    expectedPrefix += ".creating-";
    const auto& actual = staging.filename().native();
    const auto& prefix = expectedPrefix.native();
    if (actual.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), actual.begin())) {
        return;
    }
    std::error_code ignored;
    fs::remove_all(staging, ignored);
}

Result<std::string> safeRecoveryComponent(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    constexpr std::size_t maximumBytes = 128;
    std::string encoded;
    for (const unsigned char byte : value) {
        const bool portable =
            (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
        if (portable) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[byte >> 4U]);
            encoded.push_back(hex[byte & 0x0FU]);
        }
        if (encoded.size() > maximumBytes) {
            return AppError{ErrorCode::InvalidArgument,
                            "recovery session identifier is too long for quarantine"};
        }
    }
    return encoded;
}

Result<std::optional<fs::file_status>> inspectedStatus(const fs::path& path) {
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        status.type() == fs::file_type::not_found) {
        return std::optional<fs::file_status>{};
    }
    if (error) return filesystemError("inspect recovery path", error);
    return std::optional<fs::file_status>{status};
}

Result<void> rejectReparsePoint(const fs::path& path) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return filesystemError(
            "inspect recovery attributes",
            std::error_code{static_cast<int>(GetLastError()), std::system_category()});
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "recovery paths must not contain reparse points"};
    }
#else
    static_cast<void>(path);
#endif
    return core::ok();
}

Result<void> ensureSafeDirectory(const fs::path& packagePath,
                                 const fs::path& relativeDirectory) {
    fs::path current = packagePath;
    for (const auto& component : relativeDirectory) {
        current /= component;
        auto inspected = inspectedStatus(current);
        if (!inspected.hasValue()) return inspected.error();
        if (!inspected.value()) {
            std::error_code error;
            if (!fs::create_directory(current, error) && error) {
                return filesystemError("create recovery directory", error);
            }
            inspected = inspectedStatus(current);
            if (!inspected.hasValue()) return inspected.error();
        }
        if (!inspected.value() || fs::is_symlink(*inspected.value()) ||
            !fs::is_directory(*inspected.value())) {
            return AppError{ErrorCode::InvalidArgument,
                            "recovery directory path is not a plain directory"};
        }
        if (auto reparse = rejectReparsePoint(current); !reparse.hasValue()) {
            return reparse.error();
        }
    }
    return core::ok();
}

Result<std::optional<fs::path>> validatedPlainFile(
    const fs::path& packagePath, const fs::path& relativePath) {
    fs::path current = packagePath;
    for (auto component = relativePath.begin(); component != relativePath.end();
         ++component) {
        current /= *component;
        auto inspected = inspectedStatus(current);
        if (!inspected.hasValue()) return inspected.error();
        if (!inspected.value()) return std::optional<fs::path>{};
        if (fs::is_symlink(*inspected.value())) {
            return AppError{ErrorCode::InvalidArgument,
                            "recovery paths must not contain symbolic links"};
        }
        if (auto reparse = rejectReparsePoint(current); !reparse.hasValue()) {
            return reparse.error();
        }
        const bool last = std::next(component) == relativePath.end();
        if (!last && !fs::is_directory(*inspected.value())) {
            return AppError{ErrorCode::InvalidArgument,
                            "recovery parent path is not a directory"};
        }
        if (last && !fs::is_regular_file(*inspected.value())) {
            return AppError{ErrorCode::InvalidArgument,
                            "recovery part path is not a regular file"};
        }
    }
    std::error_code error;
    const auto links = fs::hard_link_count(current, error);
    if (error) return filesystemError("inspect recovery file links", error);
    if (links != 1) {
        return AppError{ErrorCode::InvalidArgument,
                        "recovery part files must not have hard links"};
    }
    const auto resolvedPackage = fs::canonical(packagePath, error);
    if (error) return filesystemError("resolve recovery package", error);
    const auto resolvedFile = fs::canonical(current, error);
    if (error) return filesystemError("resolve recovery file", error);
    if (!isPathInside(resolvedPackage, resolvedFile)) {
        return AppError{ErrorCode::InvalidArgument,
                        "recovery part file resolves outside the package"};
    }
    return std::optional<fs::path>{current};
}

Result<void> moveWithoutOverwrite(const fs::path& source,
                                  const fs::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            return AppError{ErrorCode::AlreadyExists,
                            "recovery quarantine destination already exists"};
        }
        return filesystemError(
            "move part to quarantine",
            std::error_code{static_cast<int>(code), std::system_category()});
    }
#elif defined(__APPLE__)
    if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) != 0) {
        if (errno == EEXIST) {
            return AppError{ErrorCode::AlreadyExists,
                            "recovery quarantine destination already exists"};
        }
        return filesystemError("move part to quarantine",
                               std::error_code{errno, std::generic_category()});
    }
#else
    if (::link(source.c_str(), destination.c_str()) != 0) {
        if (errno == EEXIST) {
            return AppError{ErrorCode::AlreadyExists,
                            "recovery quarantine destination already exists"};
        }
        return filesystemError("link part into quarantine",
                               std::error_code{errno, std::generic_category()});
    }
    if (::unlink(source.c_str()) != 0) {
        const int sourceError = errno;
        // The destination was created by the successful link above. Remove it
        // on failure so a retry does not see a two-link file and become stuck.
        static_cast<void>(::unlink(destination.c_str()));
        return filesystemError(
            "remove quarantined part source",
            std::error_code{sourceError, std::generic_category()});
    }
#endif
    return core::ok();
}

Result<bool> quarantinePart(const fs::path& packagePath,
                            const fs::path& sourceRelative,
                            const fs::path& destinationRelative) {
    auto source = validatedPlainFile(packagePath, sourceRelative);
    if (!source.hasValue()) return source.error();
    auto destination = validatedPlainFile(packagePath, destinationRelative);
    if (!destination.hasValue()) return destination.error();
    if (!source.value()) {
        // Either no encoder created the part, or a prior recovery already moved it.
        return false;
    }
    if (destination.value()) {
        return AppError{ErrorCode::AlreadyExists,
                        "recovery quarantine destination already exists"};
    }
    if (auto directories =
            ensureSafeDirectory(packagePath, destinationRelative.parent_path());
        !directories.hasValue()) {
        return directories.error();
    }
    if (auto moved = moveWithoutOverwrite(*source.value(),
                                          packagePath / destinationRelative);
        !moved.hasValue()) {
        return moved.error();
    }
    return true;
}

Result<std::vector<fs::path>> orphanPartCandidates(const fs::path& packagePath) {
    const fs::path temporaryRelative{".tmp"};
    const fs::path temporaryRoot = packagePath / temporaryRelative;
    auto rootStatus = inspectedStatus(temporaryRoot);
    if (!rootStatus.hasValue()) return rootStatus.error();
    if (!rootStatus.value()) return std::vector<fs::path>{};
    if (fs::is_symlink(*rootStatus.value()) ||
        !fs::is_directory(*rootStatus.value())) {
        return AppError{ErrorCode::InvalidArgument,
                        "recovery temporary root is not a plain directory"};
    }
    if (auto reparse = rejectReparsePoint(temporaryRoot); !reparse.hasValue()) {
        return reparse.error();
    }

    std::vector<fs::path> result;
    std::error_code error;
    for (fs::recursive_directory_iterator iterator{temporaryRoot, error};
         !error && iterator != fs::recursive_directory_iterator{};
         iterator.increment(error)) {
        const auto status = iterator->symlink_status(error);
        if (error) break;
        if (fs::is_symlink(status)) {
            return AppError{ErrorCode::InvalidArgument,
                            "recovery temporary tree contains a symbolic link"};
        }
        if (auto reparse = rejectReparsePoint(iterator->path());
            !reparse.hasValue()) {
            return reparse.error();
        }
        if (fs::is_directory(status)) continue;
        if (!fs::is_regular_file(status)) {
            return AppError{ErrorCode::InvalidArgument,
                            "recovery temporary tree contains a non-regular entry"};
        }
        if (iterator->path().extension() != fs::path{".part"}) continue;
        const auto withinTemporary = iterator->path().lexically_relative(temporaryRoot);
        if (withinTemporary.empty()) {
            return AppError{ErrorCode::IoFailure,
                            "recovery could not relativize an orphan part"};
        }
        result.push_back(temporaryRelative / withinTemporary);
    }
    if (error) return filesystemError("scan temporary recovery files", error);
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace

Result<OpenProjectResult> ProjectPackageStore::create(const fs::path& packagePath,
                                                      const std::string& name) {
    if (packagePath.filename().empty()) {
        return AppError{ErrorCode::InvalidArgument, "project package path needs a filename"};
    }
    std::error_code ec;
    if (fs::exists(packagePath, ec)) {
        return AppError{ErrorCode::AlreadyExists, "project package target already exists"};
    }
    if (ec) return filesystemError("inspect target", ec);

    const fs::path staging = stagingPathFor(packagePath);
    if (fs::exists(staging, ec)) {
        return AppError{ErrorCode::AlreadyExists, "project package staging path already exists"};
    }
    if (ec) return filesystemError("inspect staging", ec);

    JsonProjectStore manifests;
    auto manifest = manifests.create(staging, name);
    if (!manifest.hasValue()) {
        removeGeneratedStaging(staging, packagePath);
        return manifest.error();
    }
    {
        auto database = SqliteProjectDatabase::create(
            staging / manifest.value().database, manifest.value());
        if (!database.hasValue()) {
            removeGeneratedStaging(staging, packagePath);
            return database.error();
        }
    }
    fs::rename(staging, packagePath, ec);
    if (ec) {
        removeGeneratedStaging(staging, packagePath);
        return filesystemError("publish staging directory", ec);
    }
    return open(packagePath);
}

Result<OpenProjectResult> ProjectPackageStore::open(const fs::path& packagePath) {
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    auto candidates = opened.value().database.scanRecovery(
        packagePath, opened.value().package.manifest.name);
    if (!candidates.hasValue()) return candidates.error();
    return OpenProjectResult{.package = std::move(opened.value().package),
                             .recoveryCandidates = std::move(candidates).value()};
}

Result<void> ProjectPackageStore::beginRecording(const fs::path& packagePath,
                                                 const domain::SessionId& sessionId,
                                                 core::TimestampNs startedAt,
                                                 const core::Utc& createdAt) {
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    return opened.value().database.beginRecording(sessionId, startedAt, createdAt);
}

Result<void> ProjectPackageStore::completeRecording(
    const fs::path& packagePath, const domain::RecordingSession& session,
    const core::Utc& finishedAt) {
    if (session.state() != domain::SessionState::Stopped || !session.stoppedAt().has_value()) {
        return AppError{ErrorCode::InvalidState,
                        "only a stopped recording session can be persisted as complete"};
    }
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    return opened.value().database.completeRecording(
        session.id(), *session.stoppedAt(), session.segments(), finishedAt);
}

Result<void> ProjectPackageStore::abortRecording(const fs::path& packagePath,
                                                 const domain::SessionId& sessionId,
                                                 std::string_view reason,
                                                 const core::Utc& finishedAt) {
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    return opened.value().database.abortRecording(sessionId, reason, finishedAt);
}

Result<void> ProjectPackageStore::beginSegment(const fs::path& packagePath,
                                               const domain::SessionId& sessionId,
                                               const domain::SegmentInfo& segment) {
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    return opened.value().database.beginSegment(sessionId, segment);
}

Result<void> ProjectPackageStore::markSegmentReady(const fs::path& packagePath,
                                                   const domain::SessionId& sessionId,
                                                   const domain::SegmentInfo& segment) {
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    return opened.value().database.markSegmentReady(sessionId, segment);
}

Result<void> ProjectPackageStore::markSegmentFailed(const fs::path& packagePath,
                                                    const domain::SessionId& sessionId,
                                                    const domain::SourceId& sourceId,
                                                    std::uint64_t segmentIndex) {
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    return opened.value().database.markSegmentFailed(sessionId, sourceId, segmentIndex);
}

Result<RecoveryResult> ProjectPackageStore::recover(const fs::path& packagePath,
                                                    const domain::SessionId& sessionId,
                                                    const core::Utc& finishedAt) {
    auto opened = openValidatedDatabase(packagePath);
    if (!opened.hasValue()) return opened.error();
    auto session = opened.value().database.session(sessionId);
    if (!session.hasValue()) return session.error();
    if (session.value().state != PersistedSessionState::Recording) {
        // Recovered sessions are idempotent, while completed/aborted sessions
        // are rejected by the database. Neither case may mutate temp files.
        return opened.value().database.recover(sessionId, finishedAt);
    }
    auto writing = opened.value().database.writingSegments();
    if (!writing.hasValue()) return writing.error();
    auto sessionComponent = safeRecoveryComponent(sessionId.value());
    if (!sessionComponent.hasValue()) return sessionComponent.error();

    std::set<fs::path> referencedParts;
    std::size_t quarantinedParts = 0;
    for (const auto& segment : writing.value()) {
        auto relative = validatedRelativeDatabasePath(segment.relativePath);
        if (!relative.hasValue()) return relative.error();
        fs::path sourceRelative = fs::path{".tmp"} / relative.value();
        sourceRelative += ".part";
        sourceRelative = sourceRelative.lexically_normal();
        referencedParts.insert(sourceRelative);
        if (segment.sessionId != sessionId) continue;
        fs::path destinationRelative =
            fs::path{"recovery/quarantine"} / sessionComponent.value() /
            relative.value();
        destinationRelative += ".part";
        auto quarantined = quarantinePart(packagePath, sourceRelative,
                                          destinationRelative.lexically_normal());
        if (!quarantined.hasValue()) return quarantined.error();
        if (quarantined.value()) ++quarantinedParts;
    }

    auto orphanCandidates = orphanPartCandidates(packagePath);
    if (!orphanCandidates.hasValue()) return orphanCandidates.error();
    std::size_t orphanParts = 0;
    for (const auto& sourceRelative : orphanCandidates.value()) {
        if (referencedParts.contains(sourceRelative)) continue;
        const auto withinTemporary =
            sourceRelative.lexically_relative(fs::path{".tmp"});
        if (withinTemporary.empty()) {
            return AppError{ErrorCode::IoFailure,
                            "recovery orphan path escaped the temporary root"};
        }
        const fs::path destinationRelative =
            fs::path{"recovery/quarantine/orphans"} / withinTemporary;
        auto quarantined = quarantinePart(packagePath, sourceRelative,
                                          destinationRelative.lexically_normal());
        if (!quarantined.hasValue()) return quarantined.error();
        if (quarantined.value()) ++orphanParts;
    }

    auto recovered = opened.value().database.recover(sessionId, finishedAt);
    if (!recovered.hasValue()) return recovered.error();
    recovered.value().quarantinedParts = quarantinedParts;
    recovered.value().orphanParts = orphanParts;
    return recovered;
}

}  // namespace creator::project_store
