#include "project_store/ProjectPackageStore.h"

#include "core/AppError.h"
#include "core/Uuid.h"
#include "project_store/JsonProjectStore.h"
#include "project_store/SqliteProjectDatabase.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

Result<ValidatedDatabase> openValidatedDatabase(const fs::path& packagePath) {
    JsonProjectStore manifests;
    auto loaded = manifests.load(packagePath);
    if (!loaded.hasValue()) return loaded.error();
    auto databasePath = validatedRelativeDatabasePath(loaded.value().database);
    if (!databasePath.hasValue()) return databasePath.error();
    auto database = SqliteProjectDatabase::open(packagePath / databasePath.value(),
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
    return opened.value().database.recover(sessionId, finishedAt);
}

}  // namespace creator::project_store
