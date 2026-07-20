#include "app/CursorRecordingBinding.h"

#include "app/ILiveRecordingEngine.h"
#include "app/IRecordingPersistence.h"
#include "cursor/ICursorSource.h"
#include "domain/RecordingSession.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using creator::app::CursorRecordingBinding;
using creator::app::ILiveRecordingEngine;
using creator::app::IRecordingPersistence;
using creator::app::LiveRecordingCompletion;
using creator::app::LiveRecordingEngineSnapshot;
using creator::app::LiveRecordingStart;
using creator::core::DurationNs;
using creator::core::Result;
using creator::core::TimestampNs;

class ImmediatePersistence final : public IRecordingPersistence {
public:
    void begin(const creator::domain::SessionId&, TimestampNs,
               Completion completion) override {
        completion(creator::core::ok());
    }

    void complete(const creator::domain::RecordingSession&,
                  Completion completion) override {
        completion(creator::core::ok());
    }

    void abort(const creator::domain::SessionId&, std::string,
               Completion completion) override {
        completion(creator::core::ok());
    }
};

class ImmediateEngine final : public ILiveRecordingEngine {
public:
    [[nodiscard]] bool available() const noexcept override { return true; }
    [[nodiscard]] std::string unavailableReason() const override { return {}; }

    [[nodiscard]] Result<void> start(LiveRecordingStart start,
                                     Completion completion) override {
        creator::domain::RecordingSession session{start.sessionId};
        auto started = session.start(start.startedAt);
        if (!started.hasValue()) return started.error();
        session_ = std::move(session);
        completion_ = std::move(completion);
        return creator::core::ok();
    }

    [[nodiscard]] Result<std::vector<creator::app::LiveCaptureSource>>
    sourceSnapshot() const override {
        return sources;
    }

    void stopAsync(TimestampNs stoppedAt) override {
        ASSERT_TRUE(session_.has_value());
        ASSERT_TRUE(session_->stop(stoppedAt).hasValue());
        auto completion = std::move(completion_);
        completion(LiveRecordingCompletion{.session = std::move(*session_)});
        session_.reset();
    }

    [[nodiscard]] LiveRecordingEngineSnapshot snapshot() const override {
        return {};
    }

    std::vector<creator::app::LiveCaptureSource> sources{
        {.sourceId = creator::domain::SourceId::create("screen-source").value(),
         .role = creator::recorder::TrackRole::Screen}};

private:
    std::optional<creator::domain::RecordingSession> session_;
    Completion completion_;
};

class ScriptedCursorSource final : public creator::cursor::ICursorSource {
public:
    ScriptedCursorSource(
        std::vector<creator::cursor::RawCursorSample> samples,
        std::shared_ptr<std::atomic<int>> destroyed)
        : samples_(std::move(samples)), destroyed_(std::move(destroyed)) {}

    ~ScriptedCursorSource() override { ++*destroyed_; }

    [[nodiscard]] std::optional<creator::cursor::RawCursorSample> poll() override {
        if (next_ == samples_.size()) return std::nullopt;
        return samples_[next_++];
    }

private:
    std::vector<creator::cursor::RawCursorSample> samples_;
    std::shared_ptr<std::atomic<int>> destroyed_;
    std::size_t next_{};
};

class CursorRecordingBindingTest : public ::testing::Test {
protected:
    void SetUp() override {
        package_ = std::filesystem::temp_directory_path() /
                   ("cs_cursor_recording_binding_" +
                    std::to_string(counter_++)) /
                   "project.cstudio";
        std::filesystem::create_directories(package_);
        auto engine = std::make_unique<ImmediateEngine>();
        engine_ = engine.get();
        controller_ = std::make_unique<creator::app::LiveRecordingController>(
            std::move(engine), &persistence_, [this] {
                return std::optional<std::filesystem::path>{package_};
            }, [this] { return now_; });
    }

    void TearDown() override {
        binding_.reset();
        controller_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(package_.parent_path(), ignored);
    }

    void bindScriptedSource() {
        binding_ = std::make_unique<CursorRecordingBinding>(
            *controller_,
            [destroyed = destroyed_](const LiveRecordingStart& start)
                -> Result<std::unique_ptr<creator::cursor::ICursorSource>> {
                std::vector<creator::cursor::RawCursorSample> samples{
                    creator::cursor::RawCursorMoveSample{
                        start.startedAt + DurationNs{100}, 50, 25, 100, 100},
                    creator::cursor::RawCursorClickSample{
                        start.startedAt + DurationNs{200}, 75, 50, 100, 100, 0}};
                return std::unique_ptr<creator::cursor::ICursorSource>{
                    new ScriptedCursorSource{std::move(samples), destroyed}};
            });
    }

    static std::vector<nlohmann::json> readLines(
        const std::filesystem::path& path) {
        std::vector<nlohmann::json> lines;
        std::ifstream input{path, std::ios::binary};
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(nlohmann::json::parse(line));
        }
        return lines;
    }

    static inline int counter_{};
    ImmediatePersistence persistence_;
    ImmediateEngine* engine_{};
    TimestampNs now_{DurationNs{10'000'000'000}};
    std::filesystem::path package_;
    std::shared_ptr<std::atomic<int>> destroyed_ =
        std::make_shared<std::atomic<int>>(0);
    std::unique_ptr<creator::app::LiveRecordingController> controller_;
    std::unique_ptr<CursorRecordingBinding> binding_;
};

TEST_F(CursorRecordingBindingTest,
       StartsWithMediaMapsToTakeTimeAndFinalizesOneDurableStream) {
    bindScriptedSource();

    controller_->startRecording();
    ASSERT_TRUE(controller_->isRecording());
    ASSERT_TRUE(binding_->active());
    binding_->poll();
    EXPECT_EQ(binding_->eventCount(), 2U);

    now_ += DurationNs{2'000'000'000};
    controller_->stopRecording();

    EXPECT_FALSE(binding_->active());
    EXPECT_EQ(destroyed_->load(), 1);
    ASSERT_FALSE(binding_->outputPath().isEmpty());
    const std::filesystem::path output{binding_->outputPath().toStdWString()};
    EXPECT_TRUE(std::filesystem::exists(output));
    EXPECT_FALSE(std::filesystem::exists(output.wstring() + L".part"));
    const auto lines = readLines(output);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0].at("tNs"), 100);
    EXPECT_EQ(lines[1].at("tNs"), 200);
    EXPECT_EQ(lines[0].at("sourceId"), "screen-source");
}

TEST_F(CursorRecordingBindingTest,
       EveryTakeOwnsAndReleasesExactlyOneNativeSource) {
    bindScriptedSource();

    controller_->startRecording();
    controller_->stopRecording();
    now_ += DurationNs{3'000'000'000};
    controller_->startRecording();
    controller_->stopRecording();

    EXPECT_EQ(destroyed_->load(), 2);
}

TEST_F(CursorRecordingBindingTest,
       CursorStartupFailureLeavesMediaRecordingActiveAndReportsFailure) {
    binding_ = std::make_unique<CursorRecordingBinding>(
        *controller_, [](const LiveRecordingStart&)
            -> Result<std::unique_ptr<creator::cursor::ICursorSource>> {
            return creator::core::AppError{
                creator::core::ErrorCode::IoFailure,
                "injected cursor registration failure"};
        });

    controller_->startRecording();

    EXPECT_TRUE(controller_->isRecording());
    EXPECT_FALSE(binding_->active());
    EXPECT_TRUE(binding_->statusMessage().contains(
        QStringLiteral("video/audio recording continues")));
    controller_->stopRecording();
}

TEST_F(CursorRecordingBindingTest,
       DestroyingBindingDuringRecordingReleasesTheNativeSource) {
    bindScriptedSource();
    controller_->startRecording();
    ASSERT_TRUE(binding_->active());

    binding_.reset();

    EXPECT_EQ(destroyed_->load(), 1);
    EXPECT_TRUE(controller_->isRecording());
    controller_->stopRecording();
}

TEST_F(CursorRecordingBindingTest,
       DoesNotRegisterRawInputWhenTheTakeHasNoScreenSource) {
    engine_->sources = {
        {.sourceId = creator::domain::SourceId::create("microphone-source").value(),
         .role = creator::recorder::TrackRole::Microphone}};
    int factoryCalls = 0;
    binding_ = std::make_unique<CursorRecordingBinding>(
        *controller_, [&factoryCalls](const LiveRecordingStart&)
            -> Result<std::unique_ptr<creator::cursor::ICursorSource>> {
            ++factoryCalls;
            return creator::core::AppError{
                creator::core::ErrorCode::InvalidState, "unexpected call"};
        });

    controller_->startRecording();

    EXPECT_EQ(factoryCalls, 0);
    EXPECT_FALSE(binding_->active());
    EXPECT_TRUE(binding_->statusMessage().contains(QStringLiteral("No screen")));
    controller_->stopRecording();
}

}  // namespace
