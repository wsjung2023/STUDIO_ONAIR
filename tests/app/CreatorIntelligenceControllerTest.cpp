#include "app/CreatorIntelligenceController.h"

#include "core/AppError.h"
#include "domain/Identifiers.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "transcription/FakeTranscriptionProvider.h"

#include <QCoreApplication>
#include <QTest>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

using namespace creator;

edit_engine::TimelineSnapshot emptySnapshot(std::int64_t revision = 7) {
    auto rate = core::FrameRate::create(30, 1).value();
    auto timeline = domain::Timeline::create(
                        domain::TimelineId::create("main").value(), "Main", rate)
                        .value();
    return {std::move(timeline),
            domain::TimelineRevision::create(revision).value(), {}, {}};
}

TEST(CreatorIntelligenceControllerTest,
     ProposesWithoutMutationThenAppliesOnlyApprovedArtifacts) {
    std::atomic_int transcriptApprovals{};
    std::atomic_int cutApprovals{};
    app::CreatorIntelligenceController controller{
        std::make_unique<transcription::FakeTranscriptionProvider>(),
        [](const edit_engine::TimelineSnapshot&, std::stop_token)
            -> core::Result<std::vector<float>> {
            return std::vector<float>(32'000, 0.0F);
        },
        [] { return std::optional{emptySnapshot()}; },
        [&transcriptApprovals](const transcription::Transcript&,
                               std::int64_t expectedRevision) {
            EXPECT_EQ(expectedRevision, 7);
            transcriptApprovals.fetch_add(1, std::memory_order_relaxed);
            return core::ok();
        },
        [&cutApprovals](const domain::TimeRange& range,
                        std::int64_t expectedRevision) {
            EXPECT_EQ(expectedRevision, 7);
            EXPECT_GT(range.duration().count(), 0);
            cutApprovals.fetch_add(1, std::memory_order_relaxed);
            return core::ok();
        }};

    ASSERT_TRUE(controller.analyzeProject());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5'000);
    ASSERT_TRUE(controller.hasPendingProposal());
    EXPECT_FALSE(controller.transcriptProposal().empty());
    ASSERT_FALSE(controller.cutSuggestions().empty());
    EXPECT_EQ(transcriptApprovals.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(cutApprovals.load(std::memory_order_relaxed), 0);

    EXPECT_TRUE(controller.approveTranscript());
    EXPECT_EQ(transcriptApprovals.load(std::memory_order_relaxed), 1);
    EXPECT_TRUE(controller.approveCut(0));
    EXPECT_EQ(cutApprovals.load(std::memory_order_relaxed), 1);
}

TEST(CreatorIntelligenceControllerTest, CancelDiscardsWorkerResult) {
    std::atomic_bool loaderEntered{};
    app::CreatorIntelligenceController controller{
        std::make_unique<transcription::FakeTranscriptionProvider>(),
        [&loaderEntered](const edit_engine::TimelineSnapshot&, std::stop_token stop)
            -> core::Result<std::vector<float>> {
            loaderEntered.store(true, std::memory_order_relaxed);
            while (!stop.stop_requested()) {
                std::this_thread::yield();
            }
            return core::AppError{core::ErrorCode::InvalidState, "cancelled"};
        },
        [] { return std::optional{emptySnapshot()}; },
        [](const transcription::Transcript&, std::int64_t) {
            return core::ok();
        },
        [](const domain::TimeRange&, std::int64_t) { return core::ok(); }};

    ASSERT_TRUE(controller.analyzeProject());
    QTRY_VERIFY_WITH_TIMEOUT(loaderEntered.load(std::memory_order_relaxed),
                             2'000);
    controller.cancelAnalysis();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5'000);
    EXPECT_FALSE(controller.hasPendingProposal());
    EXPECT_TRUE(controller.statusMessage().contains("cancel", Qt::CaseInsensitive));
}

TEST(CreatorIntelligenceControllerTest, FailureNeverCallsApprovalCallbacks) {
    std::atomic_int mutations{};
    app::CreatorIntelligenceController controller{
        std::make_unique<transcription::FakeTranscriptionProvider>(),
        [](const edit_engine::TimelineSnapshot&, std::stop_token)
            -> core::Result<std::vector<float>> {
            return core::AppError{core::ErrorCode::IoFailure, "decode failed"};
        },
        [] { return std::optional{emptySnapshot()}; },
        [&mutations](const transcription::Transcript&, std::int64_t) {
            mutations.fetch_add(1, std::memory_order_relaxed);
            return core::ok();
        },
        [&mutations](const domain::TimeRange&, std::int64_t) {
            mutations.fetch_add(1, std::memory_order_relaxed);
            return core::ok();
        }};

    ASSERT_TRUE(controller.analyzeProject());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 5'000);
    EXPECT_FALSE(controller.hasPendingProposal());
    EXPECT_EQ(mutations.load(std::memory_order_relaxed), 0);
    EXPECT_TRUE(controller.statusMessage().contains("decode failed"));
}

}  // namespace
