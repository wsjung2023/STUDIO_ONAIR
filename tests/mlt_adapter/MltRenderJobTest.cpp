#include "mlt_adapter/MltRenderJob.h"

#include "core/Uuid.h"
#include "domain/Timeline.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace {

using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::ProjectId;
using creator::domain::RgbaColor;
using creator::domain::TextAlignment;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TimelineRevision;
using creator::domain::TitlePayload;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::edit_engine::RenderJobState;
using creator::edit_engine::RenderOverwritePolicy;
using creator::edit_engine::RenderPreset;
using creator::edit_engine::RenderRequest;
using creator::edit_engine::TimelineSnapshot;
using creator::mlt_adapter::MltRenderJob;

class RenderJobFixture : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("creator-studio-mlt-render-job-" +
                 creator::core::generateUuidV4());
        ASSERT_TRUE(std::filesystem::create_directories(root_));
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    RenderRequest request(std::int64_t revision = 7) const {
        auto timeline =
            Timeline::create(TimelineId::create("timeline").value(), "Main",
                             FrameRate::create(30, 1).value())
                .value();
        const auto trackId = TrackId::create("titles").value();
        EXPECT_TRUE(timeline
                        .addTrack(Track::create(trackId, TrackKind::Title,
                                                "Titles", true, false)
                                      .value())
                        .hasValue());
        auto payload = TitlePayload::create(
                           "Frozen", "Creator Sans", 0.5, 0.5,
                           RgbaColor::parse("#ffffffff").value(),
                           RgbaColor::parse("#00000000").value(),
                           TextAlignment::Center)
                           .value();
        EXPECT_TRUE(timeline
                        .insertClip(
                            trackId,
                            Clip::createTitle(
                                ClipId::create("title").value(),
                                TimeRange::create(TimestampNs{},
                                                  DurationNs{1'000'000'000})
                                    .value(),
                                true, std::move(payload), std::nullopt)
                                .value())
                        .hasValue());
        TimelineSnapshot snapshot{
            std::move(timeline), TimelineRevision::create(revision).value()};
        return RenderRequest::create(
                   ProjectId::create("project").value(), std::move(snapshot),
                   root_ / "out.mp4", RenderPreset::h2641080p30().value(),
                   RenderOverwritePolicy::FailIfExists)
            .value();
    }

    static RenderJobState waitForTerminal(
        creator::edit_engine::IRenderJob& job) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds{2};
        while (std::chrono::steady_clock::now() < deadline) {
            auto value = job.progress();
            EXPECT_TRUE(value.hasValue());
            if (value.hasValue() &&
                (value.value().state() == RenderJobState::Completed ||
                 value.value().state() == RenderJobState::Failed ||
                 value.value().state() == RenderJobState::Cancelled)) {
                return value.value().state();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return job.progress().value().state();
    }

    std::filesystem::path root_;
};

TEST_F(RenderJobFixture, OwnsFrozenRequestAndPublishesMonotonicCompletion) {
    std::atomic<std::int64_t> observedRevision{};
    auto job = MltRenderJob::start(
        request(11), [&](const RenderRequest& frozen, std::stop_token,
                         const MltRenderJob::ProgressReporter& report) {
            observedRevision = frozen.snapshot().revision.value();
            EXPECT_TRUE(report(RenderJobState::Running, 0.5,
                               TimestampNs{DurationNs{500'000'000}}));
            EXPECT_TRUE(report(RenderJobState::Publishing, 0.999,
                               TimestampNs{DurationNs{1'000'000'000}}));
            return creator::core::ok();
        });

    ASSERT_TRUE(job.hasValue()) << job.error().message();
    EXPECT_EQ(waitForTerminal(*job.value()), RenderJobState::Completed);
    EXPECT_EQ(observedRevision.load(), 11);
    const auto completed = job.value()->progress();
    ASSERT_TRUE(completed.hasValue());
    EXPECT_DOUBLE_EQ(completed.value().fraction(), 1.0);
}

TEST_F(RenderJobFixture, FailureBecomesTerminalWithoutFalseCompletion) {
    auto job = MltRenderJob::start(
        request(), [](const RenderRequest&, std::stop_token,
                      const MltRenderJob::ProgressReporter&) {
            return creator::core::Result<void>{creator::core::AppError{
                creator::core::ErrorCode::IoFailure, "injected encode failure"}};
        });

    ASSERT_TRUE(job.hasValue());
    EXPECT_EQ(waitForTerminal(*job.value()), RenderJobState::Failed);
    EXPECT_LT(job.value()->progress().value().fraction(), 1.0);
}

TEST_F(RenderJobFixture, CancellationIsIdempotentlyJoinedOnDestruction) {
    std::atomic<bool> entered{};
    std::atomic<bool> exited{};
    {
        auto job = MltRenderJob::start(
            request(), [&](const RenderRequest&, std::stop_token token,
                           const MltRenderJob::ProgressReporter&) {
                entered = true;
                while (!token.stop_requested()) std::this_thread::yield();
                exited = true;
                return creator::core::ok();
            });
        ASSERT_TRUE(job.hasValue());
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds{2};
        while (!entered && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_TRUE(entered);
        ASSERT_TRUE(job.value()->cancel().hasValue());
        EXPECT_EQ(waitForTerminal(*job.value()), RenderJobState::Cancelled);
        EXPECT_FALSE(job.value()->cancel().hasValue());
    }
    EXPECT_TRUE(exited);
}

TEST_F(RenderJobFixture, RejectsCancellationAfterPublicationBoundary) {
    std::mutex mutex;
    std::condition_variable condition;
    bool publishing = false;
    bool release = false;
    auto job = MltRenderJob::start(
        request(), [&](const RenderRequest&, std::stop_token,
                       const MltRenderJob::ProgressReporter& report) {
            EXPECT_TRUE(report(RenderJobState::Publishing, 0.999,
                               TimestampNs{DurationNs{1'000'000'000}}));
            std::unique_lock lock(mutex);
            publishing = true;
            condition.notify_one();
            condition.wait(lock, [&] { return release; });
            return creator::core::ok();
        });
    ASSERT_TRUE(job.hasValue());
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds{2},
                                       [&] { return publishing; }));
    }
    EXPECT_FALSE(job.value()->cancel().hasValue());
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_one();
    EXPECT_EQ(waitForTerminal(*job.value()), RenderJobState::Completed);
}

}  // namespace
