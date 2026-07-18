#include "app/DeviceCaptureController.h"
#include "app/EditorController.h"
#include "app/ExportController.h"
#include "app/LiveRecordingController.h"
#include "app/LiveRecordingEngineFactory.h"
#include "app/ProjectController.h"
#include "app/ProjectEditorBinding.h"
#include "app/ProjectExportEngine.h"
#include "app/RecordingTimelineReconciler.h"
#include "app/ScreenCaptureController.h"
#include "app/StudioRecordingBinding.h"
#include "app/StudioWorkflowController.h"
#include "core/Uuid.h"
#include "core/Utc.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "ffmpeg_adapter/windows/WindowsCaptureBackend.h"
#include "mlt_adapter/MltEditEngine.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QUrl>
#include <QVariantMap>

#include <gtest/gtest.h>

#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using creator::app::DeviceCaptureController;
using creator::app::EditorController;
using creator::app::ExportController;
using creator::app::LiveRecordingController;
using creator::app::ScreenCaptureController;
using creator::app::ScreenCaptureState;
using creator::project_store::ProjectPackageStore;

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout);

QVariantList timelineRows(EditorController& controller) {
    QVariantList rows;
    auto* model = controller.timelineTrackModel();
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        rows.push_back(QVariantMap{
            {QStringLiteral("trackId"),
             model->data(index, creator::app::TimelineTrackModel::TrackIdRole)},
            {QStringLiteral("name"),
             model->data(index, creator::app::TimelineTrackModel::NameRole)},
            {QStringLiteral("kind"),
             model->data(index, creator::app::TimelineTrackModel::KindRole)},
            {QStringLiteral("clips"),
             model->data(index, creator::app::TimelineTrackModel::ClipsRole)}});
    }
    return rows;
}

std::pair<QString, QString> firstClipForTrack(
    const QVariantList& rows, const QString& name) {
    for (const auto& value : rows) {
        const auto row = value.toMap();
        if (row.value(QStringLiteral("name")).toString() != name) continue;
        const auto clips = row.value(QStringLiteral("clips")).toList();
        if (clips.empty()) return {};
        return {row.value(QStringLiteral("trackId")).toString(),
                clips.front().toMap().value(QStringLiteral("id")).toString()};
    }
    return {};
}

bool waitRevision(EditorController& controller, qlonglong revision,
                  std::chrono::milliseconds timeout = 60s) {
    return waitUntil(
        [&] {
            return !controller.sessionBusy() && !controller.busy() &&
                   controller.timelineRevision() == revision;
        },
        timeout);
}

void seekAndWait(EditorController& controller, qlonglong position) {
    controller.seek(position);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }, 30s))
        << controller.statusMessage().toStdString();
}

void waitEditorIdle(EditorController& controller) {
    ASSERT_TRUE(waitUntil(
        [&] { return !controller.busy() && !controller.sessionBusy(); }, 60s))
        << controller.statusMessage().toStdString();
}

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        std::this_thread::sleep_for(5ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return predicate();
}

class PhysicalPackage final {
public:
    PhysicalPackage() {
        root_ = fs::temp_directory_path() /
                fs::path{u8"creator-studio-r1-07-물리검증"} /
                std::to_string(QCoreApplication::applicationPid());
        std::error_code error;
        fs::remove_all(root_, error);
        error.clear();
        fs::create_directories(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    ~PhysicalPackage() {
        std::error_code error;
        fs::remove_all(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    const fs::path& root() const noexcept { return root_; }
    fs::path package() const { return root_ / fs::path{u8"30분 강의.cstudio"}; }
    fs::path registry() const { return root_ / "recent-projects.json"; }

private:
    fs::path root_;
};

TEST(R1FinalPhysicalAcceptanceTest,
     RecordsAllPhysicalWindowsSourcesForThirtyWallClockMinutes) {
    PhysicalPackage physical;
    creator::app::ProjectController project{
        std::make_unique<ProjectPackageStore>(), physical.registry(), false};
    QSignalSpy opened{&project, &creator::app::ProjectController::projectOpened};
    project.createProject(QUrl::fromLocalFile(
                              QString::fromStdWString(physical.package().wstring())),
                          QStringLiteral("R1-07 실제 30분 강의"));
    ASSERT_TRUE(opened.wait(15'000)) << project.statusMessage().toStdString();
    ASSERT_TRUE(project.hasOpenProject());

    auto windows =
        creator::ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    DeviceCaptureController devices{std::move(windows.devices)};
    ScreenCaptureController screen{std::move(windows.screenPermission),
                                   std::move(windows.screenDiscovery),
                                   std::move(windows.screenSourceFactory)};
    devices.initialize();
    screen.initialize();
    ASSERT_TRUE(waitUntil(
        [&] { return screen.state() == ScreenCaptureState::Ready; }, 10s))
        << screen.statusMessage().toStdString();
    screen.startPreview();
    devices.setCameraEnabled(true);
    devices.setMicrophoneEnabled(true);
    devices.setSystemAudioEnabled(true);
    ASSERT_TRUE(waitUntil(
        [&] {
            return screen.previewing() && devices.cameraCapturing() &&
                   devices.microphoneCapturing() &&
                   devices.systemAudioCapturing();
        },
        20s))
        << "screen=" << screen.statusMessage().toStdString()
        << " camera=" << devices.cameraStatus().toStdString()
        << " microphone=" << devices.microphoneStatus().toStdString()
        << " system=" << devices.systemAudioStatus().toStdString();

    auto recordingStore = std::make_shared<ProjectPackageStore>();
    auto engine = creator::app::makeLiveRecordingEngine(
        &screen, &devices, std::move(recordingStore));
    LiveRecordingController recording{
        std::move(engine), &project,
        [&project] { return project.recordingPackagePath(); }};
    auto studioPackages = std::make_shared<ProjectPackageStore>();
    creator::ffmpeg_adapter::FfmpegMediaProbe studioMediaProbe;
    auto reconciler =
        std::make_unique<creator::app::RecordingTimelineReconciler>(
            studioMediaProbe,
            [] { return creator::core::generateUuidV4(); },
            [] { return creator::core::Utc::now(); });
    creator::app::StudioWorkflowController workflow{
        [studioPackages](const fs::path& packageRoot)
            -> creator::core::Result<std::unique_ptr<
                creator::project_store::IStudioStore>> {
            auto package = studioPackages->open(packageRoot);
            if (!package.hasValue()) return package.error();
            const auto lease = package.value().databaseIdentityLease;
            if (!lease) {
                return creator::core::AppError{
                    creator::core::ErrorCode::IoFailure,
                    "validated Studio database identity is missing"};
            }
            auto store = creator::project_store::SqliteStudioStore::open(
                package.value().databasePath,
                package.value().package.manifest.projectId,
                [lease] { return lease->verifyCurrentIdentity(); });
            if (!store.hasValue()) return store.error();
            return std::unique_ptr<creator::project_store::IStudioStore>{
                new creator::project_store::SqliteStudioStore{
                    std::move(store).value()}};
        },
        std::move(reconciler),
        [] { return creator::core::generateUuidV4(); }};
    auto editEngine = std::make_unique<creator::mlt_adapter::MltEditEngine>(
        creator::mlt_adapter::MltEditEngineConfig{
            .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
            .previewWidth = 320,
            .previewHeight = 180});
    EditorController editor{std::move(editEngine)};
    creator::app::StudioRecordingBinding studioBinding{
        recording, workflow,
        [&project] { return project.projectUrl(); }};
    const auto editorBinding = creator::app::bindProjectEditor(
        project, editor, studioBinding);
    QSignalSpy reconciled{
        &studioBinding,
        &creator::app::StudioRecordingBinding::timelineReconciled};
    workflow.openProject(project.projectUrl());
    ASSERT_TRUE(waitUntil(
        [&] { return !workflow.busy() && workflow.sceneModel()->rowCount() > 0; },
        30s))
        << workflow.statusMessage().toStdString();
    editor.openProject(project.projectUrl());
    ASSERT_TRUE(waitRevision(editor, 0, 30s))
        << "editor=" << editor.statusMessage().toStdString();
    reconciled.clear();
    static_cast<void>(editorBinding);
    QSignalSpy committed{&recording,
                         &LiveRecordingController::recordingCommitted};
    QSignalSpy aborted{&recording,
                       &LiveRecordingController::recordingAborted};
    recording.startRecording();
    ASSERT_TRUE(waitUntil([&] { return recording.isRecording(); }, 20s))
        << recording.statusMessage().toStdString();
    ASSERT_TRUE(workflow.recording()) << workflow.statusMessage().toStdString();
    workflow.duplicateSelectedScene();
    ASSERT_TRUE(waitUntil(
        [&] { return !workflow.busy() && workflow.sceneModel()->rowCount() >= 2; },
        20s))
        << workflow.statusMessage().toStdString();

    constexpr auto requiredDuration = 30min;
    const auto started = std::chrono::steady_clock::now();
    auto nextSound = started;
    auto nextReport = started;
    bool markerAdded = false;
    bool sceneSwitched = false;
    while (std::chrono::steady_clock::now() - started < requiredDuration) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - started;
        if (!markerAdded && elapsed >= 1s && !workflow.busy()) {
            workflow.addMarker(QStringLiteral("실수 구간 1"),
                               recording.recordingPositionNs());
            markerAdded = true;
        }
        if (!sceneSwitched && elapsed >= 2s && !workflow.busy()) {
            const auto secondScene =
                static_cast<creator::app::StudioSceneModel*>(
                    workflow.sceneModel())
                    ->sceneIdAt(1);
            workflow.switchScene(secondScene, recording.recordingPositionNs());
            sceneSwitched = true;
        }
        if (now >= nextSound) {
            const std::wstring sound =
                L"C:\\Windows\\Media\\Windows Notify System Generic.wav";
            ASSERT_NE(PlaySoundW(sound.c_str(), nullptr,
                                 SND_FILENAME | SND_ASYNC | SND_NODEFAULT),
                      FALSE);
            nextSound = now + 10s;
        }
        if (now >= nextReport) {
            recording.pollDiagnostics();
            std::cout << "[ R1-07 SOAK ] elapsed_seconds="
                      << std::chrono::duration_cast<std::chrono::seconds>(now - started)
                             .count()
                      << " segments=" << recording.segmentCount()
                      << " tracks=" << recording.trackCount()
                      << " queued=" << recording.queuedItems()
                      << " max_drift_ms=" << recording.maximumDriftMilliseconds()
                      << std::endl;
            nextReport = now + 60s;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        std::this_thread::sleep_for(10ms);
    }
    PlaySoundW(nullptr, nullptr, 0);
    const bool survivedSoak = recording.isRecording();
    std::cout << "[ R1-07 TERMINAL ] survived=" << survivedSoak
              << " status=" << recording.statusMessage().toStdString()
              << " segments=" << recording.segmentCount()
              << " dropped=" << recording.droppedFrames() << std::endl;
    if (survivedSoak) recording.stopRecording();
    ASSERT_TRUE(waitUntil([&] { return !recording.isBusy(); }, 60s))
        << recording.statusMessage().toStdString();
    ASSERT_TRUE(survivedSoak) << recording.statusMessage().toStdString();
    ASSERT_EQ(aborted.count(), 0);
    ASSERT_EQ(committed.count(), 1) << recording.statusMessage().toStdString();
    ASSERT_TRUE(markerAdded);
    ASSERT_TRUE(sceneSwitched);
    ASSERT_TRUE(waitUntil(
        [&] {
            return reconciled.count() >= 1 && !workflow.busy() &&
                   !editor.sessionBusy() && editor.timelineRevision() >= 1;
        },
        // A 30-minute physical run produces thousands of clips.  Allow the
        // asynchronous store/import path enough time to reconcile that
        // intentionally large project before treating it as a failure.
        600s))
        << "workflow=" << workflow.statusMessage().toStdString()
        << " editor=" << editor.statusMessage().toStdString();
    EXPECT_EQ(workflow.markerCount(), 1);
    EXPECT_EQ(recording.trackCount(), 4);
    EXPECT_GT(recording.segmentCount(), 0);
    const auto allowedQueueDrops = std::max<qulonglong>(
        2U, screen.receivedFrames() / 10'000U);
    EXPECT_LE(recording.droppedFrames(), allowedQueueDrops);

    devices.setSystemAudioEnabled(false);
    devices.setMicrophoneEnabled(false);
    devices.setCameraEnabled(false);
    screen.stopPreview();
    ASSERT_TRUE(waitUntil(
        [&] {
            return !devices.systemAudioCapturing() &&
                   !devices.microphoneCapturing() &&
                   !devices.cameraCapturing() && !screen.previewing() &&
                   !screen.busy();
        },
        20s));

    ProjectPackageStore packages;
    auto reopened = packages.open(physical.package());
    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
    std::size_t videoFiles = 0;
    std::size_t audioFiles = 0;
    for (const auto& entry :
         fs::recursive_directory_iterator(physical.package())) {
        if (!entry.is_regular_file()) continue;
        const auto extension = entry.path().extension();
        if (extension == ".mkv") ++videoFiles;
        if (extension == ".mka") ++audioFiles;
        if (extension == ".mkv" || extension == ".mka") {
            std::cout << "[ R1-07 SEGMENT ] "
                      << entry.path().lexically_relative(physical.package())
                      << " bytes=" << entry.file_size() << std::endl;
        }
    }
    EXPECT_GE(videoFiles, 2U);
    EXPECT_GE(audioFiles, 2U);

    qlonglong revision = editor.timelineRevision();
    auto rows = timelineRows(editor);
    const auto camera = firstClipForTrack(rows, QStringLiteral("Camera"));
    const auto microphone =
        firstClipForTrack(rows, QStringLiteral("Microphone"));
    ASSERT_FALSE(camera.first.isEmpty());
    ASSERT_FALSE(camera.second.isEmpty());
    ASSERT_FALSE(microphone.first.isEmpty());
    ASSERT_FALSE(microphone.second.isEmpty());

    waitEditorIdle(editor);
    editor.selectClip(camera.first, camera.second);
    editor.applySelectedPipPreset(QStringLiteral("bottomRight"));
    ASSERT_TRUE(waitRevision(editor, ++revision))
        << editor.statusMessage().toStdString();
    waitEditorIdle(editor);
    editor.applySelectedVisualTransform(
        0.68, 0.62, 0.28, 0.28, 1.0, 1.0, 0.0, 0.02, 0.02, 0.02,
        0.02, 0.92, 5);
    ASSERT_TRUE(waitRevision(editor, ++revision))
        << editor.statusMessage().toStdString();

    waitEditorIdle(editor);
    editor.selectClip(microphone.first, microphone.second);
    editor.applySelectedAudioEnvelope(-3.0, 250'000'000, 250'000'000);
    ASSERT_TRUE(waitRevision(editor, ++revision))
        << editor.statusMessage().toStdString();

    seekAndWait(editor, 1'000'000'000);
    waitEditorIdle(editor);
    editor.markRangeIn();
    seekAndWait(editor, 1'250'000'000);
    waitEditorIdle(editor);
    editor.markRangeOut();
    editor.deleteMarkedRange(true);
    ASSERT_TRUE(waitRevision(editor, ++revision))
        << editor.statusMessage().toStdString();
    seekAndWait(editor, 4'000'000'000);
    waitEditorIdle(editor);
    editor.markRangeIn();
    seekAndWait(editor, 4'250'000'000);
    waitEditorIdle(editor);
    editor.markRangeOut();
    editor.deleteMarkedRange(true);
    ASSERT_TRUE(waitRevision(editor, ++revision))
        << editor.statusMessage().toStdString();

    seekAndWait(editor, 0);
    waitEditorIdle(editor);
    editor.addTitle(QStringLiteral("R1-07 실제 녹화 강의"),
                    QStringLiteral("Noto Sans"), 0.08, 0.08,
                    QStringLiteral("#ffffffff"),
                    QStringLiteral("#00000080"), QStringLiteral("left"));
    ASSERT_TRUE(waitRevision(editor, ++revision))
        << editor.statusMessage().toStdString();
    waitEditorIdle(editor);
    editor.addCaptionCue(0, 1'500'000'000,
                         QStringLiteral("실제 장치 녹화 자막 검증"));
    ASSERT_TRUE(waitRevision(editor, ++revision))
        << editor.statusMessage().toStdString();
    editor.save();
    ASSERT_TRUE(waitUntil(
        [&] { return !editor.sessionBusy() && editor.clean(); }, 60s))
        << editor.statusMessage().toStdString();
    const auto savedRows = timelineRows(editor);
    const auto editedDuration = editor.timelineDurationNs();
    ASSERT_GT(editedDuration, 0);

    auto reopenedEngine = std::make_unique<creator::mlt_adapter::MltEditEngine>(
        creator::mlt_adapter::MltEditEngineConfig{
            .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
            .previewWidth = 320,
            .previewHeight = 180});
    EditorController reopenedEditor{std::move(reopenedEngine)};
    reopenedEditor.openProject(project.projectUrl());
    ASSERT_TRUE(waitRevision(reopenedEditor, revision, 120s))
        << reopenedEditor.statusMessage().toStdString();
    EXPECT_EQ(timelineRows(reopenedEditor), savedRows);
    ASSERT_TRUE(reopenedEditor.exportSnapshot().has_value());

    auto projectId = creator::domain::ProjectId::create(
        project.projectId().toUtf8().toStdString());
    ASSERT_TRUE(projectId.hasValue()) << projectId.error().message();
    ExportController exporter{
        std::make_unique<creator::app::ProjectExportEngine>(
            fs::path{CS_TEST_STAGED_MLT_ROOT})};
    exporter.setSource(projectId.value(), *reopenedEditor.exportSnapshot());
    QSignalSpy exportFinished{&exporter, &ExportController::exportFinished};
    const auto cancelledPath = physical.root() / "cancelled.mp4";
    exporter.exportTo(
        QUrl::fromLocalFile(QString::fromStdWString(cancelledPath.wstring())),
        QStringLiteral("h264-1080p30"), false);
    ASSERT_TRUE(waitUntil(
        [&] { return exporter.canCancel() || !exporter.busy(); }, 60s))
        << exporter.statusMessage().toStdString();
    ASSERT_TRUE(exporter.busy())
        << "cancellation export completed before it could be cancelled";
    exporter.cancelExport();
    ASSERT_TRUE(waitUntil([&] { return exportFinished.count() >= 1; }, 120s))
        << exporter.statusMessage().toStdString();
    EXPECT_FALSE(exportFinished.takeFirst().front().toBool());
    EXPECT_FALSE(fs::exists(cancelledPath));

    const auto finalPath = physical.root() / fs::path{u8"최종 강의.mp4"};
    exporter.exportTo(
        QUrl::fromLocalFile(QString::fromStdWString(finalPath.wstring())),
        QStringLiteral("h264-1080p30"), false);
    ASSERT_TRUE(waitUntil([&] { return exportFinished.count() >= 1; }, 90min))
        << exporter.statusMessage().toStdString();
    const bool exportSucceeded = exportFinished.takeFirst().front().toBool();
    std::cout << "[ R1-07 EXPORT ] success=" << exportSucceeded
              << " status=" << exporter.statusMessage().toStdString()
              << std::endl;
    ASSERT_TRUE(exportSucceeded) << exporter.statusMessage().toStdString();
    ASSERT_TRUE(fs::is_regular_file(finalPath));

    creator::ffmpeg_adapter::FfmpegMediaProbe outputProbe;
    auto output = outputProbe.probe(physical.root(),
                                    finalPath.lexically_relative(physical.root()));
    ASSERT_TRUE(output.hasValue()) << output.error().message();
    EXPECT_TRUE(output.value().video.has_value());
    EXPECT_TRUE(output.value().audio.has_value());
    const auto outputDuration = output.value().duration.count();
    EXPECT_LE(std::abs(outputDuration - editedDuration), 1'000'000'000LL);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
