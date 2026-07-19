#include "app/EditorController.h"
#include "app/ExportController.h"
#include "app/ProjectController.h"
#include "app/ProjectExportEngine.h"
#include "app/RecordingTimelineReconciler.h"
#include "app/StudioWorkflowController.h"
#include "core/Uuid.h"
#include "core/Utc.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "mlt_adapter/MltEditEngine.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QUrl>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

TEST(R1ReplayStressTest, ReopensRetainedPackageAndBuildsExportGraph) {
    const auto packageText = qEnvironmentVariable("CS_R1_REPLAY_PACKAGE");
    if (packageText.isEmpty()) {
        GTEST_SKIP() << "Set CS_R1_REPLAY_PACKAGE to the retained .cstudio package";
    }
    const fs::path packagePath{packageText.toStdWString()};
    ASSERT_TRUE(fs::is_directory(packagePath)) << packagePath.string();
    ASSERT_TRUE(fs::exists(packagePath / "manifest.json"))
        << "missing project manifest: " << packagePath.string();

    const auto registry = packagePath.parent_path() / "replay-recent.json";
    creator::app::ProjectController project{
        std::make_unique<creator::project_store::ProjectPackageStore>(),
        registry, false};
    project.openProject(QUrl::fromLocalFile(packageText));
    ASSERT_TRUE(waitUntil([&] { return !project.busy(); }, 30s))
        << project.statusMessage().toStdString();
    ASSERT_TRUE(project.hasOpenProject()) << project.statusMessage().toStdString();

    auto packages = std::make_shared<creator::project_store::ProjectPackageStore>();
    creator::ffmpeg_adapter::FfmpegMediaProbe mediaProbe;
    auto reconciler = std::make_unique<creator::app::RecordingTimelineReconciler>(
        mediaProbe, [] { return creator::core::generateUuidV4(); },
        [] { return creator::core::Utc::now(); });
    creator::app::StudioWorkflowController workflow{
        [packages](const fs::path& root)
            -> creator::core::Result<std::unique_ptr<
                creator::project_store::IStudioStore>> {
            auto package = packages->open(root);
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
                new creator::project_store::SqliteStudioStore{std::move(store).value()}};
        },
        std::move(reconciler), [] { return creator::core::generateUuidV4(); }};

    workflow.openProject(project.projectUrl());
    ASSERT_TRUE(waitUntil(
        [&] { return !workflow.busy() && !workflow.reconciling(); }, 15min))
        << workflow.statusMessage().toStdString();
    ASSERT_GT(workflow.sceneModel()->rowCount(), 0);

    auto editorEngine = std::make_unique<creator::mlt_adapter::MltEditEngine>(
        creator::mlt_adapter::MltEditEngineConfig{
            .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
            .previewWidth = 320,
            .previewHeight = 180});
    auto editor = std::make_unique<creator::app::EditorController>(
        std::move(editorEngine));
    editor->openProject(project.projectUrl());
    ASSERT_TRUE(waitUntil(
        [&] { return !editor->busy() && !editor->sessionBusy(); }, 15min))
        << editor->statusMessage().toStdString();
    ASSERT_TRUE(editor->exportSnapshot().has_value())
        << editor->statusMessage().toStdString();
    ASSERT_GT(editor->timelineDurationNs(), 0);

    const auto output = packagePath.parent_path() / "r1-replay-cancelled.mp4";
    std::error_code ignored;
    fs::remove(output, ignored);
    const auto projectId = creator::domain::ProjectId::create(
        project.projectId().toUtf8().toStdString());
    ASSERT_TRUE(projectId.hasValue()) << projectId.error().message();
    auto exporter = std::make_unique<creator::app::ExportController>(
        std::make_unique<creator::app::ProjectExportEngine>(
            fs::path{CS_TEST_STAGED_MLT_ROOT}));
    exporter->setSource(projectId.value(), *editor->exportSnapshot());
    exporter->exportTo(
        QUrl::fromLocalFile(QString::fromStdWString(output.wstring())),
        QStringLiteral("h264-1080p30"), false);
    ASSERT_TRUE(waitUntil([&] { return exporter->canCancel() || !exporter->busy(); },
                          120s))
        << exporter->statusMessage().toStdString();
    ASSERT_TRUE(exporter->busy()) << exporter->statusMessage().toStdString();
    exporter->cancelExport();
    ASSERT_TRUE(waitUntil([&] { return !exporter->busy(); }, 180s))
        << exporter->statusMessage().toStdString();
    EXPECT_FALSE(fs::exists(output));
    std::cout << "[R1-REPLAY] scenes=" << workflow.sceneModel()->rowCount()
              << " tracks=" << editor->timelineTrackModel()->rowCount()
              << " durationNs=" << editor->timelineDurationNs() << std::endl;
    std::cout << "[R1-REPLAY] destroying exporter" << std::endl;
    exporter.reset();
    std::cout << "[R1-REPLAY] destroyed exporter" << std::endl;
    std::cout << "[R1-REPLAY] destroying editor" << std::endl;
    editor.reset();
    std::cout << "[R1-REPLAY] destroyed editor" << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
