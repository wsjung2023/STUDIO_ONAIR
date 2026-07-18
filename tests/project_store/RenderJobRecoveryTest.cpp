#include "project_store/RenderJobRecovery.h"

#include "core/Uuid.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace creator;

core::Utc utc(std::string_view value) {
    return core::Utc::parseRfc3339(value).value();
}

class MemoryRenderJobStore final : public project_store::IRenderJobStore {
public:
    explicit MemoryRenderJobStore(project_store::RenderJobRecord record)
        : record_(std::move(record)) {}

    core::Result<void> begin(
        const project_store::RenderJobRecord&) override {
        return core::ok();
    }
    core::Result<void> advance(
        const domain::RenderJobId&,
        const project_store::RenderJobUpdate& update) override {
        record_.progress = update.progress;
        record_.diagnostics = update.diagnostics;
        record_.startedAt = update.startedAt;
        record_.updatedAt = update.updatedAt;
        record_.finishedAt = update.finishedAt;
        return core::ok();
    }
    core::Result<std::optional<project_store::RenderJobRecord>> load(
        const domain::RenderJobId&) override {
        return std::optional{record_};
    }
    core::Result<std::vector<project_store::RenderJobRecord>>
    listRecoverable() override {
        return std::vector{record_};
    }

    const project_store::RenderJobRecord& record() const { return record_; }

private:
    project_store::RenderJobRecord record_;
};

class RenderJobRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("creator-studio-render-recovery-" + core::generateUuidV4());
        ASSERT_TRUE(fs::create_directories(root_));
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    project_store::RenderJobRecord record(
        edit_engine::RenderJobState state) const {
        const bool publishing = state == edit_engine::RenderJobState::Publishing;
        return {
            .jobId = domain::RenderJobId::create("job").value(),
            .projectId = domain::ProjectId::create("project").value(),
            .timelineId = domain::TimelineId::create("timeline").value(),
            .timelineRevision = domain::TimelineRevision::create(1).value(),
            .preset = edit_engine::RenderPreset::h2641080p30().value(),
            .overwritePolicy =
                edit_engine::RenderOverwritePolicy::FailIfExists,
            .destination = root_ / "lesson.mp4",
            .partial = root_ / ".lesson.partial.mp4",
            .progress = edit_engine::RenderProgress::create(
                            state, publishing ? 0.999 : 0.5,
                            core::TimestampNs{core::DurationNs{
                                publishing ? 1'000 : 500}},
                            core::DurationNs{1'000})
                            .value(),
            .diagnostics = {},
            .createdAt = utc("2026-07-18T00:00:00Z"),
            .startedAt = utc("2026-07-18T00:00:01Z"),
            .updatedAt = utc("2026-07-18T00:00:02Z"),
            .finishedAt = std::nullopt};
    }

    static void write(const fs::path& path, std::string_view bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    fs::path root_;
};

TEST_F(RenderJobRecoveryTest, PublishesOnlyPartialMatchingHashAndIdentity) {
    auto value = record(edit_engine::RenderJobState::Publishing);
    write(value.partial, "complete-export");
    auto evidence = project_store::inspectRenderArtifact(value.partial);
    ASSERT_TRUE(evidence.hasValue()) << evidence.error().message();
    value.diagnostics.outputSha256 = evidence.value().sha256;
    value.diagnostics.destinationIdentity = evidence.value().identity;
    MemoryRenderJobStore store{value};

    auto recovered = project_store::RenderJobRecovery::recoverAll(
        store, utc("2026-07-18T00:00:03Z"));

    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    ASSERT_EQ(recovered.value().size(), 1U);
    EXPECT_EQ(recovered.value()[0].state,
              edit_engine::RenderJobState::Completed);
    EXPECT_TRUE(fs::exists(value.destination));
    EXPECT_FALSE(fs::exists(value.partial));
    EXPECT_EQ(store.record().progress.state(),
              edit_engine::RenderJobState::Completed);
}

TEST_F(RenderJobRecoveryTest, LeavesMismatchedArtifactUntouchedAndFailsJob) {
    auto value = record(edit_engine::RenderJobState::Publishing);
    write(value.partial, "expected");
    auto evidence = project_store::inspectRenderArtifact(value.partial).value();
    value.diagnostics.outputSha256 = evidence.sha256;
    value.diagnostics.destinationIdentity = evidence.identity;
    write(value.partial, "tampered");
    MemoryRenderJobStore store{value};

    auto recovered = project_store::RenderJobRecovery::recoverAll(
        store, utc("2026-07-18T00:00:03Z"));

    ASSERT_TRUE(recovered.hasValue());
    EXPECT_EQ(store.record().progress.state(),
              edit_engine::RenderJobState::Failed);
    EXPECT_TRUE(fs::exists(value.partial));
    EXPECT_FALSE(fs::exists(value.destination));
}

TEST_F(RenderJobRecoveryTest, ReconcilesAlreadyPublishedMatchingFinal) {
    auto value = record(edit_engine::RenderJobState::Publishing);
    write(value.destination, "already-published");
    auto evidence =
        project_store::inspectRenderArtifact(value.destination).value();
    value.diagnostics.outputSha256 = evidence.sha256;
    value.diagnostics.destinationIdentity = evidence.identity;
    MemoryRenderJobStore store{value};

    auto recovered = project_store::RenderJobRecovery::recoverAll(
        store, utc("2026-07-18T00:00:03Z"));

    ASSERT_TRUE(recovered.hasValue());
    EXPECT_EQ(store.record().progress.state(),
              edit_engine::RenderJobState::Completed);
    EXPECT_TRUE(fs::exists(value.destination));
}

TEST_F(RenderJobRecoveryTest, FinalizesInterruptedRunningAndCancellingJobs) {
    MemoryRenderJobStore running{record(edit_engine::RenderJobState::Running)};
    ASSERT_TRUE(project_store::RenderJobRecovery::recoverAll(
                    running, utc("2026-07-18T00:00:03Z"))
                    .hasValue());
    EXPECT_EQ(running.record().progress.state(),
              edit_engine::RenderJobState::Failed);

    MemoryRenderJobStore cancelling{
        record(edit_engine::RenderJobState::Cancelling)};
    ASSERT_TRUE(project_store::RenderJobRecovery::recoverAll(
                    cancelling, utc("2026-07-18T00:00:03Z"))
                    .hasValue());
    EXPECT_EQ(cancelling.record().progress.state(),
              edit_engine::RenderJobState::Cancelled);
}

TEST_F(RenderJobRecoveryTest, RejectsHardLinkedArtifactEvidence) {
    const auto first = root_ / "first.mp4";
    const auto second = root_ / "second.mp4";
    write(first, "payload");
#ifdef _WIN32
    ASSERT_TRUE(CreateHardLinkW(second.c_str(), first.c_str(), nullptr));
#else
    ASSERT_EQ(link(first.c_str(), second.c_str()), 0);
#endif
    auto evidence = project_store::inspectRenderArtifact(first);
    EXPECT_FALSE(evidence.hasValue());
    EXPECT_EQ(evidence.error().code(), core::ErrorCode::InvalidState);
}

}  // namespace
