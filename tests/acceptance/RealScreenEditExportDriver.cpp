// Headless driver: take a REAL recorded .cstudio package (recorded by the app's
// real screen-capture + record path), reconcile it into an editor timeline
// through RecordingTimelineReconciler / RecordingImportPlanner, apply a real cut
// (Split + ripple DeleteRange) through the edit engine, and export the edited
// timeline to a real H.264/AAC MP4 through ProjectExportEngine.
//
// Unlike OneToolEndToEndAcceptanceTest (which records SYNTHETIC solid-color
// frames), this driver consumes a package whose screen segments are the ACTUAL
// desktop captured by gdigrab -- so the exported MP4 shows the real screen.
//
// Usage:
//   RealScreenEditExportDriver <package.cstudio> <sessionId> <output.mp4>

#include "app/EditorSessionWorker.h"
#include "app/ProjectExportEngine.h"
#include "app/RecordingTimelineReconciler.h"
#include "core/Utc.h"
#include "domain/Timeline.h"
#include "edit_engine/EditEngineTypes.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "project_store/ProjectPackageStore.h"

#include <QGuiApplication>
#include <QObject>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using creator::app::EditorSessionResultPtr;
using creator::app::EditorSessionState;
using creator::app::EditorSessionWorker;
using creator::app::ProjectExportEngine;
using creator::app::RecordingTimelineReconciler;
using creator::core::DurationNs;
using creator::core::TimestampNs;

TimestampNs at(std::int64_t ns) { return TimestampNs{DurationNs{ns}}; }

std::size_t totalClipCount(const creator::domain::Timeline& timeline) {
    std::size_t count{};
    for (const auto& track : timeline.tracks()) count += track.clips().size();
    return count;
}

std::int64_t timelineDurationNs(const creator::domain::Timeline& timeline) {
    std::int64_t end{};
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            end = std::max(
                end, clip.timelineRange().end().time_since_epoch().count());
        }
    }
    return end;
}

std::optional<EditorSessionState> loadEditorState(const fs::path& packageRoot) {
    EditorSessionWorker worker;
    EditorSessionResultPtr result;
    QObject::connect(
        &worker, &EditorSessionWorker::opened, &worker,
        [&](quint64, EditorSessionResultPtr value) { result = std::move(value); },
        Qt::DirectConnection);
    worker.openProject(1, packageRoot);
    if (!result || !result->hasValue()) return std::nullopt;
    return result->value().state;
}

int fail(const std::string& step, const std::string& why) {
    std::cout << "[ REAL-SCREEN FAIL ] step=" << step << " reason=" << why << '\n';
    return 3;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    if (argc < 4) {
        std::cout << "usage: RealScreenEditExportDriver <package.cstudio> "
                     "<sessionId> <output.mp4>\n";
        return 2;
    }
    const fs::path packagePath = fs::path{argv[1]};
    const auto sessionId =
        creator::domain::SessionId::create(std::string{argv[2]});
    if (!sessionId.hasValue()) return fail("args", "bad session id");
    const fs::path destination = fs::path{argv[3]};

    // ---- projectId from the package manifest --------------------------------
    creator::project_store::ProjectPackageStore packageStore;
    auto opened = packageStore.open(packagePath);
    if (!opened.hasValue()) return fail("open-package", opened.error().message());
    const auto projectId = opened.value().package.manifest.projectId;
    opened.value().databaseIdentityLease.reset();  // release before reconcile

    // ---- STEP 1: RECORD -> EDITOR (reconcile recorded segments) -------------
    // The app finalizes a take by reconciling it into the editor timeline; if
    // that already ran, reconcile reports imported=false but the timeline is
    // populated. Either way this exercises RecordingImportPlanner on the REAL
    // recorded segments.
    creator::ffmpeg_adapter::FfmpegMediaProbe mediaProbe;
    std::uint64_t seq{};
    RecordingTimelineReconciler reconciler{
        mediaProbe, [&seq] { return "rse-event-" + std::to_string(++seq); },
        [] { return creator::core::Utc::parseRfc3339("2026-07-21T02:00:00Z").value(); }};
    auto imported = reconciler.reconcile(packagePath, sessionId.value());
    if (!imported.hasValue()) return fail("reconcile", imported.error().message());
    std::cout << "[ REAL-SCREEN PROOF ] step1 reconcile: imported="
              << (imported.value().imported ? "true(new)" : "false(existing)")
              << " trackCount=" << imported.value().trackCount << '\n';

    auto editorState = loadEditorState(packagePath);
    if (!editorState.has_value()) return fail("load-editor", "no editor state");
    const auto& recordedTimeline = editorState->snapshot.timeline;
    const std::size_t recordedClips = totalClipCount(recordedTimeline);
    const std::int64_t rawTakeNs = timelineDurationNs(recordedTimeline);
    std::cout << "[ REAL-SCREEN PROOF ] step1 editor: tracks="
              << recordedTimeline.tracks().size() << " clips=" << recordedClips
              << " assets=" << editorState->assets.size()
              << " raw_take_ns=" << rawTakeNs << '\n';
    if (recordedClips == 0) return fail("load-editor", "no clips on timeline");

    // ---- pick the longest clip on a SCREEN video track to cut inside of ----
    creator::domain::TrackId cutTrack = recordedTimeline.tracks().front().id();
    creator::domain::ClipId cutClip = creator::domain::ClipId::create("x").value();
    std::int64_t bestStart = 0, bestSpan = -1;
    bool foundScreen = false;
    for (const auto& track : recordedTimeline.tracks()) {
        if (track.kind() != creator::domain::TrackKind::Video) continue;
        const bool isScreen =
            track.id().value().find("/track/screen/") != std::string::npos;
        for (const auto& clip : track.clips()) {
            const auto s = clip.timelineRange().start().time_since_epoch().count();
            const auto e = clip.timelineRange().end().time_since_epoch().count();
            // Prefer screen clips; only fall back to other video if no screen.
            const bool better = (isScreen && !foundScreen) ||
                                ((isScreen == foundScreen) && (e - s > bestSpan));
            if (better) {
                foundScreen = foundScreen || isScreen;
                bestSpan = e - s;
                bestStart = s;
                cutTrack = track.id();
                cutClip = clip.id();
            }
        }
    }
    std::cout << "[ REAL-SCREEN PROOF ] step-pick cut track=" << cutTrack.value()
              << " screen=" << (foundScreen ? "yes" : "no")
              << " span_ns=" << bestSpan << '\n';
    if (bestSpan < 400'000'000)
        return fail("pick-clip", "no video clip long enough to cut");

    const std::int64_t splitAt = bestStart + bestSpan / 2;
    const std::int64_t delStart = splitAt + bestSpan / 8;
    const std::int64_t delSpan = bestSpan / 8;  // strictly inside [splitAt, end]
    const auto deleteRange =
        creator::domain::TimeRange::create(at(delStart), DurationNs{delSpan}).value();

    // ---- STEP 2: CUT (Split + ripple DeleteRange) through the edit engine ---
    EditorSessionWorker editWorker;
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(
        &editWorker, &EditorSessionWorker::opened, &editWorker,
        [&](quint64, EditorSessionResultPtr v) { openedResult = std::move(v); },
        Qt::DirectConnection);
    QObject::connect(
        &editWorker, &EditorSessionWorker::edited, &editWorker,
        [&](quint64, quint64, EditorSessionResultPtr v) { editedResult = std::move(v); },
        Qt::DirectConnection);
    editWorker.openProject(3, packagePath);
    if (!openedResult || !openedResult->hasValue())
        return fail("edit-open",
                    openedResult ? openedResult->error().message() : "no result");
    const std::size_t beforeClips =
        totalClipCount(openedResult->value().state.snapshot.timeline);
    const std::int64_t beforeDuration =
        timelineDurationNs(openedResult->value().state.snapshot.timeline);

    editedResult.reset();
    editWorker.edit(3, 1,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Split,
                        .trackId = cutTrack,
                        .clipId = cutClip,
                        .position = at(splitAt)});
    if (!editedResult || !editedResult->hasValue())
        return fail("split", editedResult ? editedResult->error().message() : "no result");
    const std::size_t afterSplitClips =
        totalClipCount(editedResult->value().state.snapshot.timeline);

    editedResult.reset();
    editWorker.edit(3, 2,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::DeleteRange,
                        .range = deleteRange,
                        .ripple = true});
    if (!editedResult || !editedResult->hasValue())
        return fail("ripple-delete",
                    editedResult ? editedResult->error().message() : "no result");
    const std::int64_t afterDuration =
        timelineDurationNs(editedResult->value().state.snapshot.timeline);
    if (afterDuration >= beforeDuration)
        return fail("ripple-delete", "timeline did not shorten");
    std::cout << "[ REAL-SCREEN PROOF ] step2 cut: clips " << beforeClips << " -> "
              << afterSplitClips << " (split) ; duration_ns " << beforeDuration
              << " -> " << afterDuration << " (ripple -"
              << (beforeDuration - afterDuration) << ")\n";

    // Durability: a completely fresh reopen must still show the cut.
    auto durable = loadEditorState(packagePath);
    if (!durable.has_value()) return fail("durable", "reopen failed");
    if (timelineDurationNs(durable->snapshot.timeline) != afterDuration)
        return fail("durable", "cut did not survive reopen");

    // ---- STEP 3: EXPORT the edited timeline to a real H.264/AAC MP4 ---------
    ProjectExportEngine exportEngine{fs::path{CS_DRIVER_MLT_ROOT}};
    std::error_code mkdirError;
    fs::create_directories(destination.parent_path(), mkdirError);
    auto request = creator::edit_engine::RenderRequest::create(
        projectId, durable->snapshot, destination,
        creator::edit_engine::RenderPreset::h2641080p30().value(),
        creator::edit_engine::RenderOverwritePolicy::ReplaceExisting);
    if (!request.hasValue()) return fail("render-request", request.error().message());
    auto job = exportEngine.render(request.value());
    if (!job.hasValue()) return fail("render-start", job.error().message());
    creator::edit_engine::RenderJobState state =
        creator::edit_engine::RenderJobState::Pending;
    const auto deadline = std::chrono::steady_clock::now() + 180s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto progress = job.value()->progress();
        if (!progress.hasValue()) return fail("render-progress", progress.error().message());
        state = progress.value().state();
        if (state == creator::edit_engine::RenderJobState::Completed ||
            state == creator::edit_engine::RenderJobState::Failed ||
            state == creator::edit_engine::RenderJobState::Cancelled)
            break;
        std::this_thread::sleep_for(20ms);
    }
    if (state != creator::edit_engine::RenderJobState::Completed)
        return fail("render", job.value()->diagnostic());
    if (!fs::is_regular_file(destination))
        return fail("render", "output file missing");

    auto media = mediaProbe.probe(destination.parent_path(), destination.filename());
    if (!media.hasValue()) return fail("probe-output", media.error().message());
    std::error_code sizeError;
    const auto bytes = fs::file_size(destination, sizeError);
    std::cout << "[ REAL-SCREEN PROOF ] step3 export: " << destination.string()
              << " codec=" << media.value().codecName;
    if (media.value().video.has_value())
        std::cout << " " << media.value().video->width << "x"
                  << media.value().video->height;
    if (media.value().audio.has_value())
        std::cout << " audio=" << media.value().audio->sampleRate << "/"
                  << media.value().audio->channels;
    std::cout << " bytes=" << bytes
              << " duration_ns=" << media.value().duration.count() << '\n';
    std::cout << "[ REAL-SCREEN PROOF ] SUMMARY raw_take_ns=" << rawTakeNs
              << " post_cut_ns=" << afterDuration
              << " exported_ns=" << media.value().duration.count()
              << " output=" << destination.string() << '\n';
    return 0;
}
