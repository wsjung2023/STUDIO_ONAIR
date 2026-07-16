#include "app/ProjectController.h"

#include "app/FakeProjectPackageStore.h"
#include "app/RecentProjectRegistry.h"
#include "core/Utc.h"

#include <QSignalSpy>
#include <QThread>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

using creator::app::ProjectController;
using creator::app::test::FakeProjectPackageStore;
using creator::domain::SessionId;
using creator::project_store::OpenProjectResult;
using creator::project_store::ProjectPackage;
using creator::project_store::RecoveryCandidate;

class ProjectControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / "cs_project_controller";
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_);
        packagePath_ = root_ / "lesson.cstudio";
        otherPath_ = root_ / "other.cstudio";
        auto fake = std::make_unique<FakeProjectPackageStore>();
        fake_ = fake.get();
        controller_ = std::make_unique<ProjectController>(
            std::move(fake), root_ / "recent-projects.json", false);
    }

    void TearDown() override {
        controller_.reset();
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    fs::path root_;
    fs::path packagePath_;
    fs::path otherPath_;
    FakeProjectPackageStore* fake_{};
    std::unique_ptr<ProjectController> controller_;
};

TEST_F(ProjectControllerTest, CreateRunsStoreOffUiThreadAndPublishesProject) {
    QSignalSpy opened{controller_.get(), &ProjectController::projectOpened};

    controller_->createProject(QUrl::fromLocalFile(
                                   QString::fromStdWString(packagePath_.wstring())),
                               QStringLiteral("강의"));

    ASSERT_TRUE(opened.wait(3000));
    EXPECT_NE(fake_->lastThreadId(), QThread::currentThreadId());
    EXPECT_TRUE(controller_->hasOpenProject());
    EXPECT_EQ(controller_->projectName(), QStringLiteral("강의"));
    EXPECT_FALSE(controller_->busy());
}

TEST_F(ProjectControllerTest, OpenPublishesRecoveryCandidates) {
    auto result = fake_->create(packagePath_, "Recovery").value();
    result.recoveryCandidates.push_back(RecoveryCandidate{
        .packagePath = packagePath_,
        .projectName = "Recovery",
        .sessionId = SessionId::create("session-1").value(),
        .createdAt = creator::core::Utc::parseRfc3339("2026-07-16T12:00:00Z").value(),
        .readySegments = 1,
        .writingSegments = 1,
    });
    fake_->setOpenResult(std::move(result));
    QSignalSpy required{controller_.get(), &ProjectController::recoveryRequired};

    controller_->openProject(
        QUrl::fromLocalFile(QString::fromStdWString(packagePath_.wstring())));

    ASSERT_TRUE(required.wait(3000));
    EXPECT_EQ(controller_->recoveries().size(), 1);
}

TEST_F(ProjectControllerTest, RejectsSecondCommandWhileBusy) {
    fake_->holdNextCall();
    QSignalSpy opened{controller_.get(), &ProjectController::projectOpened};
    controller_->openProject(
        QUrl::fromLocalFile(QString::fromStdWString(packagePath_.wstring())));
    controller_->openProject(
        QUrl::fromLocalFile(QString::fromStdWString(otherPath_.wstring())));

    EXPECT_EQ(controller_->statusMessage(),
              QStringLiteral("A project operation is already running"));
    fake_->releaseHeldCall();
    if (controller_->busy()) EXPECT_TRUE(opened.wait(3000));
}

TEST_F(ProjectControllerTest, StartupScansRegisteredProjectsForRecoveries) {
    controller_.reset();
    fs::create_directories(packagePath_);
    creator::app::RecentProjectRegistry registry{root_ / "recent-projects.json"};
    ASSERT_TRUE(registry.remember(
                            packagePath_,
                            creator::core::Utc::parseRfc3339("2026-07-16T12:00:00Z").value())
                    .hasValue());
    auto fake = std::make_unique<FakeProjectPackageStore>();
    fake_ = fake.get();
    auto result = fake_->create(packagePath_, "Recovery").value();
    result.recoveryCandidates.push_back(RecoveryCandidate{
        .packagePath = packagePath_,
        .projectName = "Recovery",
        .sessionId = SessionId::create("startup-session").value(),
        .createdAt = creator::core::Utc::parseRfc3339("2026-07-16T12:00:00Z").value(),
        .readySegments = 2,
        .writingSegments = 1,
    });
    fake_->setOpenResult(std::move(result));
    controller_ = std::make_unique<ProjectController>(
        std::move(fake), root_ / "recent-projects.json", true);
    QSignalSpy required{controller_.get(), &ProjectController::recoveryRequired};

    ASSERT_TRUE(required.wait(3000));
    ASSERT_EQ(controller_->recoveries().size(), 1);
    EXPECT_EQ(controller_->recentProjects().size(), 1);
    EXPECT_NE(fake_->lastThreadId(), QThread::currentThreadId());
}

TEST_F(ProjectControllerTest, RegistryBacksUpMalformedFileOnNextRemember) {
    controller_.reset();
    const fs::path registryPath = root_ / "recent-projects.json";
    std::ofstream{registryPath} << "not json";
    creator::app::RecentProjectRegistry registry{registryPath};

    ASSERT_TRUE(registry.remember(
                            packagePath_,
                            creator::core::Utc::parseRfc3339("2026-07-16T12:00:00Z").value())
                    .hasValue());

    const auto loaded = registry.load();
    ASSERT_TRUE(loaded.hasValue());
    ASSERT_EQ(loaded.value().size(), 1u);
    bool foundBackup = false;
    for (const auto& entry : fs::directory_iterator{root_}) {
        if (entry.path().filename().string().find("recent-projects.json.corrupt-") == 0) {
            foundBackup = true;
        }
    }
    EXPECT_TRUE(foundBackup);
}

TEST_F(ProjectControllerTest, NoOpenProjectFailsAsynchronouslyExactlyOnce) {
    int completionCount = 0;
    creator::core::ErrorCode errorCode = creator::core::ErrorCode::Unknown;
    Qt::HANDLE completionThread = nullptr;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    controller_->begin(SessionId::create("session-1").value(), {},
                       [&](creator::core::Result<void> result) {
                           ++completionCount;
                           completionThread = QThread::currentThreadId();
                           if (!result.hasValue()) errorCode = result.error().code();
                           loop.quit();
                       });

    EXPECT_EQ(completionCount, 0);
    timeout.start(3000);
    loop.exec();
    EXPECT_EQ(completionCount, 1);
    EXPECT_EQ(errorCode, creator::core::ErrorCode::InvalidState);
    EXPECT_EQ(completionThread, QThread::currentThreadId());
}

TEST_F(ProjectControllerTest, RecordingPersistenceLeavesAndReturnsToUiThread) {
    QSignalSpy opened{controller_.get(), &ProjectController::projectOpened};
    controller_->createProject(
        QUrl::fromLocalFile(QString::fromStdWString(packagePath_.wstring())),
        QStringLiteral("Recording"));
    ASSERT_TRUE(opened.wait(3000));
    fake_->holdNextCall();

    int completionCount = 0;
    Qt::HANDLE completionThread = nullptr;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    controller_->begin(SessionId::create("session-1").value(), {},
                       [&](creator::core::Result<void> result) {
                           EXPECT_TRUE(result.hasValue());
                           ++completionCount;
                           completionThread = QThread::currentThreadId();
                           loop.quit();
                       });

    EXPECT_EQ(completionCount, 0);
    fake_->releaseHeldCall();
    timeout.start(3000);
    loop.exec();
    EXPECT_EQ(completionCount, 1);
    EXPECT_NE(fake_->lastThreadId(), QThread::currentThreadId());
    EXPECT_EQ(completionThread, QThread::currentThreadId());
}

}  // namespace
