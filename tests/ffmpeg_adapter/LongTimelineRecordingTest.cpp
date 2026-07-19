#include "app/MultiTrackRecordingService.h"
#include "app/ProjectSegmentLifecycleSink.h"
#include "project_store/ProjectPackageStore.h"
#include "recorder/AsyncTrackRecorder.h"
#include "recorder/DiskSpaceMonitor.h"
#include "recorder/DurableSegmentPublisher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;
using creator::app::MultiTrackRecordingService;
using creator::app::MultiTrackRecordingSummary;
using creator::app::ProjectSegmentLifecycleContext;
using creator::app::ProjectSegmentLifecycleSink;
using creator::core::Result;
using creator::domain::RecordingSession;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::project_store::ProjectPackageStore;
using creator::recorder::AsyncTrackRecorder;
using creator::recorder::AsyncTrackRecorderConfig;
using creator::recorder::DiskSpaceMonitor;
using creator::recorder::DurableSegmentPublisher;
using creator::recorder::EncodedSegment;
using creator::recorder::ITrackSegmentEncoder;
using creator::recorder::RecordingTrack;
using creator::recorder::SegmentEncodeConfig;
using creator::recorder::TrackRole;

class TinySegmentEncoder final : public ITrackSegmentEncoder {
public:
    Result<void> start(const SegmentEncodeConfig& config) override {
        path_ = config.partPath;
        std::ofstream output{path_, std::ios::binary};
        output << "segment\n";
        if (!output) {
            return creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "Could not write long-timeline fixture"};
        }
        return creator::core::ok();
    }
    Result<void> accept(const creator::media::VideoFrame&) override {
        return creator::core::ok();
    }
    Result<void> accept(const creator::media::AudioBlock&) override {
        return creator::core::ok();
    }
    Result<EncodedSegment> finish(creator::core::TimestampNs endTime) override {
        return EncodedSegment{.endTime = endTime,
                              .bytesWritten = 8,
                              .codecName = "timeline-fixture"};
    }
    void abort() noexcept override {}

private:
    fs::path path_;
};

std::unique_ptr<AsyncTrackRecorder> makeTimelineRecorder(
    const RecordingTrack& track, const fs::path& packagePath,
    std::shared_ptr<ProjectSegmentLifecycleContext> context) {
    auto publisher = std::make_unique<DurableSegmentPublisher>(
        packagePath, creator::recorder::makeSegmentFileOperations(packagePath),
        std::make_unique<ProjectSegmentLifecycleSink>(std::move(context)));
    AsyncTrackRecorderConfig config{
        .track = track,
        .packageRoot = packagePath,
        .recordingStartTime = {},
        .segmentDuration = std::chrono::seconds{2},
        .videoQueueCapacity = 901,
        .audioQueueFrameCapacity = 901,
        .nextSegmentEstimateBytes = 8,
    };
    return std::make_unique<AsyncTrackRecorder>(
        std::move(config), std::make_unique<TinySegmentEncoder>(),
        std::move(publisher),
        std::make_unique<DiskSpaceMonitor>(
            std::make_unique<creator::recorder::FilesystemDiskSpaceProbe>(), 0));
}

creator::media::VideoFrame videoAt(std::chrono::seconds timestamp) {
    return creator::media::VideoFrame{
        .timestamp = creator::core::TimestampNs{timestamp},
        .width = 1,
        .height = 1,
        .pixelFormat = creator::media::PixelFormat::Bgra8,
    };
}

creator::media::AudioBlock audioAt(std::chrono::seconds timestamp) {
    auto sample = std::shared_ptr<float[]>(new float[1]{});
    return creator::media::AudioBlock{
        .timestamp = creator::core::TimestampNs{timestamp},
        .sampleRate = 1,
        .channels = 1,
        .frameCount = 1,
        .samples = std::move(sample),
    };
}

TEST(LongTimelineRecordingTest, AcceleratedThirtyMinutesKeepsEveryTrackIndexAndFile) {
    constexpr std::uint64_t segmentsPerTrack = 900;
    const auto root = fs::temp_directory_path() / "cs_long_timeline_recording";
    const auto packagePath = root / "long.cstudio";
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root);

    auto store = std::make_shared<ProjectPackageStore>();
    ASSERT_TRUE(store->create(packagePath, "Long timeline").hasValue());
    const auto sessionId = SessionId::create("session-thirty-minutes").value();
    const auto createdAt =
        creator::core::Utc::parseRfc3339("2026-07-16T10:00:00Z").value();
    ASSERT_TRUE(store->beginRecording(packagePath, sessionId, {}, createdAt).hasValue());
    RecordingSession session{sessionId};
    ASSERT_TRUE(session.start({}).hasValue());
    auto context = ProjectSegmentLifecycleContext::create(
                       store, packagePath, std::move(session))
                       .value();
    const auto screen = SourceId::create("screen-1").value();
    const auto microphone = SourceId::create("microphone-1").value();
    MultiTrackRecordingService service;
    ASSERT_TRUE(service.addTrack(makeTimelineRecorder(
                    RecordingTrack::create(screen, TrackRole::Screen).value(),
                    packagePath, context))
                    .hasValue());
    ASSERT_TRUE(service.addTrack(makeTimelineRecorder(
                    RecordingTrack::create(microphone, TrackRole::Microphone).value(),
                    packagePath, context))
                    .hasValue());
    ASSERT_TRUE(service.start().hasValue());

    for (std::uint64_t index = 0; index < segmentsPerTrack; ++index) {
        const auto timestamp = std::chrono::seconds{static_cast<std::int64_t>(index * 2)};
        ASSERT_TRUE(service.accept(screen, videoAt(timestamp)).hasValue());
        ASSERT_TRUE(service.accept(microphone, audioAt(timestamp)).hasValue());
    }
    auto promise = std::make_shared<std::promise<Result<MultiTrackRecordingSummary>>>();
    auto future = promise->get_future();
    const auto endTime =
        creator::core::TimestampNs{std::chrono::minutes{30}};
    service.stopAsync(endTime,
                      [promise](const auto& result) { promise->set_value(result); });
    const auto stopped = future.get();
    ASSERT_TRUE(stopped.hasValue()) << stopped.error().message();

    const auto snapshot = context->sessionSnapshot();
    ASSERT_EQ(snapshot.segmentCount(), segmentsPerTrack * 2);
    std::map<std::string, std::set<std::uint64_t>> indices;
    std::set<std::string> indexedFiles;
    for (const auto& segment : snapshot.segments()) {
        indices[segment.sourceId.value()].insert(segment.index);
        indexedFiles.insert(segment.relativePath);
        EXPECT_TRUE(fs::is_regular_file(packagePath / fs::path{segment.relativePath}));
    }
    ASSERT_EQ(indices[screen.value()].size(), segmentsPerTrack);
    ASSERT_EQ(indices[microphone.value()].size(), segmentsPerTrack);
    for (std::uint64_t index = 0; index < segmentsPerTrack; ++index) {
        EXPECT_TRUE(indices[screen.value()].contains(index));
        EXPECT_TRUE(indices[microphone.value()].contains(index));
    }

    std::set<std::string> finalFiles;
    std::size_t temporaryFiles = 0;
    for (fs::recursive_directory_iterator iterator{packagePath};
         iterator != fs::recursive_directory_iterator{}; ++iterator) {
        if (!iterator->is_regular_file()) continue;
        if (iterator->path().extension() == ".mkv" ||
            iterator->path().extension() == ".mka") {
            finalFiles.insert(fs::relative(iterator->path(), packagePath).generic_string());
        }
        if (iterator->path().extension() == ".part") ++temporaryFiles;
    }
    EXPECT_EQ(finalFiles, indexedFiles);
    EXPECT_EQ(temporaryFiles, 0u);
    const auto opened = store->open(packagePath);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    ASSERT_EQ(opened.value().recoveryCandidates.size(), 1u);
    EXPECT_EQ(opened.value().recoveryCandidates.front().readySegments,
              segmentsPerTrack * 2);
    EXPECT_EQ(opened.value().recoveryCandidates.front().writingSegments, 0u);

    const auto finishedAt =
        creator::core::Utc::parseRfc3339("2026-07-16T10:30:00Z").value();
    const auto completed = context->complete(endTime, finishedAt);
    ASSERT_TRUE(completed.hasValue()) << completed.error().message();
    ASSERT_TRUE(completed.value().duration().has_value());
    EXPECT_EQ(*completed.value().duration(), std::chrono::minutes{30});

    fs::remove_all(root, ignored);
}

}  // namespace
