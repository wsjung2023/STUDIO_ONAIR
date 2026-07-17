#include "app/FfmpegLiveRecordingEngine.h"

#include "app/MultiTrackRecordingService.h"
#include "app/ProjectSegmentLifecycleSink.h"
#include "core/AppError.h"
#include "domain/RecordingSession.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"
#include "ffmpeg_adapter/FfmpegAudioSegmentEncoder.h"
#include "ffmpeg_adapter/FfmpegCapabilityProbe.h"
#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"
#include "recorder/AsyncTrackRecorder.h"
#include "recorder/DiskSpaceMonitor.h"
#include "recorder/DurableSegmentPublisher.h"
#include "sync/ClockCoordinator.h"
#include "sync/VideoSyncPlanner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace creator::app {
namespace {

using recorder::AsyncTrackRecorder;
using recorder::AsyncTrackRecorderConfig;
using recorder::DiskSpaceMonitor;
using recorder::DurableSegmentPublisher;
using recorder::RecordingTrack;
using recorder::TrackMediaKind;

class VideoRouter final : public capture::IVideoFrameSink {
public:
    VideoRouter(std::weak_ptr<FfmpegLiveRecordingEngine::Run> run,
                domain::SourceId sourceId,
                std::unique_ptr<synchronization::VideoSyncPlanner> planner)
        : run_(std::move(run)), sourceId_(std::move(sourceId)),
          planner_(std::move(planner)) {}

    void onCaptureStarted() noexcept override {}
    void onVideoFrame(media::VideoFrame frame) noexcept override;
    void onCaptureError(core::AppError error) noexcept override;
    [[nodiscard]] synchronization::VideoSyncSnapshot syncSnapshot() const noexcept;

private:
    std::weak_ptr<FfmpegLiveRecordingEngine::Run> run_;
    domain::SourceId sourceId_;
    mutable std::mutex plannerMutex_;
    std::unique_ptr<synchronization::VideoSyncPlanner> planner_;
};

class AudioRouter final : public capture::IAudioBlockSink {
public:
    AudioRouter(std::weak_ptr<FfmpegLiveRecordingEngine::Run> run,
                domain::SourceId sourceId)
        : run_(std::move(run)), sourceId_(std::move(sourceId)) {}

    void onCaptureStarted() noexcept override {}
    void onAudioBlock(media::AudioBlock block) noexcept override;
    void onCaptureError(core::AppError error) noexcept override;

private:
    std::weak_ptr<FfmpegLiveRecordingEngine::Run> run_;
    domain::SourceId sourceId_;
};

std::unique_ptr<recorder::IVideoFrameMapper> makeVideoMapper() {
#if defined(__APPLE__)
    return std::make_unique<ffmpeg_adapter::MacCvPixelBufferFrameMapper>();
#else
    return std::make_unique<ffmpeg_adapter::CpuBgraFrameMapper>();
#endif
}

core::Result<std::unique_ptr<AsyncTrackRecorder>> makeTrackRecorder(
    RecordingTrack track, const LiveRecordingStart& start,
    std::shared_ptr<ProjectSegmentLifecycleContext> context) {
    std::unique_ptr<recorder::ITrackSegmentEncoder> encoder;
    if (track.mediaKind() == TrackMediaKind::Video) {
        encoder = std::make_unique<ffmpeg_adapter::FfmpegVideoSegmentEncoder>(
            makeVideoMapper());
    } else {
        encoder =
            std::make_unique<ffmpeg_adapter::FfmpegAudioSegmentEncoder>();
    }
    auto publisher = std::make_unique<DurableSegmentPublisher>(
        start.packagePath, recorder::makeSegmentFileOperations(start.packagePath),
        std::make_unique<ProjectSegmentLifecycleSink>(std::move(context)));
    AsyncTrackRecorderConfig config{
        .track = std::move(track),
        .packageRoot = start.packagePath,
        .recordingStartTime = start.startedAt,
        .segmentDuration = std::chrono::seconds{2},
        .videoQueueCapacity = 3,
        .audioQueueFrameCapacity = 240'000,
        .nextSegmentEstimateBytes = 64ULL * 1024ULL * 1024ULL,
    };
    auto diskMonitor = std::make_unique<DiskSpaceMonitor>();
    auto recorder = std::make_unique<AsyncTrackRecorder>(
        std::move(config), std::move(encoder), std::move(publisher),
        std::move(diskMonitor));
    return recorder;
}

std::uint32_t clockPriority(recorder::TrackRole role,
                            std::size_t sourceOrder) noexcept {
    switch (role) {
        case recorder::TrackRole::Microphone:
            return 0;
        case recorder::TrackRole::SystemAudio:
            return 1;
        default:
            constexpr auto maximum =
                static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
            return sourceOrder > maximum - 2
                       ? std::numeric_limits<std::uint32_t>::max()
                       : static_cast<std::uint32_t>(sourceOrder + 2);
    }
}

core::DurationNs videoPeriod(recorder::TrackRole role) noexcept {
    if (role == recorder::TrackRole::Screen) {
        return std::chrono::nanoseconds{1'000'000'000LL / 60LL};
    }
    return std::chrono::nanoseconds{1'000'000'000LL / 30LL};
}

}  // namespace

struct FfmpegLiveRecordingEngine::Run final {
    std::shared_ptr<ILiveCaptureBindings> bindings;
    std::shared_ptr<MultiTrackRecordingService> service;
    std::shared_ptr<ProjectSegmentLifecycleContext> context;
    std::unique_ptr<synchronization::ClockCoordinator> coordinator;
    Completion completion;
    core::TimestampNs startedAt{};
    std::mutex endMutex;
    core::TimestampNs stoppedAt{};
    std::atomic<bool> accepting{false};
    std::atomic<bool> delivered{false};
    std::vector<std::shared_ptr<VideoRouter>> videoSinks;
    std::vector<std::shared_ptr<capture::IAudioBlockSink>> audioSinks;

    void fail(core::AppError error, core::TimestampNs at) noexcept {
        if (!accepting.exchange(false, std::memory_order_acq_rel)) return;
        {
            std::lock_guard lock{endMutex};
            stoppedAt = std::max(stoppedAt, at);
        }
        service->fail(std::move(error), at);
    }
};

namespace {

void VideoRouter::onVideoFrame(media::VideoFrame frame) noexcept {
    const auto run = run_.lock();
    if (!run || !run->accepting.load(std::memory_order_acquire)) return;
    const auto observedAt = core::ProjectClock::now();
    const auto correction =
        run->coordinator->observe(sourceId_, frame.timestamp, observedAt);
    if (!correction.hasValue()) {
        run->fail(correction.error(), observedAt);
        return;
    }
    auto planned = [&] {
        std::lock_guard lock{plannerMutex_};
        return planner_->plan(std::move(frame),
                              correction.value().correctedTimestamp);
    }();
    if (!planned.hasValue()) {
        run->fail(planned.error(), observedAt);
        return;
    }
    for (auto& synchronizedFrame : planned.value().frames) {
        if (auto accepted =
                run->service->accept(sourceId_, std::move(synchronizedFrame));
            !accepted.hasValue()) {
            run->fail(accepted.error(), observedAt);
            return;
        }
    }
}

synchronization::VideoSyncSnapshot VideoRouter::syncSnapshot() const noexcept {
    std::lock_guard lock{plannerMutex_};
    return planner_->snapshot();
}

void VideoRouter::onCaptureError(core::AppError error) noexcept {
    if (const auto run = run_.lock()) {
        run->fail(std::move(error), core::ProjectClock::now());
    }
}

void AudioRouter::onAudioBlock(media::AudioBlock block) noexcept {
    const auto run = run_.lock();
    if (!run || !run->accepting.load(std::memory_order_acquire)) return;
    const auto observedAt = core::ProjectClock::now();
    const auto correction =
        run->coordinator->observe(sourceId_, block.timestamp, observedAt);
    if (!correction.hasValue()) {
        run->fail(correction.error(), observedAt);
        return;
    }
    block.timestamp = correction.value().correctedTimestamp;
    block.sampleRateRatio = correction.value().audioRateRatio;
    if (auto accepted = run->service->accept(sourceId_, std::move(block));
        !accepted.hasValue()) {
        run->fail(accepted.error(), observedAt);
    }
}

void AudioRouter::onCaptureError(core::AppError error) noexcept {
    if (const auto run = run_.lock()) {
        run->fail(std::move(error), core::ProjectClock::now());
    }
}

LiveRecordingEngineSnapshot snapshotOf(
    const std::shared_ptr<FfmpegLiveRecordingEngine::Run>& run) {
    if (!run) return {};
    const auto serviceSnapshot = run->service->snapshot();
    LiveRecordingEngineSnapshot result;
    result.trackCount = serviceSnapshot.tracks.size();
    result.terminalError = serviceSnapshot.terminalError;
    std::set<std::string> encoders;
    for (const auto& observed : serviceSnapshot.tracks) {
        const auto& track = observed.snapshot;
        result.queuedItems += static_cast<std::uint64_t>(track.queuedVideoFrames) +
                              track.queuedAudioFrames;
        result.videoFramesDropped += track.summary.videoFramesDropped;
        result.segmentsPublished += track.summary.segmentsPublished;
        if (track.diskSpace) {
            result.availableDiskBytes = result.availableDiskBytes
                                            ? std::min(*result.availableDiskBytes,
                                                       track.diskSpace->availableBytes)
                                            : track.diskSpace->availableBytes;
        }
        if (!track.encoderName.empty()) encoders.insert(track.encoderName);
    }
    const auto clockSnapshot = run->coordinator->snapshot();
    for (const auto& source : clockSnapshot.sources) {
        result.maximumAbsoluteDriftNanoseconds = std::max(
            result.maximumAbsoluteDriftNanoseconds,
            static_cast<std::uint64_t>(source.maximumAbsoluteDrift.count()));
        if (source.mediaKind == synchronization::SyncMediaKind::Audio) {
            result.audioCorrectionPpm = std::max(
                result.audioCorrectionPpm, std::abs(source.rateCorrectionPpm));
        }
    }
    for (const auto& sink : run->videoSinks) {
        const auto sync = sink->syncSnapshot();
        result.syncVideoFramesDropped += sync.framesDropped;
        result.syncVideoFramesDuplicated += sync.framesDuplicated;
    }
    for (const auto& encoder : encoders) {
        if (!result.encoderName.empty()) result.encoderName += ", ";
        result.encoderName += encoder;
    }
    return result;
}

void finishRun(const std::shared_ptr<FfmpegLiveRecordingEngine::Run>& run,
               const core::Result<MultiTrackRecordingSummary>& result) {
    run->accepting.store(false, std::memory_order_release);
    run->bindings->dispatch([run, result] {
        if (run->delivered.exchange(true, std::memory_order_acq_rel)) return;
        run->bindings->detachAll();
        auto session = run->context->sessionSnapshot();
        core::TimestampNs stoppedAt;
        {
            std::lock_guard lock{run->endMutex};
            stoppedAt = std::max(run->stoppedAt, run->startedAt);
        }
        for (const auto& segment : session.segments()) {
            stoppedAt = std::max(stoppedAt, segment.startTime + segment.duration);
        }
        if (auto stopped = session.stop(stoppedAt); !stopped.hasValue()) {
            run->completion(stopped.error());
            return;
        }
        const auto snapshot = snapshotOf(run);
        run->completion(LiveRecordingCompletion{
            .session = std::move(session),
            .trackCount = snapshot.trackCount,
            .segmentsPublished = snapshot.segmentsPublished,
            .videoFramesDropped = snapshot.videoFramesDropped,
            .terminalError = result.hasValue()
                                 ? std::optional<core::AppError>{}
                                 : std::optional<core::AppError>{result.error()},
        });
    });
}

}  // namespace

FfmpegLiveRecordingEngine::FfmpegLiveRecordingEngine(
    std::shared_ptr<ILiveCaptureBindings> captureBindings,
    std::shared_ptr<project_store::IProjectPackageStore> store)
    : captureBindings_(std::move(captureBindings)), store_(std::move(store)) {
    if (!captureBindings_ || !store_) {
        capabilityStatus_ = core::AppError{
            core::ErrorCode::InvalidArgument,
            "Live recording engine dependencies are incomplete"};
        return;
    }
    auto capabilities = ffmpeg_adapter::probeFfmpegCapabilities();
    if (!capabilities.hasValue()) {
        capabilityStatus_ = capabilities.error();
        return;
    }
    const auto hasEncoder = [&capabilities](std::string_view name) {
        const auto found = std::find_if(
            capabilities.value().encoders.begin(),
            capabilities.value().encoders.end(),
            [name](const auto& encoder) {
                return encoder.name == name && encoder.available;
            });
        return found != capabilities.value().encoders.end();
    };
    const bool hasVideo = hasEncoder("h264_videotoolbox") || hasEncoder("h264_mf") ||
                          hasEncoder("h264_nvenc") || hasEncoder("h264_qsv") ||
                          hasEncoder("mpeg4");
    if (!hasVideo || !hasEncoder("aac")) {
        capabilityStatus_ = core::AppError{
            core::ErrorCode::UnsupportedVersion,
            "Audited FFmpeg runtime lacks a required video or AAC encoder"};
    }
}

FfmpegLiveRecordingEngine::~FfmpegLiveRecordingEngine() {
    std::shared_ptr<Run> run;
    {
        std::lock_guard lock{mutex_};
        run = std::move(run_);
    }
    if (!run) return;
    run->accepting.store(false, std::memory_order_release);
    run->bindings->detachAll();
    run->service->observeCompletion({});
    run->service->stopAsync(core::ProjectClock::now());
}

bool FfmpegLiveRecordingEngine::available() const noexcept {
    return capabilityStatus_.hasValue();
}

std::string FfmpegLiveRecordingEngine::unavailableReason() const {
    return capabilityStatus_.hasValue() ? std::string{} : capabilityStatus_.error().message();
}

core::Result<std::vector<LiveCaptureSource>>
FfmpegLiveRecordingEngine::sourceSnapshot() const {
    if (!capabilityStatus_.hasValue()) return capabilityStatus_.error();
    if (!captureBindings_) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Live capture bindings are unavailable"};
    }
    auto sources = captureBindings_->activeSources();
    if (sources.empty()) {
        return core::AppError{
            core::ErrorCode::InvalidState,
            "Start at least one capture source before recording"};
    }
    return sources;
}

core::Result<void> FfmpegLiveRecordingEngine::start(
    LiveRecordingStart start, Completion completion) {
    if (!capabilityStatus_.hasValue()) return capabilityStatus_.error();
    if (!completion) {
        return core::AppError{core::ErrorCode::InvalidArgument,
                              "Live recording requires a completion callback"};
    }
    {
        std::lock_guard lock{mutex_};
        if (run_ && run_->service->snapshot().state != MultiTrackRecordingState::Stopped) {
            return core::AppError{core::ErrorCode::InvalidState,
                                  "Live recording is already active"};
        }
        run_.reset();
    }

    auto session = domain::RecordingSession{start.sessionId};
    if (auto started = session.start(start.startedAt); !started.hasValue()) {
        return started.error();
    }
    auto contextResult = ProjectSegmentLifecycleContext::create(
        store_, start.packagePath, std::move(session));
    if (!contextResult.hasValue()) return contextResult.error();
    const auto& sources = start.sources;
    if (sources.empty()) {
        return core::AppError{core::ErrorCode::InvalidState,
                              "Start at least one capture source before recording"};
    }

    std::vector<synchronization::ClockSourceConfig> clockSources;
    clockSources.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto track = RecordingTrack::create(sources[index].sourceId,
                                                  sources[index].role);
        if (!track.hasValue()) return track.error();
        clockSources.push_back(synchronization::ClockSourceConfig{
            .sourceId = sources[index].sourceId,
            .mediaKind = track.value().mediaKind() == TrackMediaKind::Audio
                             ? synchronization::SyncMediaKind::Audio
                             : synchronization::SyncMediaKind::Video,
            .masterPriority = clockPriority(sources[index].role, index),
        });
    }
    auto coordinator =
        synchronization::ClockCoordinator::create(std::move(clockSources));
    if (!coordinator.hasValue()) return coordinator.error();

    auto run = std::make_shared<Run>();
    run->bindings = captureBindings_;
    run->service = std::make_shared<MultiTrackRecordingService>();
    run->context = std::move(contextResult).value();
    run->coordinator = std::move(coordinator).value();
    run->completion = std::move(completion);
    run->startedAt = start.startedAt;
    run->stoppedAt = start.startedAt;

    struct PendingAttachment final {
        LiveCaptureSource source;
        std::shared_ptr<capture::IVideoFrameSink> video;
        std::shared_ptr<capture::IAudioBlockSink> audio;
    };
    std::vector<PendingAttachment> pendingAttachments;
    pendingAttachments.reserve(sources.size());
    for (const auto& source : sources) {
        auto track = RecordingTrack::create(source.sourceId, source.role);
        if (!track.hasValue()) return track.error();
        auto recordingTrack = std::move(track).value();
        const auto mediaKind = recordingTrack.mediaKind();
        auto recorder = makeTrackRecorder(std::move(recordingTrack), start, run->context);
        if (!recorder.hasValue()) return recorder.error();
        auto added = run->service->addTrack(std::move(recorder).value());
        if (!added.hasValue()) {
            return added.error();
        }
        if (mediaKind == TrackMediaKind::Video) {
            auto planner = synchronization::VideoSyncPlanner::create(
                videoPeriod(source.role));
            if (!planner.hasValue()) return planner.error();
            auto sink = std::make_shared<VideoRouter>(
                run, source.sourceId, std::move(planner).value());
            run->videoSinks.push_back(sink);
            pendingAttachments.push_back(
                PendingAttachment{.source = source, .video = std::move(sink)});
        } else {
            auto sink = std::make_shared<AudioRouter>(run, source.sourceId);
            run->audioSinks.push_back(sink);
            pendingAttachments.push_back(
                PendingAttachment{.source = source, .audio = std::move(sink)});
        }
    }
    for (const auto& pending : pendingAttachments) {
        if (auto attached = captureBindings_->attach(
                pending.source, pending.video, pending.audio);
            !attached.hasValue()) {
            captureBindings_->detachAll();
            return attached.error();
        }
    }
    if (auto started = run->service->start(); !started.hasValue()) {
        captureBindings_->detachAll();
        return started.error();
    }
    run->service->observeCompletion(
        [weak = std::weak_ptr<Run>{run}](const auto& result) {
            if (const auto active = weak.lock()) finishRun(active, result);
        });
    run->accepting.store(true, std::memory_order_release);
    {
        std::lock_guard lock{mutex_};
        run_ = std::move(run);
    }
    return core::ok();
}

void FfmpegLiveRecordingEngine::stopAsync(core::TimestampNs stoppedAt) {
    std::shared_ptr<Run> run;
    {
        std::lock_guard lock{mutex_};
        run = run_;
    }
    if (!run) return;
    run->accepting.store(false, std::memory_order_release);
    {
        std::lock_guard lock{run->endMutex};
        run->stoppedAt = std::max(run->stoppedAt, stoppedAt);
    }
    run->bindings->detachAll();
    run->service->stopAsync(stoppedAt);
}

LiveRecordingEngineSnapshot FfmpegLiveRecordingEngine::snapshot() const {
    std::shared_ptr<Run> run;
    {
        std::lock_guard lock{mutex_};
        run = run_;
    }
    return snapshotOf(run);
}

}  // namespace creator::app
