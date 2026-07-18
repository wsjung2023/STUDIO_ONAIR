#include "app/RecordingTimelineReconciler.h"

#include "app/RecordingImportPlanner.h"
#include "app/TimelineEditService.h"
#include "core/AppError.h"
#include "domain/ImportRecordingCommand.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteProjectDatabase.h"
#include "project_store/SqliteStudioStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

std::string hexId(std::string_view value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        result.push_back(digits[character >> 4]);
        result.push_back(digits[character & 0x0f]);
    }
    return result;
}

std::string ffconcatPath(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const auto character : value) {
        if (character == '\\' || character == '\'') result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

std::string seconds(core::DurationNs duration) {
    const auto count = duration.count();
    const auto whole = count / 1'000'000'000;
    const auto fraction = count % 1'000'000'000;
    std::ostringstream output;
    output << whole << '.' << std::setw(9) << std::setfill('0') << fraction;
    return output.str();
}

Result<std::vector<RecordingConcatSource>> buildConcatSources(
    const std::filesystem::path& packageRoot,
    const domain::SessionId& sessionId,
    const std::vector<domain::SegmentInfo>& segments,
    media::IMediaProbe& mediaProbe) {
    std::vector<const domain::SegmentInfo*> ready;
    ready.reserve(segments.size());
    for (const auto& segment : segments) {
        if (segment.status == domain::SegmentStatus::Ready) ready.push_back(&segment);
    }
    std::sort(ready.begin(), ready.end(), [](const auto* first, const auto* second) {
        if (first->sourceId != second->sourceId) return first->sourceId < second->sourceId;
        if (first->startTime != second->startTime) return first->startTime < second->startTime;
        return first->index < second->index;
    });

    std::vector<RecordingConcatSource> result;
    std::size_t run = 0;
    std::size_t cursor = 0;
    while (cursor < ready.size()) {
        const auto sourceId = ready[cursor]->sourceId;
        std::vector<const domain::SegmentInfo*> parts{ready[cursor]};
        ++cursor;
        while (cursor < ready.size() && ready[cursor]->sourceId == sourceId) {
            const auto* previous = parts.back();
            if (ready[cursor]->startTime != previous->startTime + previous->duration) break;
            parts.push_back(ready[cursor]);
            ++cursor;
        }
        if (parts.size() < 2) continue;

        // Keep the manifest at package root: ffconcat resolves `file` entries
        // relative to the list itself, so this lets us reference immutable
        // media/... and audio/... package paths without `..` traversal.
        const auto relativePath = std::filesystem::path(
            "derived-concat-" + hexId(sessionId.value()) + "-" +
            hexId(sourceId.value()) + "-" + std::to_string(run++) +
            ".ffconcat");
        const auto manifestPath = packageRoot / relativePath;
        std::error_code error;
        std::filesystem::create_directories(manifestPath.parent_path(), error);
        if (error) {
            return AppError{ErrorCode::IoFailure,
                            "could not create concat manifest directory: " + error.message()};
        }
        std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            return AppError{ErrorCode::IoFailure,
                            "could not create concat manifest: " + manifestPath.string()};
        }
        output << "ffconcat version 1.0\n";
        RecordingConcatSource source{.sourceId = sourceId,
                                     .relativePath = relativePath.generic_string(),
                                     .media = {},
                                     .entries = {}};
        core::DurationNs offset{};
        for (const auto* part : parts) {
            output << "file '" << ffconcatPath(part->relativePath) << "'\n";
            output << "duration " << seconds(part->duration) << "\n";
            source.entries.push_back(RecordingConcatEntry{
                .segmentPath = part->relativePath, .offset = offset});
            offset += part->duration;
        }
        output.flush();
        if (!output) {
            return AppError{ErrorCode::IoFailure,
                            "could not write concat manifest: " + manifestPath.string()};
        }
        // Windows media identity probing opens the file with exclusive
        // metadata sharing. Release the writer before probing the derived
        // manifest, otherwise reconciliation races its own ofstream and
        // reports "media file could not be locked for inspection".
        output.close();
        auto probe = mediaProbe.probe(packageRoot, source.relativePath);
        if (!probe.hasValue()) return probe.error();
        source.media = std::move(probe).value();
        result.push_back(std::move(source));
    }
    return result;
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
    std::vector<domain::SegmentInfo> normalizedSegments;
    normalizedSegments.reserve(segments.value().size());
    for (const auto& segment : segments.value()) {
        if (segment.startTime < session.value().startedAt) {
            return AppError{ErrorCode::InvalidArgument,
                            "recording segment starts before its session"};
        }
        auto normalized = segment;
        normalized.startTime = core::TimestampNs{
            segment.startTime - session.value().startedAt};
        normalizedSegments.push_back(std::move(normalized));
    }
    auto sources = studio.loadRecordingSources(sessionId);
    if (!sources.hasValue()) return sources.error();
    auto events = studio.loadRecordingSceneEvents(sessionId);
    if (!events.hasValue()) return events.error();
    auto markers = studio.loadRecordingMarkers(sessionId);
    if (!markers.hasValue()) return markers.error();
    auto concatSources = buildConcatSources(validatedPackageRoot, sessionId,
                                            normalizedSegments, *mediaProbe_);
    if (!concatSources.hasValue()) return concatSources.error();
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
        .segments = std::move(normalizedSegments),
        .sources = sources.value(),
        .scenes = snapshot.value().scenes,
        .sceneEvents = events.value(),
        .markers = markers.value(),
        .timeline = editService.snapshot(),
        .probes = probes,
        .concatSources = concatSources.value()});
    if (!plan.hasValue()) return plan.error();

    // Probing every segment a second time makes a long recording scale with
    // two full FFmpeg parses and hashes per segment.  Each first-pass probe
    // retains an identity lease; verify those leases immediately before the
    // transaction instead, which detects replacement/truncation without
    // reopening thousands of containers.
    std::vector<media::MediaProbeResult> revalidatedProbes;
    revalidatedProbes.reserve(probes.size());
    for (const auto& expected : probes) {
        if (!expected.media.identityLease) {
            return AppError{ErrorCode::InvalidState,
                            "media probe did not retain an identity lease"};
        }
        auto identity = expected.media.identityLease->verifyCurrentIdentity();
        if (!identity.hasValue()) return identity.error();
        revalidatedProbes.push_back(expected.media);
    }
    for (const auto& concat : concatSources.value()) {
        if (!concat.media.identityLease) {
            return AppError{ErrorCode::InvalidState,
                            "concat media probe did not retain an identity lease"};
        }
        auto identity = concat.media.identityLease->verifyCurrentIdentity();
        if (!identity.hasValue()) return identity.error();
        revalidatedProbes.push_back(concat.media);
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
