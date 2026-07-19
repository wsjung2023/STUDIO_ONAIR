#include "app/ProjectSegmentLifecycleSink.h"

#include "app/FakeProjectPackageStore.h"

#include "core/Utc.h"
#include "domain/Identifiers.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteProjectDatabase.h"
#include "recorder/DurableSegmentPublisher.h"
#include "recorder/RecordingTrack.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace {

namespace fs = std::filesystem;
using creator::app::ProjectSegmentLifecycleContext;
using creator::app::ProjectSegmentLifecycleSink;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::RecordingSession;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::project_store::PersistedSessionState;
using creator::project_store::ProjectPackageStore;
using creator::project_store::SqliteProjectDatabase;
using creator::recorder::DurableSegmentPublisher;
using creator::recorder::RecordingTrack;
using creator::recorder::TrackRole;

Utc utc(std::string_view value) {
    return Utc::parseRfc3339(value).value();
}

class ProjectSegmentLifecycleSinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        root_ = fs::temp_directory_path() /
                ("cs_segment_sink_" + std::string{info->name()});
        std::error_code ignored;
        fs::remove_all(root_, ignored);
        fs::create_directories(root_);
        packagePath_ = root_ / "recording.cstudio";

        store_ = std::make_shared<ProjectPackageStore>();
        auto created = store_->create(packagePath_, "Segment sink");
        ASSERT_TRUE(created.hasValue()) << created.error().message();
        created_ = std::move(created).value();
        sessionId_ = SessionId::create("session-1").value();
        ASSERT_TRUE(store_->beginRecording(packagePath_, *sessionId_, TimestampNs{},
                                           utc("2026-07-16T10:00:00Z"))
                        .hasValue());
        RecordingSession session{*sessionId_};
        ASSERT_TRUE(session.start(TimestampNs{}).hasValue());
        context_ = ProjectSegmentLifecycleContext::create(
                       store_, packagePath_, std::move(session))
                       .value();
    }

    void TearDown() override {
        context_.reset();
        store_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    fs::path root_;
    fs::path packagePath_;
    std::shared_ptr<ProjectPackageStore> store_;
    std::optional<creator::project_store::OpenProjectResult> created_;
    std::optional<SessionId> sessionId_;
    std::shared_ptr<ProjectSegmentLifecycleContext> context_;
};

TEST_F(ProjectSegmentLifecycleSinkTest, PublishedFileAndReadyDatabaseRowStayInAgreement) {
    const auto source = SourceId::create("screen-1").value();
    const auto track = RecordingTrack::create(source, TrackRole::Screen).value();
    DurableSegmentPublisher publisher{
        packagePath_, creator::recorder::makeSegmentFileOperations(packagePath_),
        std::make_unique<ProjectSegmentLifecycleSink>(context_)};

    auto paths = publisher.begin(track, 0, TimestampNs{});
    ASSERT_TRUE(paths.hasValue()) << paths.error().message();
    {
        std::ofstream output{paths.value().partPath, std::ios::binary};
        output << "encoded-segment";
    }
    const auto published = publisher.publish(
        {.endTime = TimestampNs{std::chrono::seconds{2}},
         .bytesWritten = 15,
         .codecName = "fixture"});
    ASSERT_TRUE(published.hasValue()) << published.error().message();
    ASSERT_TRUE(context_->ready(published.value()).hasValue());

    EXPECT_TRUE(fs::is_regular_file(paths.value().finalPath));
    EXPECT_EQ(context_->sessionSnapshot().segmentCount(), 1u);
    const auto opened = store_->open(packagePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    ASSERT_EQ(opened.value().recoveryCandidates.size(), 1u);
    EXPECT_EQ(opened.value().recoveryCandidates.front().readySegments, 1u);
    EXPECT_EQ(opened.value().recoveryCandidates.front().writingSegments, 0u);

    const auto completed = context_->complete(
        TimestampNs{std::chrono::seconds{2}}, utc("2026-07-16T10:00:02Z"));
    ASSERT_TRUE(completed.hasValue()) << completed.error().message();
    ASSERT_EQ(completed.value().segmentCount(), 1u);

    auto database = SqliteProjectDatabase::open(
        packagePath_ / "project.db", created_->package.manifest.projectId);
    ASSERT_TRUE(database.hasValue()) << database.error().message();
    const auto record = database.value().session(*sessionId_);
    ASSERT_TRUE(record.hasValue()) << record.error().message();
    EXPECT_EQ(record.value().state, PersistedSessionState::Completed);
}

TEST_F(ProjectSegmentLifecycleSinkTest, FailedPublicationIsNotAggregatedAsReady) {
    ProjectSegmentLifecycleSink sink{context_};
    const auto source = SourceId::create("screen-1").value();
    const creator::domain::SegmentInfo writing{
        .index = 0,
        .sourceId = source,
        .startTime = {},
        .duration = {},
        .status = creator::domain::SegmentStatus::Writing,
        .relativePath = "media/screen/screen-1/segment_000000.mkv",
    };
    ASSERT_TRUE(sink.begin(writing).hasValue());
    ASSERT_TRUE(sink.failed(source, 0).hasValue());

    EXPECT_EQ(context_->sessionSnapshot().segmentCount(), 0u);
    const auto opened = store_->open(packagePath_);
    ASSERT_TRUE(opened.hasValue());
    ASSERT_EQ(opened.value().recoveryCandidates.size(), 1u);
    EXPECT_EQ(opened.value().recoveryCandidates.front().readySegments, 0u);
    EXPECT_EQ(opened.value().recoveryCandidates.front().writingSegments, 0u);
}

TEST(ProjectSegmentLifecycleContextTest, ReadyStoreFailureDoesNotEnterFinalSession) {
    auto store = std::make_shared<creator::app::test::FakeProjectPackageStore>();
    const auto sessionId = SessionId::create("session-failure").value();
    RecordingSession session{sessionId};
    ASSERT_TRUE(session.start(TimestampNs{}).hasValue());
    auto contextResult = ProjectSegmentLifecycleContext::create(
        store, "failure.cstudio", std::move(session));
    ASSERT_TRUE(contextResult.hasValue()) << contextResult.error().message();
    auto context = std::move(contextResult).value();
    store->failNextMarkSegmentReady(
        creator::core::AppError{creator::core::ErrorCode::IoFailure, "database unavailable"});
    ProjectSegmentLifecycleSink sink{context};
    const creator::domain::SegmentInfo ready{
        .index = 0,
        .sourceId = SourceId::create("screen-1").value(),
        .startTime = {},
        .duration = std::chrono::seconds{2},
        .status = creator::domain::SegmentStatus::Ready,
        .relativePath = "media/screen/screen-1/segment_000000.mkv",
    };

    const auto result = sink.ready(ready);

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::IoFailure);
    EXPECT_EQ(context->sessionSnapshot().segmentCount(), 0u);
}

}  // namespace
