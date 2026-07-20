#include "project_store/PersistentRenderJobLifecycle.h"

#include "core/AppError.h"
#include "domain/Timeline.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace creator;
namespace fs = std::filesystem;

class RecordingStore final : public project_store::IRenderJobStore {
public:
    core::Result<void> begin(
        const project_store::RenderJobRecord& pending) override {
        record = pending;
        return core::ok();
    }

    core::Result<void> advance(
        const domain::RenderJobId&,
        const project_store::RenderJobUpdate& update) override {
        if (!record.has_value()) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "missing render record"};
        }
        record->progress = update.progress;
        record->diagnostics = update.diagnostics;
        record->startedAt = update.startedAt;
        record->updatedAt = update.updatedAt;
        record->finishedAt = update.finishedAt;
        updates.push_back(update);
        return core::ok();
    }

    core::Result<std::optional<project_store::RenderJobRecord>> load(
        const domain::RenderJobId&) override {
        return record;
    }

    core::Result<std::vector<project_store::RenderJobRecord>>
    listRecoverable() override {
        return std::vector<project_store::RenderJobRecord>{};
    }

    std::optional<project_store::RenderJobRecord> record;
    std::vector<project_store::RenderJobUpdate> updates;
};

edit_engine::RenderRequest request(const fs::path& destination) {
    const auto rate = core::FrameRate::create(30, 1).value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("timeline-1").value(),
                        "Timeline", rate)
                        .value();
    const auto trackId = domain::TrackId::create("title-track").value();
    EXPECT_TRUE(timeline.addTrack(
                            domain::Track::create(trackId,
                                                  domain::TrackKind::Title,
                                                  "Titles", true, false)
                                .value())
                    .hasValue());
    const auto range = domain::TimeRange::create(
                           core::TimestampNs{},
                           core::DurationNs{1'000'000'000})
                           .value();
    const auto payload = domain::TitlePayload::create(
                             "Export", "Creator Sans", 0.5, 0.5,
                             domain::RgbaColor::parse("#ffffffff").value(),
                             domain::RgbaColor::parse("#00000000").value(),
                             domain::TextAlignment::Center)
                             .value();
    EXPECT_TRUE(timeline.insertClip(
                            trackId,
                            domain::Clip::createTitle(
                                domain::ClipId::create("title-clip").value(),
                                range, true, payload, std::nullopt)
                                .value())
                    .hasValue());
    auto snapshot = edit_engine::TimelineSnapshot{
        .timeline = std::move(timeline),
        .revision = domain::TimelineRevision::create(1).value()};
    return edit_engine::RenderRequest::create(
               domain::ProjectId::create("project-1").value(),
               std::move(snapshot), destination,
               edit_engine::RenderPreset::h2641080p30().value(),
               edit_engine::RenderOverwritePolicy::FailIfExists)
        .value();
}

TEST(PersistentRenderJobLifecycleTest,
     PersistsArtifactEvidenceBeforeCompletion) {
    const auto root = fs::temp_directory_path() /
                      ("creator-studio-lifecycle-" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    fs::create_directories(root);
    struct Cleanup final {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    const auto destination = fs::absolute(root / "final.mp4");
    const auto partial = fs::absolute(root / "partial.mp4");
    auto renderRequest = request(destination);
    auto store = std::make_shared<RecordingStore>();
    project_store::PersistentRenderJobLifecycle lifecycle{store};

    ASSERT_TRUE(lifecycle.begin(renderRequest, partial,
                                core::DurationNs{1'000'000'000})
                    .hasValue());
    EXPECT_EQ(store->record->progress.state(),
              edit_engine::RenderJobState::Pending);
    ASSERT_TRUE(lifecycle.encoderSelected(
                              renderRequest.jobId(),
                              {.attemptedEncoders = "nvenc,qsv,x264",
                               .selectedEncoder = "x264",
                               .fallbackReason = "hardware unavailable"})
                    .hasValue());
    auto running = edit_engine::RenderProgress::create(
                       edit_engine::RenderJobState::Running, 0.75,
                       core::TimestampNs{core::DurationNs{750'000'000}},
                       core::DurationNs{1'000'000'000})
                       .value();
    ASSERT_TRUE(lifecycle.advance(renderRequest.jobId(), running).hasValue());
    {
        std::ofstream output(partial, std::ios::binary);
        output << "validated partial bytes";
    }
    auto publishing = edit_engine::RenderProgress::create(
                          edit_engine::RenderJobState::Publishing, 0.999,
                          core::TimestampNs{core::DurationNs{1'000'000'000}},
                          core::DurationNs{1'000'000'000})
                          .value();
    ASSERT_TRUE(lifecycle.preparePublication(renderRequest.jobId(), partial,
                                             publishing)
                    .hasValue());

    ASSERT_EQ(store->record->progress.state(),
              edit_engine::RenderJobState::Publishing);
    EXPECT_TRUE(store->record->diagnostics.outputSha256.has_value());
    EXPECT_FALSE(store->record->diagnostics.outputSha256->empty());
    EXPECT_TRUE(store->record->diagnostics.destinationIdentity.has_value());
    EXPECT_FALSE(store->record->diagnostics.destinationIdentity->empty());
    EXPECT_FALSE(fs::exists(destination));

    ASSERT_TRUE(lifecycle.finish(renderRequest.jobId(),
                                 edit_engine::RenderJobState::Completed, {})
                    .hasValue());
    EXPECT_EQ(store->record->progress.state(),
              edit_engine::RenderJobState::Completed);
    EXPECT_TRUE(store->record->finishedAt.has_value());
}

TEST(PersistentRenderJobLifecycleTest,
     RefusesPublishingWhenPartialCannotBeInspected) {
    const auto root = fs::absolute(
        fs::temp_directory_path() /
        ("creator-studio-missing-lifecycle-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count())));
    fs::create_directories(root);
    struct Cleanup final {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    auto renderRequest = request(root / "final.mp4");
    auto store = std::make_shared<RecordingStore>();
    project_store::PersistentRenderJobLifecycle lifecycle{store};
    ASSERT_TRUE(lifecycle.begin(renderRequest, root / "missing.partial.mp4",
                                core::DurationNs{1'000'000'000})
                    .hasValue());
    auto running = edit_engine::RenderProgress::create(
                       edit_engine::RenderJobState::Running, 0.5,
                       core::TimestampNs{core::DurationNs{500'000'000}},
                       core::DurationNs{1'000'000'000})
                       .value();
    ASSERT_TRUE(lifecycle.advance(renderRequest.jobId(), running).hasValue());
    auto publishing = edit_engine::RenderProgress::create(
                          edit_engine::RenderJobState::Publishing, 0.999,
                          core::TimestampNs{core::DurationNs{1'000'000'000}},
                          core::DurationNs{1'000'000'000})
                          .value();

    EXPECT_FALSE(lifecycle.preparePublication(
                              renderRequest.jobId(), root / "missing.partial.mp4",
                              publishing)
                     .hasValue());
    EXPECT_EQ(store->record->progress.state(),
              edit_engine::RenderJobState::Running);
}

TEST(PersistentRenderJobLifecycleTest,
     PersistsCancellingBeforeCancelledTerminalState) {
    const auto root = fs::absolute(
        fs::temp_directory_path() /
        ("creator-studio-cancel-lifecycle-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count())));
    fs::create_directories(root);
    struct Cleanup final {
        fs::path root;
        ~Cleanup() { fs::remove_all(root); }
    } cleanup{root};
    auto renderRequest = request(root / "cancelled.mp4");
    auto store = std::make_shared<RecordingStore>();
    project_store::PersistentRenderJobLifecycle lifecycle{store};
    ASSERT_TRUE(lifecycle.begin(renderRequest, root / "partial.mp4",
                                core::DurationNs{1'000'000'000})
                    .hasValue());
    auto running = edit_engine::RenderProgress::create(
                       edit_engine::RenderJobState::Running, 0.25,
                       core::TimestampNs{core::DurationNs{250'000'000}},
                       core::DurationNs{1'000'000'000})
                       .value();
    ASSERT_TRUE(lifecycle.advance(renderRequest.jobId(), running).hasValue());

    ASSERT_TRUE(lifecycle.finish(renderRequest.jobId(),
                                 edit_engine::RenderJobState::Cancelled,
                                 "cancelled by user")
                    .hasValue());
    ASSERT_GE(store->updates.size(), 3U);
    EXPECT_EQ(store->updates[store->updates.size() - 2].progress.state(),
              edit_engine::RenderJobState::Cancelling);
    EXPECT_EQ(store->updates.back().progress.state(),
              edit_engine::RenderJobState::Cancelled);
    EXPECT_EQ(store->record->diagnostics.diagnostic, "cancelled by user");
    EXPECT_TRUE(store->record->finishedAt.has_value());
}

}  // namespace
