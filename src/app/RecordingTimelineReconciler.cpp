#include "app/RecordingTimelineReconciler.h"

#include "app/RecordingImportPlanner.h"
#include "app/TimelineEditService.h"
#include "core/AppError.h"
#include "domain/ImportRecordingCommand.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteProjectDatabase.h"
#include "project_store/SqliteStudioStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace creator::app {
namespace {

using core::AppError;
using core::ErrorCode;
using core::Result;

RecordingReconcileResult existingResult(
    const domain::SessionId& sessionId,
    const project_store::RecordingImportRecord& record) {
    return RecordingReconcileResult{.sessionId = sessionId,
                                    .imported = false,
                                    .revision = record.importedRevision,
                                    .assetCount = 0,
                                    .trackCount = 0,
                                    .markerCount = 0};
}

}  // namespace

Result<RecordingReconcileResult> RecordingTimelineReconciler::reconcile(
    const std::filesystem::path& packageRoot,
    const domain::SessionId& sessionId) {
    if (mediaProbe_ == nullptr || !eventIdFactory_ || !clock_ ||
        historyLimit_ == 0) {
        return AppError{ErrorCode::InvalidArgument,
                        "recording reconciler dependencies are invalid"};
    }
    project_store::ProjectPackageStore packages;
    auto openedPackage = packages.open(packageRoot);
    if (!openedPackage.hasValue()) return openedPackage.error();
    const auto& manifest = openedPackage.value().package.manifest;
    const auto& validatedPackageRoot = openedPackage.value().package.path;
    const auto& databasePath = openedPackage.value().databasePath;
    const auto& databaseIdentity =
        openedPackage.value().databaseIdentityLease;
    if (!databaseIdentity) {
        return AppError{ErrorCode::IoFailure,
                        "validated project database identity is missing"};
    }
    auto verifyDatabaseIdentity = [databaseIdentity] {
        return databaseIdentity->verifyCurrentIdentity();
    };

    auto studioResult = project_store::SqliteStudioStore::open(
        databasePath, manifest.projectId, verifyDatabaseIdentity);
    if (!studioResult.hasValue()) return studioResult.error();
    if (auto identity = databaseIdentity->verifyCurrentIdentity();
        !identity.hasValue()) {
        return identity.error();
    }
    auto studio = std::move(studioResult).value();
    auto existing = studio.recordingImport(sessionId);
    if (!existing.hasValue()) return existing.error();
    if (existing.value().has_value()) {
        return existingResult(sessionId, *existing.value());
    }

    auto databaseResult = project_store::SqliteProjectDatabase::open(
        databasePath, manifest.projectId, verifyDatabaseIdentity);
    if (!databaseResult.hasValue()) return databaseResult.error();
    if (auto identity = databaseIdentity->verifyCurrentIdentity();
        !identity.hasValue()) {
        return identity.error();
    }
    auto database = std::move(databaseResult).value();
    auto session = database.session(sessionId);
    if (!session.hasValue()) return session.error();
    if (session.value().state != project_store::PersistedSessionState::Completed &&
        session.value().state != project_store::PersistedSessionState::Recovered) {
        return AppError{ErrorCode::InvalidState,
                        "recording session is not ready to reconcile"};
    }
    auto segments = database.segments(sessionId);
    if (!segments.hasValue()) return segments.error();
    auto sources = studio.loadRecordingSources(sessionId);
    if (!sources.hasValue()) return sources.error();
    auto events = studio.loadRecordingSceneEvents(sessionId);
    if (!events.hasValue()) return events.error();
    auto markers = studio.loadRecordingMarkers(sessionId);
    if (!markers.hasValue()) return markers.error();
    auto snapshot = studio.load();
    if (!snapshot.hasValue()) return snapshot.error();

    auto timelineStoreResult = project_store::SqliteTimelineStore::open(
        databasePath, manifest.projectId, verifyDatabaseIdentity);
    if (!timelineStoreResult.hasValue()) return timelineStoreResult.error();
    if (auto identity = databaseIdentity->verifyCurrentIdentity();
        !identity.hasValue()) {
        return identity.error();
    }
    auto timelineStore = std::move(timelineStoreResult).value();
    auto editServiceResult = TimelineEditService::open(
        timelineStore, historyLimit_, eventIdFactory_, clock_);
    if (!editServiceResult.hasValue()) return editServiceResult.error();
    auto editService = std::move(editServiceResult).value();

    std::vector<RecordingSegmentProbe> probes;
    for (const auto& segment : segments.value()) {
        if (segment.status != domain::SegmentStatus::Ready) continue;
        auto probe =
            mediaProbe_->probe(validatedPackageRoot, segment.relativePath);
        if (!probe.hasValue()) return probe.error();
        if (!probe.value().identityLease) {
            return AppError{ErrorCode::InvalidState,
                            "media probe did not retain an identity lease"};
        }
        probes.push_back(RecordingSegmentProbe{
            .relativePath = segment.relativePath,
            .media = std::move(probe).value()});
    }
    auto plan = planRecordingImport(RecordingImportRequest{
        .sessionId = sessionId,
        .segments = segments.value(),
        .sources = sources.value(),
        .scenes = snapshot.value().scenes,
        .sceneEvents = events.value(),
        .markers = markers.value(),
        .timeline = editService.snapshot(),
        .probes = probes});
    if (!plan.hasValue()) return plan.error();

    std::vector<media::MediaProbeResult> revalidatedProbes;
    revalidatedProbes.reserve(probes.size());
    for (const auto& expected : probes) {
        auto revalidated =
            mediaProbe_->probe(validatedPackageRoot, expected.relativePath);
        if (!revalidated.hasValue()) return revalidated.error();
        if (revalidated.value() != expected.media) {
            return AppError{ErrorCode::IoFailure,
                            "recording media changed before import commit"};
        }
        if (!revalidated.value().identityLease) {
            return AppError{ErrorCode::InvalidState,
                            "media revalidation did not retain an identity lease"};
        }
        auto identity =
            revalidated.value().identityLease->verifyCurrentIdentity();
        if (!identity.hasValue()) return identity.error();
        revalidatedProbes.push_back(std::move(revalidated).value());
    }

    auto command = domain::ImportRecordingCommand::create(
        domain::CommandId::create(
            "recording/" + sessionId.value() + "/import-command")
            .value(),
        plan.value().tracks, plan.value().markers);
    if (!command.hasValue()) return command.error();
    if (editService.revision() == std::numeric_limits<std::int64_t>::max()) {
        return AppError{ErrorCode::InvalidArgument,
                        "timeline revision exceeds import range"};
    }
    const project_store::RecordingImportRecord record{
        .sessionId = sessionId,
        .timelineId = editService.snapshot().id(),
        .base = plan.value().appendBase,
        .importedRevision = editService.revision() + 1,
        .importedAt = clock_()};
    std::vector<std::shared_ptr<const media::IMediaIdentityLease>>
        mediaIdentities;
    mediaIdentities.reserve(revalidatedProbes.size());
    for (const auto& probe : revalidatedProbes) {
        mediaIdentities.push_back(probe.identityLease);
    }
    auto validateResources =
        [databaseIdentity, mediaIdentities = std::move(mediaIdentities)] {
            auto database = databaseIdentity->verifyCurrentIdentity();
            if (!database.hasValue()) return database;
            for (const auto& identity : mediaIdentities) {
                auto media = identity->verifyCurrentIdentity();
                if (!media.hasValue()) return media;
            }
            return core::ok();
        };
    auto committed = editService.executeRecordingImport(
        std::move(command).value(), plan.value().assets, record,
        std::move(validateResources));
    if (!committed.hasValue()) {
        auto raced = studio.recordingImport(sessionId);
        if (raced.hasValue() && raced.value().has_value()) {
            return existingResult(sessionId, *raced.value());
        }
        return committed.error();
    }
    return RecordingReconcileResult{
        .sessionId = sessionId,
        .imported = true,
        .revision = editService.revision(),
        .assetCount = plan.value().assets.size(),
        .trackCount = plan.value().tracks.size(),
        .markerCount = plan.value().markers.size()};
}

}  // namespace creator::app
