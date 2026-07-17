#include "project_store/SqliteRenderJobStore.h"

#include "core/Timebase.h"
#include "core/Utc.h"
#include "core/Uuid.h"
#include "domain/Identifiers.h"
#include "domain/TimelineRevision.h"
#include "edit_engine/EditEngineTypes.h"
#include "project_store/MigrationRunner.h"
#include "project_store/internal/SqliteConnection.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace fs = std::filesystem;

using creator::core::DurationNs;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::ProjectId;
using creator::domain::RenderJobId;
using creator::domain::TimelineId;
using creator::domain::TimelineRevision;
using creator::edit_engine::RenderJobState;
using creator::edit_engine::RenderOverwritePolicy;
using creator::edit_engine::RenderPreset;
using creator::edit_engine::RenderProgress;
using creator::project_store::MigrationRunner;
using creator::project_store::RenderJobDiagnostics;
using creator::project_store::RenderJobRecord;
using creator::project_store::RenderJobUpdate;
using creator::project_store::SqliteRenderJobStore;
using creator::project_store::internal::SqliteConnection;

Utc utc(std::string_view value) { return Utc::parseRfc3339(value).value(); }

class SqliteRenderJobStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("creator-studio-render-store-" +
                 creator::core::generateUuidV4());
        fs::create_directories(root_);
        database_ = root_ / "project.db";
        auto opened = SqliteConnection::open(database_);
        ASSERT_TRUE(opened.hasValue()) << opened.error().message();
        auto connection = std::move(opened).value();
        ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
        ASSERT_TRUE(connection.execute(
            "INSERT INTO projects VALUES("
            "'project-1','Export',1,'2026-07-18T00:00:00Z',"
            "'2026-07-18T00:00:00Z');"
            "INSERT INTO timelines(timeline_id,project_id,name,"
            "frame_rate_numerator,frame_rate_denominator,revision,is_primary) "
            "VALUES('timeline-1','project-1','Main',30,1,7,1);")
                        .hasValue());
        auto store = SqliteRenderJobStore::open(
            database_, ProjectId::create("project-1").value());
        ASSERT_TRUE(store.hasValue()) << store.error().message();
        store_.emplace(std::move(store).value());
    }

    void TearDown() override {
        store_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    RenderJobRecord pending(std::string id = "job-1",
                            std::string filename = "lesson.mp4") const {
        auto preset = RenderPreset::h2641080p30().value();
        auto progress = RenderProgress::create(
            RenderJobState::Pending, 0.0, TimestampNs{DurationNs{0}},
            DurationNs{1'000})
                            .value();
        return RenderJobRecord{
            .jobId = RenderJobId::create(std::move(id)).value(),
            .projectId = ProjectId::create("project-1").value(),
            .timelineId = TimelineId::create("timeline-1").value(),
            .timelineRevision = TimelineRevision::create(7).value(),
            .preset = std::move(preset),
            .overwritePolicy = RenderOverwritePolicy::FailIfExists,
            .destination = root_ / filename,
            .partial = root_ / ("." + filename + ".job.partial.mp4"),
            .progress = std::move(progress),
            .diagnostics = {},
            .createdAt = utc("2026-07-18T00:00:00Z"),
            .startedAt = std::nullopt,
            .updatedAt = utc("2026-07-18T00:00:00Z"),
            .finishedAt = std::nullopt};
    }

    RenderJobUpdate update(RenderJobState state, double fraction,
                           std::int64_t rendered,
                           std::string_view updatedAt) const {
        return RenderJobUpdate{
            .progress = RenderProgress::create(
                state, fraction, TimestampNs{DurationNs{rendered}},
                DurationNs{1'000})
                            .value(),
            .diagnostics = {},
            .startedAt = state == RenderJobState::Pending
                             ? std::nullopt
                             : std::optional{utc("2026-07-18T00:00:01Z")},
            .updatedAt = utc(updatedAt),
            .finishedAt = std::nullopt};
    }

    fs::path root_;
    fs::path database_;
    std::optional<SqliteRenderJobStore> store_;
};

TEST_F(SqliteRenderJobStoreTest, BeginsExactlyOnceAndSurvivesReopen) {
    const auto record = pending();
    ASSERT_TRUE(store_->begin(record).hasValue());
    ASSERT_TRUE(store_->begin(record).hasValue());

    auto conflicting = pending();
    conflicting.partial = root_ / ".different.partial.mp4";
    EXPECT_FALSE(store_->begin(conflicting).hasValue());

    store_.reset();
    auto reopened = SqliteRenderJobStore::open(
        database_, ProjectId::create("project-1").value());
    ASSERT_TRUE(reopened.hasValue()) << reopened.error().message();
    auto loaded = reopened.value().load(record.jobId);
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(loaded.value()->jobId, record.jobId);
    EXPECT_EQ(loaded.value()->timelineRevision, record.timelineRevision);
    EXPECT_EQ(loaded.value()->preset.id(), "h264-1080p30");
    EXPECT_EQ(loaded.value()->destination, record.destination);
    EXPECT_EQ(loaded.value()->progress, record.progress);
}

TEST_F(SqliteRenderJobStoreTest, EnforcesProgressPublishingAndTerminalState) {
    const auto record = pending();
    ASSERT_TRUE(store_->begin(record).hasValue());

    auto running = update(RenderJobState::Running, 0.5, 500,
                          "2026-07-18T00:00:01Z");
    running.diagnostics.attemptedEncoder = "h264_nvenc";
    ASSERT_TRUE(store_->advance(record.jobId, running).hasValue());

    auto regressed = update(RenderJobState::Running, 0.4, 400,
                            "2026-07-18T00:00:02Z");
    EXPECT_FALSE(store_->advance(record.jobId, regressed).hasValue());

    auto publishing = update(RenderJobState::Publishing, 0.999, 1'000,
                             "2026-07-18T00:00:03Z");
    publishing.diagnostics.attemptedEncoder = "h264_nvenc";
    publishing.diagnostics.selectedEncoder = "h264_mf";
    publishing.diagnostics.fallbackReason = "hardware preflight failed";
    publishing.diagnostics.outputSha256 =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    publishing.diagnostics.destinationIdentity = "volume:file";
    ASSERT_TRUE(store_->advance(record.jobId, publishing).hasValue());

    auto completed = update(RenderJobState::Completed, 1.0, 1'000,
                            "2026-07-18T00:00:04Z");
    completed.diagnostics = publishing.diagnostics;
    completed.finishedAt = utc("2026-07-18T00:00:04Z");
    ASSERT_TRUE(store_->advance(record.jobId, completed).hasValue());
    EXPECT_TRUE(store_->advance(record.jobId, completed).hasValue())
        << "an exact repeated terminal update must be idempotent";
    EXPECT_FALSE(store_->advance(record.jobId, publishing).hasValue());

    auto loaded = store_->load(record.jobId);
    ASSERT_TRUE(loaded.hasValue());
    ASSERT_TRUE(loaded.value().has_value());
    EXPECT_EQ(loaded.value()->progress.state(), RenderJobState::Completed);
    EXPECT_EQ(loaded.value()->diagnostics.selectedEncoder,
              std::optional<std::string>{"h264_mf"});
    EXPECT_EQ(loaded.value()->finishedAt,
              std::optional{utc("2026-07-18T00:00:04Z")});
}

TEST_F(SqliteRenderJobStoreTest, ListsOnlyRecoverableNonTerminalJobs) {
    auto first = pending("pending", "pending.mp4");
    auto second = pending("failed", "failed.mp4");
    ASSERT_TRUE(store_->begin(first).hasValue());
    ASSERT_TRUE(store_->begin(second).hasValue());
    auto failed = update(RenderJobState::Failed, 0.0, 0,
                         "2026-07-18T00:00:01Z");
    failed.startedAt = std::nullopt;
    failed.finishedAt = utc("2026-07-18T00:00:01Z");
    failed.diagnostics.diagnostic = "preflight failed";
    ASSERT_TRUE(store_->advance(second.jobId, failed).hasValue());

    auto recoverable = store_->listRecoverable();
    ASSERT_TRUE(recoverable.hasValue()) << recoverable.error().message();
    ASSERT_EQ(recoverable.value().size(), 1U);
    EXPECT_EQ(recoverable.value().front().jobId, first.jobId);
}

}  // namespace
