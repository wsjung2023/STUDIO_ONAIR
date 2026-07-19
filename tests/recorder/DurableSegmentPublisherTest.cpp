#include "recorder/DurableSegmentPublisher.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "recorder/RecordingTrack.h"
#include "recorder/TrackSegmentPorts.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using creator::core::AppError;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::domain::SegmentInfo;
using creator::domain::SegmentStatus;
using creator::domain::SourceId;
using creator::recorder::DurableSegmentPublisher;
using creator::recorder::EncodedSegment;
using creator::recorder::ISegmentFileOperations;
using creator::recorder::ISegmentLifecycleSink;
using creator::recorder::RecordingTrack;
using creator::recorder::TrackRole;

class FileOperationsFake final : public ISegmentFileOperations {
public:
    Result<void> prepare(const std::filesystem::path& part,
                         const std::filesystem::path& final) override {
        events->push_back("prepare");
        partPath = part;
        finalPath = final;
        if (prepareError) return *prepareError;
        return creator::core::ok();
    }
    Result<void> publish(const std::filesystem::path& part,
                         const std::filesystem::path& final) override {
        events->push_back("publish");
        EXPECT_EQ(part, partPath);
        EXPECT_EQ(final, finalPath);
        if (publishBeforeError) published = true;
        if (publishError) return *publishError;
        published = true;
        return creator::core::ok();
    }
    bool didPublishLastAttempt(const std::filesystem::path&,
                               const std::filesystem::path&) const noexcept override {
        events->push_back("inspect-published");
        return published;
    }

    std::shared_ptr<std::vector<std::string>> events;
    std::optional<AppError> prepareError;
    std::optional<AppError> publishError;
    std::filesystem::path partPath;
    std::filesystem::path finalPath;
    bool publishBeforeError{false};
    bool published{false};
};

class LifecycleSinkFake final : public ISegmentLifecycleSink {
public:
    Result<void> begin(const SegmentInfo& segment) override {
        events->push_back("begin");
        began = segment;
        if (beginError) return *beginError;
        return creator::core::ok();
    }
    Result<void> ready(const SegmentInfo& segment) override {
        events->push_back("ready");
        readied = segment;
        if (readyError) return *readyError;
        return creator::core::ok();
    }
    Result<void> failed(const SourceId&, std::uint64_t) override {
        events->push_back("failed");
        ++failedCalls;
        return creator::core::ok();
    }

    std::shared_ptr<std::vector<std::string>> events;
    std::optional<AppError> beginError;
    std::optional<AppError> readyError;
    std::optional<SegmentInfo> began;
    std::optional<SegmentInfo> readied;
    int failedCalls{0};
};

RecordingTrack screenTrack() {
    return RecordingTrack::create(SourceId::create("screen-1").value(), TrackRole::Screen)
        .value();
}

struct Fixture final {
    Fixture() {
        events = std::make_shared<std::vector<std::string>>();
        auto files = std::make_unique<FileOperationsFake>();
        filesRaw = files.get();
        filesRaw->events = events;
        auto sink = std::make_unique<LifecycleSinkFake>();
        sinkRaw = sink.get();
        sinkRaw->events = events;
        publisher = std::make_unique<DurableSegmentPublisher>(
            "project.cstudio", std::move(files), std::move(sink));
    }

    std::shared_ptr<std::vector<std::string>> events;
    FileOperationsFake* filesRaw{};
    LifecycleSinkFake* sinkRaw{};
    std::unique_ptr<DurableSegmentPublisher> publisher;
};

TEST(DurableSegmentPublisherTest, OrdersWritingBeforeBytesAndReadyAfterDurablePublish) {
    Fixture fixture;
    const auto start = creator::core::TimestampNs{std::chrono::seconds{4}};
    ASSERT_TRUE(fixture.publisher->begin(screenTrack(), 2, start).hasValue());
    const auto result = fixture.publisher->publish(
        {.endTime = creator::core::TimestampNs{std::chrono::seconds{6}},
         .bytesWritten = 12, .codecName = "mpeg4"});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(*fixture.events,
              (std::vector<std::string>{"prepare", "begin", "publish", "ready"}));
    ASSERT_TRUE(fixture.sinkRaw->began.has_value());
    EXPECT_EQ(fixture.sinkRaw->began->status, SegmentStatus::Writing);
    ASSERT_TRUE(fixture.sinkRaw->readied.has_value());
    EXPECT_EQ(fixture.sinkRaw->readied->status, SegmentStatus::Ready);
    EXPECT_EQ(fixture.sinkRaw->readied->duration, std::chrono::seconds{2});
    EXPECT_EQ(result.value(), *fixture.sinkRaw->readied);
}

TEST(DurableSegmentPublisherTest, PublishFailureMarksWritingRowFailedExactlyOnce) {
    Fixture fixture;
    fixture.filesRaw->publishError = AppError{ErrorCode::IoFailure, "rename failed"};
    ASSERT_TRUE(fixture.publisher->begin(screenTrack(), 0, {}).hasValue());

    const auto result = fixture.publisher->publish(
        {.endTime = creator::core::TimestampNs{std::chrono::seconds{2}},
         .bytesWritten = 1, .codecName = "mpeg4"});
    const auto duplicate = fixture.publisher->fail();

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message(), "rename failed");
    EXPECT_TRUE(duplicate.hasValue());
    EXPECT_EQ(fixture.sinkRaw->failedCalls, 1);
    EXPECT_EQ(*fixture.events,
              (std::vector<std::string>{"prepare", "begin", "publish",
                                        "inspect-published", "failed"}));
}

TEST(DurableSegmentPublisherTest, ReadyFailurePreservesWritingRowForRecovery) {
    Fixture fixture;
    fixture.sinkRaw->readyError = AppError{ErrorCode::IoFailure, "db unavailable"};
    ASSERT_TRUE(fixture.publisher->begin(screenTrack(), 0, {}).hasValue());

    const auto result = fixture.publisher->publish(
        {.endTime = creator::core::TimestampNs{std::chrono::seconds{2}},
         .bytesWritten = 1, .codecName = "mpeg4"});

    ASSERT_FALSE(result.hasValue());
    EXPECT_TRUE(fixture.filesRaw->published);
    EXPECT_EQ(fixture.sinkRaw->failedCalls, 0);
    EXPECT_FALSE(fixture.publisher->hasPendingSegment());
}

TEST(DurableSegmentPublisherTest, PostRenameDurabilityErrorPreservesWritingRowForRecovery) {
    Fixture fixture;
    fixture.filesRaw->publishBeforeError = true;
    fixture.filesRaw->publishError = AppError{ErrorCode::IoFailure, "directory sync failed"};
    ASSERT_TRUE(fixture.publisher->begin(screenTrack(), 0, {}).hasValue());

    const auto result = fixture.publisher->publish(
        {.endTime = creator::core::TimestampNs{std::chrono::seconds{2}},
         .bytesWritten = 1, .codecName = "mpeg4"});

    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().message(), "directory sync failed");
    EXPECT_EQ(fixture.sinkRaw->failedCalls, 0);
    EXPECT_FALSE(fixture.publisher->hasPendingSegment());
    EXPECT_EQ(*fixture.events,
              (std::vector<std::string>{"prepare", "begin", "publish",
                                        "inspect-published"}));
}

TEST(DurableSegmentPublisherTest, NativeOperationsPublishWithoutOverwriting) {
    const auto root = std::filesystem::temp_directory_path() /
                      "cs_durable_segment_publisher_test.cstudio";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    auto sink = std::make_unique<LifecycleSinkFake>();
    sink->events = std::make_shared<std::vector<std::string>>();
    DurableSegmentPublisher publisher{root, creator::recorder::makeSegmentFileOperations(root),
                                      std::move(sink)};
    auto paths = publisher.begin(screenTrack(), 0, {});
    ASSERT_TRUE(paths.hasValue()) << paths.error().message();
    {
        std::ofstream output{paths.value().partPath, std::ios::binary};
        output << "encoded-fixture";
    }
    ASSERT_TRUE(publisher.publish({.endTime = creator::core::TimestampNs{
                                      std::chrono::seconds{2}},
                                  .bytesWritten = 15,
                                  .codecName = "fixture"})
                    .hasValue());
    EXPECT_TRUE(std::filesystem::exists(paths.value().finalPath));
    EXPECT_FALSE(std::filesystem::exists(paths.value().partPath));

    auto secondSink = std::make_unique<LifecycleSinkFake>();
    secondSink->events = std::make_shared<std::vector<std::string>>();
    DurableSegmentPublisher second{root, creator::recorder::makeSegmentFileOperations(root),
                                   std::move(secondSink)};
    EXPECT_FALSE(second.begin(screenTrack(), 0, {}).hasValue());
    std::filesystem::remove_all(root, ignored);
}

}  // namespace
