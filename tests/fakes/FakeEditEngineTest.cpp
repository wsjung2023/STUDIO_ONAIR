#include "fakes/FakeEditEngine.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "edit_engine/EditEngineTypes.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace {

using creator::core::AppError;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TimelineRevision;
using creator::domain::TrackId;
using creator::edit_engine::RenderJobState;
using creator::edit_engine::RenderPreset;
using creator::edit_engine::RenderRequest;
using creator::edit_engine::TimelineChangeSet;
using creator::edit_engine::TimelineSnapshot;
using creator::fakes::FakeEditEngine;
using creator::fakes::FakeEditOperation;

TimelineSnapshot snapshot(std::int64_t revision, std::string name = "Main") {
    auto timeline = Timeline::create(
                        TimelineId::create("main").value(), std::move(name),
                        FrameRate::create(60, 1).value())
                        .value();
    return TimelineSnapshot{std::move(timeline),
                            TimelineRevision::create(revision).value()};
}

TEST(FakeEditEngineTest, RejectsOperationsBeforeLoad) {
    FakeEditEngine engine;
    auto preset = RenderPreset::create(
                      1920, 1080, FrameRate::create(60, 1).value(), 8'000'000,
                      192'000)
                      .value();

    EXPECT_FALSE(engine.play().hasValue());
    EXPECT_FALSE(engine.pause().hasValue());
    EXPECT_FALSE(engine.seek(TimestampNs{DurationNs{10}}).hasValue());
    EXPECT_FALSE(engine.requestFrame(TimestampNs{DurationNs{10}}).hasValue());
    EXPECT_FALSE(engine.render(RenderRequest::create(
                                   snapshot(1),
                                   std::filesystem::path{"D:/Exports/test.mp4"},
                                   preset)
                                   .value())
                     .hasValue());
}

TEST(FakeEditEngineTest, RecordsPlaybackAndReturnsDeterministicFrame) {
    FakeEditEngine engine;
    ASSERT_TRUE(engine.load(snapshot(3)).hasValue());
    ASSERT_TRUE(engine.play().hasValue());
    ASSERT_TRUE(engine.seek(TimestampNs{DurationNs{250}}).hasValue());
    auto frame = engine.requestFrame(TimestampNs{DurationNs{250}});
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    ASSERT_TRUE(engine.pause().hasValue());

    EXPECT_FALSE(engine.playing());
    EXPECT_EQ(engine.playhead().time_since_epoch().count(), 250);
    EXPECT_EQ(frame.value().revision().value(), 3);
    EXPECT_EQ(frame.value().frame().width, 64U);
    EXPECT_NE(frame.value().frame().platformHandle, nullptr);
    ASSERT_EQ(engine.calls().size(), 5U);
    EXPECT_EQ(engine.calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(engine.calls()[1].operation, FakeEditOperation::Play);
    EXPECT_EQ(engine.calls()[2].operation, FakeEditOperation::Seek);
    EXPECT_EQ(engine.calls()[3].operation, FakeEditOperation::RequestFrame);
    EXPECT_EQ(engine.calls()[4].operation, FakeEditOperation::Pause);
}

TEST(FakeEditEngineTest, AppliesOnlyChangeForCurrentlyLoadedRevision) {
    FakeEditEngine engine;
    ASSERT_TRUE(engine.load(snapshot(3)).hasValue());
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(3).value(), snapshot(4, "Edited"),
                      {TrackId::create("screen").value()}, false)
                      .value();
    ASSERT_TRUE(engine.update(change).hasValue());
    ASSERT_TRUE(engine.loadedSnapshot().has_value());
    EXPECT_EQ(engine.loadedSnapshot()->revision.value(), 4);
    EXPECT_EQ(engine.loadedSnapshot()->timeline.name(), "Edited");

    auto stale = TimelineChangeSet::create(
                     TimelineRevision::create(3).value(), snapshot(4),
                     {TrackId::create("screen").value()}, false)
                     .value();
    auto result = engine.update(stale);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidState);
    EXPECT_EQ(engine.loadedSnapshot()->timeline.name(), "Edited");
}

TEST(FakeEditEngineTest, InjectsOneShotFailureWithoutMutatingState) {
    FakeEditEngine engine;
    engine.failNext(FakeEditOperation::Load,
                    AppError{ErrorCode::IoFailure, "deterministic load failure"});

    auto failed = engine.load(snapshot(1));
    ASSERT_FALSE(failed.hasValue());
    EXPECT_EQ(failed.error().message(), "deterministic load failure");
    EXPECT_FALSE(engine.loadedSnapshot().has_value());

    ASSERT_TRUE(engine.load(snapshot(1)).hasValue());
    EXPECT_TRUE(engine.loadedSnapshot().has_value());
}

TEST(FakeEditEngineTest, ReturnsCancellableRenderJob) {
    FakeEditEngine engine;
    ASSERT_TRUE(engine.load(snapshot(5)).hasValue());
    auto preset = RenderPreset::create(
                      1920, 1080, FrameRate::create(60, 1).value(), 8'000'000,
                      192'000)
                      .value();
    auto request = RenderRequest::create(
                       snapshot(5), std::filesystem::path{"D:/Exports/test.mp4"},
                       preset)
                       .value();

    auto jobResult = engine.render(request);
    ASSERT_TRUE(jobResult.hasValue()) << jobResult.error().message();
    std::unique_ptr<creator::edit_engine::IRenderJob> job =
        std::move(jobResult).value();
    auto pending = job->progress();
    ASSERT_TRUE(pending.hasValue());
    EXPECT_EQ(pending.value().state(), RenderJobState::Pending);
    ASSERT_TRUE(job->cancel().hasValue());
    auto cancelled = job->progress();
    ASSERT_TRUE(cancelled.hasValue());
    EXPECT_EQ(cancelled.value().state(), RenderJobState::Cancelled);
    EXPECT_FALSE(job->cancel().hasValue());
}

}  // namespace
