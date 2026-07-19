#include "app/RecordingTimelineReconciler.h"

#include "core/AppError.h"
#include "domain/RecordingSession.h"
#include "domain/StudioScene.h"
#include "domain/Timeline.h"
#include "fakes/FakeMediaProbe.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using creator::app::RecordingTimelineReconciler;
using creator::core::AppError;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::RecordingSession;
using creator::domain::SegmentInfo;
using creator::domain::SegmentStatus;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::domain::StudioSourceRole;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::media::IMediaProbe;
using creator::media::MediaProbeResult;
using creator::project_store::ProjectPackageStore;
using creator::project_store::RecordingMarker;
using creator::project_store::RecordingSourceRole;
using creator::project_store::SqliteStudioStore;
using creator::project_store::SqliteTimelineStore;

std::uint64_t processId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

Utc fixedUtc() {
    return Utc::parseRfc3339("2026-07-17T12:00:00Z").value();
}

class StableIdentityLease final : public creator::media::IMediaIdentityLease {
public:
    [[nodiscard]] creator::core::Result<void> verifyCurrentIdentity()
        const override {
        return creator::core::ok();
    }
};

class ChangingIdentityLease final
    : public creator::media::IMediaIdentityLease {
public:
    [[nodiscard]] creator::core::Result<void> verifyCurrentIdentity()
        const override {
        if (verified_.fetch_add(1, std::memory_order_relaxed) == 0) {
            return creator::core::ok();
        }
        return AppError{ErrorCode::IoFailure,
                        "media replaced before import commit"};
    }

private:
    mutable std::atomic_size_t verified_{0};
};

MediaProbeResult screenProbe(std::string sha = std::string(64, 'a')) {
    return MediaProbeResult{
        .duration = 3s,
        .video = creator::domain::VideoAssetMetadata{
            .width = 1920,
            .height = 1080,
            .frameRate = creator::core::FrameRate::create(60, 1).value()},
        .audio = std::nullopt,
        .formatName = "matroska",
        .codecName = "h264",
        .byteSize = 16'384,
        .sha256 = std::move(sha),
        .identityLease = std::make_shared<StableIdentityLease>()};
}

class ChangingMediaProbe final : public IMediaProbe {
public:
    [[nodiscard]] creator::core::Result<MediaProbeResult> probe(
        const fs::path&, const fs::path&) override {
        ++calls;
        auto result = screenProbe();
        result.identityLease = std::make_shared<ChangingIdentityLease>();
        return result;
    }

    std::size_t calls{0};
};

class BarrierMediaProbe final : public IMediaProbe {
public:
    [[nodiscard]] creator::core::Result<MediaProbeResult> probe(
        const fs::path&, const fs::path&) override {
        const auto call = calls_.fetch_add(1, std::memory_order_relaxed);
        if (call < 2) firstProbes_.arrive_and_wait();
        return screenProbe();
    }

private:
    std::atomic_size_t calls_{0};
    std::barrier<> firstProbes_{2};
};

class ExpiringIdentityLease final : public creator::media::IMediaIdentityLease {
public:
    [[nodiscard]] creator::core::Result<void> verifyCurrentIdentity()
        const override {
        if (verifications_.fetch_add(1, std::memory_order_relaxed) < 2) {
            return creator::core::ok();
        }
        return AppError{ErrorCode::IoFailure,
                        "media replaced at transaction commit boundary"};
    }

private:
    mutable std::atomic_size_t verifications_{0};
};

class ExpiringLeaseMediaProbe final : public IMediaProbe {
public:
    [[nodiscard]] creator::core::Result<MediaProbeResult> probe(
        const fs::path&, const fs::path&) override {
        auto result = screenProbe();
        if (calls_++ == 0) {
            result.identityLease = std::make_shared<ExpiringIdentityLease>();
        }
        return result;
    }

private:
    std::size_t calls_{0};
};

class CountingConcatMediaProbe final : public IMediaProbe {
public:
    [[nodiscard]] creator::core::Result<MediaProbeResult> probe(
        const fs::path&, const fs::path& relativePath) override {
        paths.push_back(relativePath);
        auto result = screenProbe();
        result.duration =
            relativePath.extension() == ".ffconcat" ? concatDuration : 2s;
        return result;
    }

    DurationNs concatDuration{6506ms};
    std::vector<fs::path> paths;
};

class RecordingTimelineReconcilerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        directory_ = fs::temp_directory_path() /
                     ("cs_reconcile_" + std::to_string(processId()) + "_" +
                      std::string{info->name()});
        std::error_code ec;
        fs::remove_all(directory_, ec);
        fs::create_directories(directory_);
        packageRoot_ = directory_ / "recording.creatorstudio";

        const auto created = packages_.create(packageRoot_, "Reconcile");
        ASSERT_TRUE(created.hasValue()) << created.error().message();
        const auto& manifest = created.value().package.manifest;
        const auto databasePath = packageRoot_ / manifest.database;

        auto timelineStore =
            SqliteTimelineStore::open(databasePath, manifest.projectId);
        ASSERT_TRUE(timelineStore.hasValue()) << timelineStore.error().message();
        auto timeline = Timeline::create(
            TimelineId::create("main").value(), "Main",
            creator::core::FrameRate::create(60, 1).value());
        ASSERT_TRUE(timeline.hasValue());
        ASSERT_TRUE(timelineStore.value().createTimeline(timeline.value()).hasValue());

        ASSERT_TRUE(packages_.beginRecording(packageRoot_, sessionId(),
                                             sessionOrigin_, fixedUtc())
                        .hasValue());
        auto studio = SqliteStudioStore::open(databasePath, manifest.projectId);
        ASSERT_TRUE(studio.hasValue()) << studio.error().message();
        const auto scenes = creator::domain::defaultStudioScenes();
        ASSERT_TRUE(scenes.hasValue());
        ASSERT_TRUE(studio.value().seedDefaultsIfEmpty(scenes.value()).hasValue());
        ASSERT_TRUE(studio.value()
                        .prepareRecording(
                            sessionId(),
                            {RecordingSourceRole{.sourceId = sourceId(),
                                                 .role = StudioSourceRole::Screen}},
                            scenes.value().front().id())
                        .hasValue());
        ASSERT_TRUE(studio.value()
                        .recordMarker(RecordingMarker{.markerId = "marker-1",
                                                     .sessionId = sessionId(),
                                                     .position = TimestampNs{} + 1s,
                                                     .label = "Chapter"})
                        .hasValue());

        RecordingSession session{sessionId()};
        ASSERT_TRUE(session.start(sessionOrigin_).hasValue());
        ASSERT_TRUE(session.addSegment(segment()).hasValue());
        ASSERT_TRUE(session.stop(sessionOrigin_ + 3s).hasValue());
        ASSERT_TRUE(packages_.completeRecording(packageRoot_, session, fixedUtc())
                        .hasValue());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(directory_, ec);
    }

    [[nodiscard]] SessionId sessionId() const {
        return SessionId::create("session-1").value();
    }

    [[nodiscard]] SourceId sourceId() const {
        return SourceId::create("screen").value();
    }

    [[nodiscard]] SegmentInfo segment() const {
        return SegmentInfo{.index = 0,
                           .sourceId = sourceId(),
                           .startTime = sessionOrigin_,
                           .duration = 3s,
                           .status = SegmentStatus::Ready,
                           .relativePath = "media/screen-0.mkv"};
    }

    [[nodiscard]] auto openTimelineStore() const {
        const auto opened = packages_.open(packageRoot_).value();
        return SqliteTimelineStore::open(
            packageRoot_ / opened.package.manifest.database,
            opened.package.manifest.projectId);
    }

    [[nodiscard]] auto openStudioStore() const {
        const auto opened = packages_.open(packageRoot_).value();
        return SqliteStudioStore::open(
            packageRoot_ / opened.package.manifest.database,
            opened.package.manifest.projectId);
    }

    fs::path directory_;
    fs::path packageRoot_;
    TimestampNs sessionOrigin_{DurationNs{268'907'000'000'000}};
    mutable ProjectPackageStore packages_;
};

TEST_F(RecordingTimelineReconcilerTest,
       ImportsOnceAndReturnsPersistedResultWithoutProbingAgain) {
    creator::fakes::FakeMediaProbe probe;
    probe.set(segment().relativePath, screenProbe());
    std::uint64_t eventSequence = 0;
    RecordingTimelineReconciler reconciler{
        probe,
        [&eventSequence] {
            ++eventSequence;
            return "reconcile-event-" + std::to_string(eventSequence);
        },
        [] { return fixedUtc(); }};

    const auto first = reconciler.reconcile(packageRoot_, sessionId());

    ASSERT_TRUE(first.hasValue()) << first.error().message();
    EXPECT_TRUE(first.value().imported);
    EXPECT_EQ(first.value().revision, 1);
    EXPECT_EQ(first.value().assetCount, 1U);
    EXPECT_EQ(first.value().trackCount, 1U);
    EXPECT_EQ(first.value().markerCount, 1U);
    auto timelineStore = openTimelineStore();
    ASSERT_TRUE(timelineStore.hasValue());
    const auto afterFirst = timelineStore.value().loadPrimaryTimeline();
    const auto assetsAfterFirst = timelineStore.value().assets();
    ASSERT_TRUE(afterFirst.hasValue());
    ASSERT_TRUE(assetsAfterFirst.hasValue());
    ASSERT_EQ(afterFirst.value().timeline.tracks().size(), 1U);
    ASSERT_EQ(afterFirst.value().timeline.markers().size(), 1U);
    ASSERT_EQ(assetsAfterFirst.value().size(), 1U);
    ASSERT_EQ(afterFirst.value().timeline.tracks().front().clips().size(), 1U);
    EXPECT_EQ(afterFirst.value().timeline.tracks().front().clips().front()
                  .timelineRange()
                  .start(),
              TimestampNs{});

    probe.set(segment().relativePath,
              AppError{ErrorCode::IoFailure, "must not probe imported media"});
    const auto second = reconciler.reconcile(packageRoot_, sessionId());

    ASSERT_TRUE(second.hasValue()) << second.error().message();
    EXPECT_FALSE(second.value().imported);
    EXPECT_EQ(second.value().revision, 1);
    const auto third = reconciler.reconcile(packageRoot_, sessionId());
    ASSERT_TRUE(third.hasValue()) << third.error().message();
    EXPECT_EQ(third.value(), second.value());
    auto reopenedTimeline = openTimelineStore();
    ASSERT_TRUE(reopenedTimeline.hasValue());
    const auto afterSecond = reopenedTimeline.value().loadPrimaryTimeline();
    const auto assetsAfterSecond = reopenedTimeline.value().assets();
    ASSERT_TRUE(afterSecond.hasValue());
    ASSERT_TRUE(assetsAfterSecond.hasValue());
    EXPECT_EQ(afterSecond.value(), afterFirst.value());
    EXPECT_EQ(assetsAfterSecond.value(), assetsAfterFirst.value());
    auto studio = openStudioStore();
    ASSERT_TRUE(studio.hasValue());
    const auto import = studio.value().recordingImport(sessionId());
    ASSERT_TRUE(import.hasValue());
    ASSERT_TRUE(import.value().has_value());
    EXPECT_EQ(import.value()->importedRevision, 1);
}

TEST_F(RecordingTimelineReconcilerTest,
       RejectsMediaIdentityChangeWithoutPublishingPartialImport) {
    ChangingMediaProbe probe;
    RecordingTimelineReconciler reconciler{
        probe, [] { return std::string{"reconcile-event"}; },
        [] { return fixedUtc(); }};

    const auto result = reconciler.reconcile(packageRoot_, sessionId());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    EXPECT_EQ(probe.calls, 1U);
    auto timelineStore = openTimelineStore();
    ASSERT_TRUE(timelineStore.hasValue());
    const auto timeline = timelineStore.value().loadPrimaryTimeline();
    const auto assets = timelineStore.value().assets();
    ASSERT_TRUE(timeline.hasValue());
    ASSERT_TRUE(assets.hasValue());
    EXPECT_EQ(timeline.value().revision, 0);
    EXPECT_TRUE(timeline.value().timeline.tracks().empty());
    EXPECT_TRUE(timeline.value().timeline.markers().empty());
    EXPECT_TRUE(assets.value().empty());
    auto studio = openStudioStore();
    ASSERT_TRUE(studio.hasValue());
    const auto import = studio.value().recordingImport(sessionId());
    ASSERT_TRUE(import.hasValue());
    EXPECT_FALSE(import.value().has_value());
}

TEST_F(RecordingTimelineReconcilerTest,
       MergesPhysicalScreenJitterIntoOneConcatAssetAndProbe) {
    const auto concatSession = SessionId::create("concat-session").value();
    const auto origin = sessionOrigin_ + 10s;
    ASSERT_TRUE(packages_.beginRecording(packageRoot_, concatSession, origin,
                                         fixedUtc())
                    .hasValue());
    auto studio = openStudioStore();
    ASSERT_TRUE(studio.hasValue());
    ASSERT_TRUE(studio.value()
                    .prepareRecording(
                        concatSession,
                        {RecordingSourceRole{.sourceId = sourceId(),
                                             .role = StudioSourceRole::Screen}},
                        creator::domain::defaultStudioScenes().value().front().id())
                    .hasValue());
    RecordingSession session{concatSession};
    ASSERT_TRUE(session.start(origin).hasValue());
    ASSERT_TRUE(session
                    .addSegment(SegmentInfo{.index = 0,
                                            .sourceId = sourceId(),
                                            .startTime = origin,
                                            .duration = 2s,
                                            .status = SegmentStatus::Ready,
                                            .relativePath = "media/concat-0.mkv"})
                    .hasValue());
    ASSERT_TRUE(session
                    .addSegment(SegmentInfo{.index = 1,
                                            .sourceId = sourceId(),
                                            .startTime = origin + 2679ms,
                                            .duration = 2s,
                                            .status = SegmentStatus::Ready,
                                            .relativePath = "media/concat-1.mkv"})
                    .hasValue());
    ASSERT_TRUE(session
                    .addSegment(SegmentInfo{.index = 2,
                                            .sourceId = sourceId(),
                                            .startTime = origin + 4506ms,
                                            .duration = 2s,
                                            .status = SegmentStatus::Ready,
                                            .relativePath = "media/concat-2.mkv"})
                    .hasValue());
    ASSERT_TRUE(session.stop(origin + 6506ms).hasValue());
    ASSERT_TRUE(packages_.completeRecording(packageRoot_, session, fixedUtc())
                    .hasValue());

    CountingConcatMediaProbe probe;
    RecordingTimelineReconciler reconciler{
        probe, [] { return std::string{"concat-event"}; },
        [] { return fixedUtc(); }};

    const auto result = reconciler.reconcile(packageRoot_, concatSession);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().assetCount, 1U);
    ASSERT_EQ(probe.paths.size(), 1U);
    EXPECT_EQ(probe.paths.front().extension(), ".ffconcat");
}

TEST_F(RecordingTimelineReconcilerTest,
       SplitsRecordingWhenARealGapExceedsTheJitterTolerance) {
    const auto concatSession = SessionId::create("concat-loss-session").value();
    const auto origin = sessionOrigin_ + 10s;
    ASSERT_TRUE(packages_.beginRecording(packageRoot_, concatSession, origin,
                                         fixedUtc())
                    .hasValue());
    auto studio = openStudioStore();
    ASSERT_TRUE(studio.hasValue());
    ASSERT_TRUE(studio.value()
                    .prepareRecording(
                        concatSession,
                        {RecordingSourceRole{.sourceId = sourceId(),
                                             .role = StudioSourceRole::Screen}},
                        creator::domain::defaultStudioScenes().value().front().id())
                    .hasValue());
    RecordingSession session{concatSession};
    ASSERT_TRUE(session.start(origin).hasValue());
    const std::vector<SegmentInfo> segments{
        {.index = 0,
         .sourceId = sourceId(),
         .startTime = origin,
         .duration = 2s,
         .status = SegmentStatus::Ready,
         .relativePath = "media/loss-0.mkv"},
        {.index = 1,
         .sourceId = sourceId(),
         .startTime = origin + 2100ms,
         .duration = 2s,
         .status = SegmentStatus::Ready,
         .relativePath = "media/loss-1.mkv"},
        {.index = 2,
         .sourceId = sourceId(),
         .startTime = origin + 5200ms,
         .duration = 2s,
         .status = SegmentStatus::Ready,
         .relativePath = "media/loss-2.mkv"},
        {.index = 3,
         .sourceId = sourceId(),
         .startTime = origin + 7300ms,
         .duration = 2s,
         .status = SegmentStatus::Ready,
         .relativePath = "media/loss-3.mkv"},
    };
    for (const auto& segment : segments) {
        ASSERT_TRUE(session.addSegment(segment).hasValue());
    }
    ASSERT_TRUE(session.stop(origin + 9300ms).hasValue());
    ASSERT_TRUE(packages_.completeRecording(packageRoot_, session, fixedUtc())
                    .hasValue());

    CountingConcatMediaProbe probe;
    probe.concatDuration = 4100ms;
    RecordingTimelineReconciler reconciler{
        probe, [] { return std::string{"concat-loss-event"}; },
        [] { return fixedUtc(); }};

    const auto result = reconciler.reconcile(packageRoot_, concatSession);

    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_EQ(result.value().assetCount, 2U);
    ASSERT_EQ(probe.paths.size(), 2U);
    EXPECT_EQ(probe.paths[0].extension(), ".ffconcat");
    EXPECT_EQ(probe.paths[1].extension(), ".ffconcat");
}

TEST_F(RecordingTimelineReconcilerTest,
       ConcurrentSameSessionReconcileReturnsTwoSuccessfulOutcomes) {
    BarrierMediaProbe probe;
    std::atomic_uint64_t eventSequence{0};
    const auto run = [&] {
        RecordingTimelineReconciler reconciler{
            probe,
            [&eventSequence] {
                return "concurrent-event-" + std::to_string(
                    eventSequence.fetch_add(1, std::memory_order_relaxed));
            },
            [] { return fixedUtc(); }};
        return reconciler.reconcile(packageRoot_, sessionId());
    };
    auto firstFuture = std::async(std::launch::async, run);
    auto secondFuture = std::async(std::launch::async, run);

    const auto first = firstFuture.get();
    const auto second = secondFuture.get();

    ASSERT_TRUE(first.hasValue()) << first.error().message();
    ASSERT_TRUE(second.hasValue()) << second.error().message();
    EXPECT_NE(first.value().imported, second.value().imported);
    EXPECT_EQ(first.value().revision, 1);
    EXPECT_EQ(second.value().revision, 1);
    auto timelineStore = openTimelineStore();
    ASSERT_TRUE(timelineStore.hasValue());
    EXPECT_EQ(timelineStore.value().loadPrimaryTimeline().value().revision, 1);
    EXPECT_EQ(timelineStore.value().assets().value().size(), 1U);
}

TEST_F(RecordingTimelineReconcilerTest,
       IdentityLeaseFailureImmediatelyBeforeCommitPublishesNothing) {
    ExpiringLeaseMediaProbe probe;
    RecordingTimelineReconciler reconciler{
        probe, [] { return std::string{"lease-event"}; },
        [] { return fixedUtc(); }};

    const auto result = reconciler.reconcile(packageRoot_, sessionId());

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::IoFailure);
    auto timelineStore = openTimelineStore();
    ASSERT_TRUE(timelineStore.hasValue());
    const auto timeline = timelineStore.value().loadPrimaryTimeline();
    const auto assets = timelineStore.value().assets();
    ASSERT_TRUE(timeline.hasValue());
    ASSERT_TRUE(assets.hasValue());
    EXPECT_EQ(timeline.value().revision, 0);
    EXPECT_TRUE(timeline.value().events.empty());
    EXPECT_EQ(timeline.value().historyCount, 0U);
    EXPECT_EQ(timeline.value().historyCursor, 0U);
    EXPECT_TRUE(timeline.value().timeline.tracks().empty());
    EXPECT_TRUE(timeline.value().timeline.markers().empty());
    EXPECT_TRUE(assets.value().empty());
    auto studio = openStudioStore();
    ASSERT_TRUE(studio.hasValue());
    const auto import = studio.value().recordingImport(sessionId());
    ASSERT_TRUE(import.hasValue());
    EXPECT_FALSE(import.value().has_value());
}

}  // namespace
