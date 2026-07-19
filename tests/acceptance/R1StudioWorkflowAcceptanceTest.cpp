#include "app/EditorSessionWorker.h"
#include "app/FfmpegLiveRecordingEngine.h"
#include "app/ILiveCaptureBindings.h"
#include "app/RecordingTimelineReconciler.h"
#include "app/StudioWorkflowController.h"
#include "core/Utc.h"
#include "domain/StudioScene.h"
#include "domain/RecordingSession.h"
#include "domain/Timeline.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"
#include "ffmpeg_adapter/FfmpegAudioSegmentEncoder.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "ffmpeg_adapter/FfmpegVideoSegmentEncoder.h"
#include "mlt_adapter/MltEditEngine.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteProjectDatabase.h"
#include "project_store/SqliteStudioStore.h"
#include "project_store/SqliteTimelineStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>
#include <QUrl>

#include <gtest/gtest.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using creator::app::EditorSessionResultPtr;
using creator::app::EditorSessionState;
using creator::app::EditorSessionWorker;
using creator::app::FfmpegLiveRecordingEngine;
using creator::app::ILiveCaptureBindings;
using creator::app::IRecordingTimelineReconciler;
using creator::app::LiveCaptureSource;
using creator::app::LiveRecordingCompletion;
using creator::app::LiveRecordingStart;
using creator::app::RecordingReconcileResult;
using creator::app::RecordingTimelineReconciler;
using creator::app::StudioWorkflowController;
using creator::core::AppError;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::core::TimestampNs;
using creator::domain::SceneId;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::domain::StudioSourceRole;
using creator::ffmpeg_adapter::CpuBgraFrameBuffer;
using creator::project_store::IStudioStore;
using creator::project_store::ProjectPackageStore;
using creator::project_store::RecordingMarker;
using creator::project_store::RecordingSourceRole;
using creator::project_store::SqliteStudioStore;
using creator::recorder::TrackRole;

creator::core::Utc utc(std::string_view value) {
    return creator::core::Utc::parseRfc3339(value).value();
}

TimestampNs at(std::int64_t nanoseconds) {
    return TimestampNs{DurationNs{nanoseconds}};
}

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(1ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

class PhysicalRoot final {
public:
    explicit PhysicalRoot(std::string suffix) {
        path_ = fs::temp_directory_path() /
                fs::path{u8"creator-studio-r1-06-검증"} /
                (std::to_string(QCoreApplication::applicationPid()) + "-" +
                 std::move(suffix));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_);
    }

    ~PhysicalRoot() {
        std::error_code error;
        fs::remove_all(path_, error);
        EXPECT_FALSE(error) << error.message();
        EXPECT_FALSE(fs::exists(path_));
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

class CaptureBindings final : public ILiveCaptureBindings {
public:
    [[nodiscard]] std::vector<LiveCaptureSource> activeSources() const override {
        return sources;
    }

    [[nodiscard]] Result<void> attach(
        const LiveCaptureSource& source,
        std::shared_ptr<creator::capture::IVideoFrameSink> videoSink,
        std::shared_ptr<creator::capture::IAudioBlockSink> audioSink) override {
        if (source.role == TrackRole::Screen || source.role == TrackRole::Camera) {
            if (!videoSink) {
                return AppError{ErrorCode::InvalidArgument,
                                "physical video sink is missing"};
            }
            videos[source.sourceId.value()] = std::move(videoSink);
        } else {
            if (!audioSink) {
                return AppError{ErrorCode::InvalidArgument,
                                "physical audio sink is missing"};
            }
            audios[source.sourceId.value()] = std::move(audioSink);
        }
        return creator::core::ok();
    }

    void detachAll() noexcept override {
        ++detachCalls;
        videos.clear();
        audios.clear();
    }

    void dispatch(std::function<void()> work) override { work(); }

    std::vector<LiveCaptureSource> sources;
    std::unordered_map<std::string,
                       std::shared_ptr<creator::capture::IVideoFrameSink>>
        videos;
    std::unordered_map<std::string,
                       std::shared_ptr<creator::capture::IAudioBlockSink>>
        audios;
    int detachCalls{};
};

creator::media::VideoFrame videoFrame(std::uint32_t index, bool camera) {
    constexpr std::uint32_t width = 320;
    constexpr std::uint32_t height = 180;
    auto buffer = CpuBgraFrameBuffer::create(width, height).value();
    for (std::size_t offset = 0; offset < buffer->size(); offset += 4U) {
        buffer->data()[offset] = camera ? 230U : 12U;
        buffer->data()[offset + 1U] = 20U;
        buffer->data()[offset + 2U] = camera ? 12U : 230U;
        buffer->data()[offset + 3U] = 255U;
    }
    return creator::media::VideoFrame{
        .timestamp = at(static_cast<std::int64_t>(index) * 1'000'000'000LL /
                        30LL),
        .width = width,
        .height = height,
        .visibleRect = {0, 0, width, height},
        .contentWidth = width,
        .contentHeight = height,
        .pixelFormat = creator::media::PixelFormat::Bgra8,
        .platformHandle = std::move(buffer),
    };
}

creator::media::AudioBlock audioBlock(std::uint32_t index, bool systemAudio) {
    constexpr std::uint32_t frames = 480;
    auto samples = std::shared_ptr<float[]>(new float[frames * 2U]);
    for (std::uint32_t sample = 0; sample < frames * 2U; ++sample) {
        const auto phase = static_cast<int>((sample + index) % 17U) - 8;
        samples[sample] = static_cast<float>(phase) /
                          (systemAudio ? 40.0F : 32.0F);
    }
    return creator::media::AudioBlock{
        .timestamp = at(static_cast<std::int64_t>(index) * 10'000'000LL),
        .sampleRate = 48'000,
        .channels = 2,
        .frameCount = frames,
        .samples = std::move(samples),
    };
}

Result<creator::recorder::EncodedSegment> encodeFifteenSecondAudio(
    const fs::path& path, const SourceId& sourceId) {
    fs::create_directories(path.parent_path());
    auto track = creator::recorder::RecordingTrack::create(
        sourceId, TrackRole::Microphone);
    if (!track.hasValue()) return track.error();
    creator::ffmpeg_adapter::FfmpegAudioSegmentEncoder encoder;
    auto started = encoder.start(creator::recorder::SegmentEncodeConfig{
        .track = track.value(),
        .partPath = path,
        .startTime = at(0),
        .targetDuration = 15s});
    if (!started.hasValue()) return started.error();
    for (std::uint32_t block = 0; block < 1'500; ++block) {
        if (auto accepted = encoder.accept(audioBlock(block, false));
            !accepted.hasValue()) {
            encoder.abort();
            return accepted.error();
        }
    }
    return encoder.finish(at(15'000'000'000LL));
}

AppError ffmpegFixtureError(std::string operation, int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> detail{};
    av_strerror(code, detail.data(), detail.size());
    return AppError{ErrorCode::IoFailure,
                    std::move(operation) + ": " + detail.data()};
}

std::string ffmpegFixturePath(const fs::path& path) {
    const auto encoded = path.generic_u8string();
    return {encoded.begin(), encoded.end()};
}

Result<void> encodeOneSecondVideo(const fs::path& path,
                                  const SourceId& sourceId) {
    auto track = creator::recorder::RecordingTrack::create(
        sourceId, TrackRole::Screen);
    if (!track.hasValue()) return track.error();
    creator::ffmpeg_adapter::FfmpegVideoSegmentEncoder encoder{
        std::make_unique<creator::ffmpeg_adapter::CpuBgraFrameMapper>(),
        creator::ffmpeg_adapter::VideoEncoderOptions{
            .preferredEncoderNames = {"mpeg4"}}};
    auto started = encoder.start(creator::recorder::SegmentEncodeConfig{
        .track = track.value(),
        .partPath = path,
        .startTime = at(0),
        .targetDuration = 1s});
    if (!started.hasValue()) return started.error();
    for (std::uint32_t frame = 0; frame < 30; ++frame) {
        if (auto accepted = encoder.accept(videoFrame(frame, false));
            !accepted.hasValue()) {
            encoder.abort();
            return accepted.error();
        }
    }
    auto finished = encoder.finish(at(1'000'000'000LL));
    return finished.hasValue() ? creator::core::ok()
                               : Result<void>{finished.error()};
}

Result<void> encodeOneSecondAudio(const fs::path& path,
                                  const SourceId& sourceId) {
    auto track = creator::recorder::RecordingTrack::create(
        sourceId, TrackRole::Microphone);
    if (!track.hasValue()) return track.error();
    creator::ffmpeg_adapter::FfmpegAudioSegmentEncoder encoder;
    auto started = encoder.start(creator::recorder::SegmentEncodeConfig{
        .track = track.value(),
        .partPath = path,
        .startTime = at(0),
        .targetDuration = 1s});
    if (!started.hasValue()) return started.error();
    for (std::uint32_t block = 0; block < 100; ++block) {
        if (auto accepted = encoder.accept(audioBlock(block, false));
            !accepted.hasValue()) {
            encoder.abort();
            return accepted.error();
        }
    }
    auto finished = encoder.finish(at(1'000'000'000LL));
    return finished.hasValue() ? creator::core::ok()
                               : Result<void>{finished.error()};
}

struct InputFormatDeleter final {
    void operator()(AVFormatContext* context) const noexcept {
        avformat_close_input(&context);
    }
};

struct OutputFormatDeleter final {
    void operator()(AVFormatContext* context) const noexcept {
        if (!context) return;
        if (context->pb) avio_closep(&context->pb);
        avformat_free_context(context);
    }
};

struct PacketDeleter final {
    void operator()(AVPacket* packet) const noexcept { av_packet_free(&packet); }
};

Result<void> remuxAudioBeforeVideo(const fs::path& audioPath,
                                   const fs::path& videoPath,
                                   const fs::path& outputPath) {
    AVFormatContext* rawAudio{};
    const auto audioUtf8 = ffmpegFixturePath(audioPath);
    int status = avformat_open_input(&rawAudio, audioUtf8.c_str(),
                                     nullptr, nullptr);
    if (status < 0) return ffmpegFixtureError("open fixture audio", status);
    std::unique_ptr<AVFormatContext, InputFormatDeleter> audio{rawAudio};
    AVFormatContext* rawVideo{};
    const auto videoUtf8 = ffmpegFixturePath(videoPath);
    status = avformat_open_input(&rawVideo, videoUtf8.c_str(), nullptr,
                                 nullptr);
    if (status < 0) return ffmpegFixtureError("open fixture video", status);
    std::unique_ptr<AVFormatContext, InputFormatDeleter> video{rawVideo};
    if ((status = avformat_find_stream_info(audio.get(), nullptr)) < 0) {
        return ffmpegFixtureError("inspect fixture audio", status);
    }
    if ((status = avformat_find_stream_info(video.get(), nullptr)) < 0) {
        return ffmpegFixtureError("inspect fixture video", status);
    }
    const int audioIndex = av_find_best_stream(
        audio.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const int videoIndex = av_find_best_stream(
        video.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (audioIndex < 0 || videoIndex < 0) {
        return AppError{ErrorCode::NotFound,
                        "fixture input stream was not found"};
    }

    AVFormatContext* rawOutput{};
    const auto outputUtf8 = ffmpegFixturePath(outputPath);
    status = avformat_alloc_output_context2(
        &rawOutput, nullptr, "matroska", outputUtf8.c_str());
    if (status < 0 || !rawOutput) {
        return ffmpegFixtureError("create fixture container",
                                  status < 0 ? status : AVERROR_UNKNOWN);
    }
    std::unique_ptr<AVFormatContext, OutputFormatDeleter> output{rawOutput};
    AVStream* outputAudio = avformat_new_stream(output.get(), nullptr);
    AVStream* outputVideo = avformat_new_stream(output.get(), nullptr);
    if (!outputAudio || !outputVideo) {
        return AppError{ErrorCode::IoFailure,
                        "create fixture output streams failed"};
    }
    if ((status = avcodec_parameters_copy(
             outputAudio->codecpar, audio->streams[audioIndex]->codecpar)) < 0) {
        return ffmpegFixtureError("copy fixture audio parameters", status);
    }
    if ((status = avcodec_parameters_copy(
             outputVideo->codecpar, video->streams[videoIndex]->codecpar)) < 0) {
        return ffmpegFixtureError("copy fixture video parameters", status);
    }
    outputAudio->time_base = audio->streams[audioIndex]->time_base;
    outputVideo->time_base = video->streams[videoIndex]->time_base;
    if ((status = avio_open(&output->pb, outputUtf8.c_str(),
                            AVIO_FLAG_WRITE)) < 0) {
        return ffmpegFixtureError("open fixture output", status);
    }
    if ((status = avformat_write_header(output.get(), nullptr)) < 0) {
        return ffmpegFixtureError("write fixture header", status);
    }
    const auto copyStream = [&](AVFormatContext* input, int inputIndex,
                                AVStream* destination) -> Result<void> {
        std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
        if (!packet) {
            return AppError{ErrorCode::IoFailure,
                            "allocate fixture packet failed"};
        }
        int read{};
        while ((read = av_read_frame(input, packet.get())) >= 0) {
            if (packet->stream_index == inputIndex) {
                av_packet_rescale_ts(packet.get(),
                                     input->streams[inputIndex]->time_base,
                                     destination->time_base);
                packet->stream_index = destination->index;
                packet->pos = -1;
                const int written =
                    av_interleaved_write_frame(output.get(), packet.get());
                if (written < 0) {
                    return ffmpegFixtureError("write fixture packet", written);
                }
            }
            av_packet_unref(packet.get());
        }
        return read == AVERROR_EOF
                   ? creator::core::ok()
                   : Result<void>{ffmpegFixtureError("read fixture packet",
                                                     read)};
    };
    if (auto copied = copyStream(audio.get(), audioIndex, outputAudio);
        !copied.hasValue()) {
        return copied.error();
    }
    if (auto copied = copyStream(video.get(), videoIndex, outputVideo);
        !copied.hasValue()) {
        return copied.error();
    }
    if ((status = av_write_trailer(output.get())) < 0) {
        return ffmpegFixtureError("write fixture trailer", status);
    }
    return creator::core::ok();
}

creator::edit_engine::TimelineSnapshot audioFirstVideoSnapshot(
    const fs::path& root, const fs::path& relativePath) {
    const auto rate = creator::core::FrameRate::create(30, 1).value();
    const auto duration = DurationNs{900'000'000};
    auto asset = creator::domain::MediaAsset::create(
                     creator::domain::AssetId::create("audio-first-video")
                         .value(),
                     creator::domain::MediaKind::Video,
                     relativePath.generic_string(), duration,
                     creator::domain::VideoAssetMetadata{
                         .width = 320, .height = 180, .frameRate = rate},
                     creator::domain::AudioAssetMetadata{48'000, 2},
                     fs::file_size(root / relativePath), "mpeg4+aac",
                     creator::domain::AssetAvailability::Available)
                     .value();
    auto timeline = creator::domain::Timeline::create(
                        creator::domain::TimelineId::create("main").value(),
                        "Main", rate)
                        .value();
    const auto trackId =
        creator::domain::TrackId::create("video-track").value();
    EXPECT_TRUE(timeline
                    .addTrack(creator::domain::Track::create(
                                  trackId, creator::domain::TrackKind::Video,
                                  "Video", true, false)
                                  .value())
                    .hasValue());
    const auto range = creator::domain::TimeRange::create(at(0), duration).value();
    EXPECT_TRUE(timeline
                    .insertClip(
                        trackId,
                        creator::domain::Clip::createAsset(
                            creator::domain::ClipId::create("video-clip")
                                .value(),
                            asset, range, range, true, std::nullopt,
                            std::nullopt)
                            .value())
                    .hasValue());
    return creator::edit_engine::TimelineSnapshot{
        std::move(timeline),
        creator::domain::TimelineRevision::create(1).value(),
        {std::move(asset)}, root};
}

std::optional<EditorSessionState> loadEditorState(const fs::path& packageRoot) {
    EditorSessionWorker worker;
    EditorSessionResultPtr result;
    QObject::connect(
        &worker, &EditorSessionWorker::opened, &worker,
        [&](quint64, EditorSessionResultPtr value) { result = std::move(value); },
        Qt::DirectConnection);
    worker.openProject(1, packageRoot);
    if (!result || !result->hasValue()) return std::nullopt;
    return result->value().state;
}

std::vector<std::uint8_t> fileBytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

double meanAbsolute(const std::vector<float>& samples) {
    double total{};
    for (const float sample : samples) total += std::abs(sample);
    return samples.empty() ? 0.0 : total / static_cast<double>(samples.size());
}

class NoopReconciler final : public IRecordingTimelineReconciler {
public:
    [[nodiscard]] Result<RecordingReconcileResult> reconcile(
        const fs::path&, const SessionId& sessionId) override {
        return RecordingReconcileResult{.sessionId = sessionId,
                                        .imported = false,
                                        .revision = 0,
                                        .assetCount = 0,
                                        .trackCount = 0,
                                        .markerCount = 0};
    }
};

creator::app::StudioStoreOpenFactory sqliteStudioFactory() {
    return [](const fs::path& packageRoot)
               -> Result<std::unique_ptr<IStudioStore>> {
        ProjectPackageStore packages;
        auto opened = packages.open(packageRoot);
        if (!opened.hasValue()) return opened.error();
        const auto lease = opened.value().databaseIdentityLease;
        if (!lease) {
            return AppError{ErrorCode::IoFailure,
                            "validated Studio database identity is missing"};
        }
        auto store = SqliteStudioStore::open(
            opened.value().databasePath,
            opened.value().package.manifest.projectId,
            [lease] { return lease->verifyCurrentIdentity(); });
        if (!store.hasValue()) return store.error();
        return std::unique_ptr<IStudioStore>{
            new SqliteStudioStore{std::move(store).value()}};
    };
}

QVariantList modelRows(QAbstractItemModel* model) {
    QVariantList rows;
    const auto roles = model->roleNames();
    for (int row = 0; row < model->rowCount(); ++row) {
        QVariantMap values;
        const auto index = model->index(row, 0);
        for (auto role = roles.cbegin(); role != roles.cend(); ++role) {
            values.insert(QString::fromUtf8(role.value()),
                          model->data(index, role.key()));
        }
        rows.push_back(values);
    }
    return rows;
}

TEST(R1StudioWorkflowAcceptanceTest,
     SelectsRelativeVideoStreamWhenAudioIsContainerStreamZero) {
    PhysicalRoot root{"audio-first-video"};
    const fs::path media = root.path() / "media";
    fs::create_directories(media);
    const fs::path audioPath = media / "audio.mka";
    const fs::path videoPath = media / "video.mkv";
    const fs::path combinedPath = media / "audio-first.mkv";
    ASSERT_TRUE(encodeOneSecondAudio(
                    audioPath, SourceId::create("audio-source").value())
                    .hasValue());
    ASSERT_TRUE(encodeOneSecondVideo(
                    videoPath, SourceId::create("video-source").value())
                    .hasValue());
    auto remuxed = remuxAudioBeforeVideo(audioPath, videoPath, combinedPath);
    ASSERT_TRUE(remuxed.hasValue()) << remuxed.error().message();

    AVFormatContext* rawContext{};
    const auto combinedUtf8 = ffmpegFixturePath(combinedPath);
    ASSERT_GE(avformat_open_input(&rawContext, combinedUtf8.c_str(),
                                  nullptr, nullptr),
              0);
    std::unique_ptr<AVFormatContext, InputFormatDeleter> context{rawContext};
    ASSERT_GE(avformat_find_stream_info(context.get(), nullptr), 0);
    ASSERT_EQ(context->nb_streams, 2U);
    EXPECT_EQ(context->streams[0]->codecpar->codec_type,
              AVMEDIA_TYPE_AUDIO);
    EXPECT_EQ(context->streams[1]->codecpar->codec_type,
              AVMEDIA_TYPE_VIDEO);

    creator::mlt_adapter::MltEditEngine mlt{
        {.runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
         .previewWidth = 320,
         .previewHeight = 180}};
    auto loaded = mlt.load(audioFirstVideoSnapshot(
        root.path(), fs::path{"media/audio-first.mkv"}));
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    auto frame = mlt.requestFrame(at(500'000'000));
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* bgra = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(bgra, nullptr);
    std::uint64_t red{};
    std::uint64_t blue{};
    const auto pixels = static_cast<std::size_t>(frame.value().frame().width) *
                        frame.value().frame().height;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        blue += bgra[pixel * 4U];
        red += bgra[pixel * 4U + 2U];
    }
    EXPECT_GT(red, blue * 4U);
    EXPECT_GT(red / pixels, 160U);
}

TEST(R1StudioWorkflowAcceptanceTest,
     RecordsFourPhysicalSourcesReconcilesAndReopensThroughMlt) {
    PhysicalRoot root{"physical"};
    const fs::path packagePath = root.path() / fs::path{u8"강의 프로젝트.cstudio"};
    auto packageStore = std::make_shared<ProjectPackageStore>();
    auto created = packageStore->create(packagePath, "강의 프로젝트");
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    auto createdPackage = std::move(created).value();
    const auto projectId = createdPackage.package.manifest.projectId;
    const auto databasePath = createdPackage.databasePath;
    auto databaseLease = createdPackage.databaseIdentityLease;
    ASSERT_TRUE(databaseLease);
    {
        auto timelines = creator::project_store::SqliteTimelineStore::open(
            databasePath, projectId,
            [databaseLease] { return databaseLease->verifyCurrentIdentity(); });
        ASSERT_TRUE(timelines.hasValue()) << timelines.error().message();
        auto timeline = creator::domain::Timeline::create(
            creator::domain::TimelineId::create("main").value(), "Main",
            creator::core::FrameRate::create(60, 1).value());
        ASSERT_TRUE(timeline.hasValue()) << timeline.error().message();
        ASSERT_TRUE(timelines.value().createTimeline(timeline.value()).hasValue());
    }
    createdPackage.databaseIdentityLease.reset();
    databaseLease.reset();

    const auto sessionId = SessionId::create("세션-물리-001").value();
    const auto screen = SourceId::create("screen").value();
    const auto camera = SourceId::create("camera").value();
    const auto microphone = SourceId::create("microphone").value();
    const auto systemAudio = SourceId::create("system-audio").value();
    const std::vector<LiveCaptureSource> liveSources{
        {screen, TrackRole::Screen}, {camera, TrackRole::Camera},
        {microphone, TrackRole::Microphone},
        {systemAudio, TrackRole::SystemAudio}};
    ASSERT_TRUE(packageStore
                    ->beginRecording(packagePath, sessionId, at(0),
                                     utc("2026-07-18T01:00:00Z"))
                    .hasValue());
    QVariantList expectedScenes;
    QVariantList expectedSources;
    QVariantList expectedActiveSources;
    std::uint64_t studioIdentity{};
    {
        StudioWorkflowController studio{sqliteStudioFactory(),
                                        std::make_unique<NoopReconciler>(),
                                        [&studioIdentity] {
                                            return "physical-studio-id-" +
                                                   std::to_string(
                                                       ++studioIdentity);
                                        }};
        studio.openProject(QUrl::fromLocalFile(
            QString::fromStdWString(packagePath.wstring())));
        ASSERT_TRUE(waitUntil([&] { return !studio.busy(); }));
        ASSERT_EQ(studio.sceneModel()->rowCount(), 3);
        const QVariantList sources{
            QVariantMap{{QStringLiteral("sourceId"), QStringLiteral("screen")},
                        {QStringLiteral("role"), QStringLiteral("screen")}},
            QVariantMap{{QStringLiteral("sourceId"), QStringLiteral("camera")},
                        {QStringLiteral("role"), QStringLiteral("camera")}},
            QVariantMap{{QStringLiteral("sourceId"),
                         QStringLiteral("microphone")},
                        {QStringLiteral("role"),
                         QStringLiteral("microphone")}},
            QVariantMap{{QStringLiteral("sourceId"),
                         QStringLiteral("system-audio")},
                        {QStringLiteral("role"),
                         QStringLiteral("system_audio")}}};
        studio.prepareRecording(QString::fromStdString(sessionId.value()),
                                sources, 0);
        ASSERT_TRUE(waitUntil([&] { return !studio.busy(); }));
        ASSERT_TRUE(studio.recording())
            << studio.statusMessage().toStdString();
        studio.switchScene(QStringLiteral("screen"), 500'000'000);
        ASSERT_TRUE(waitUntil([&] { return !studio.busy(); }));
        studio.switchScene(QStringLiteral("camera"), 1'000'000'000);
        ASSERT_TRUE(waitUntil([&] { return !studio.busy(); }));
        studio.switchScene(QStringLiteral("presentation"), 1'500'000'000);
        ASSERT_TRUE(waitUntil([&] { return !studio.busy(); }));
        for (int marker = 0; marker < 5; ++marker) {
            studio.addMarker(QStringLiteral("강조 ") +
                                 QString::number(marker + 1),
                             1'550'000'000LL + marker * 100'000'000LL);
            ASSERT_TRUE(waitUntil([&] { return !studio.busy(); }));
            ASSERT_TRUE(studio.statusMessage().isEmpty())
                << studio.statusMessage().toStdString();
        }
        EXPECT_EQ(studio.markerCount(), 5);
        EXPECT_EQ(studio.activeSceneId(), QStringLiteral("presentation"));
        expectedScenes = modelRows(studio.sceneModel());
        expectedSources = modelRows(studio.sourceModel());
        expectedActiveSources = modelRows(studio.activeSourceModel());
    }

    auto bindings = std::make_shared<CaptureBindings>();
    bindings->sources = liveSources;
    std::optional<LiveRecordingCompletion> completion;
    {
        FfmpegLiveRecordingEngine engine{bindings, packageStore};
        ASSERT_TRUE(engine.available()) << engine.unavailableReason();
        auto promise =
            std::make_shared<std::promise<Result<LiveRecordingCompletion>>>();
        auto future = promise->get_future();
        auto started = engine.start(
            LiveRecordingStart{.sessionId = sessionId,
                               .packagePath = packagePath,
                               .startedAt = at(0),
                               .sources = liveSources},
            [promise](auto result) {
                promise->set_value(std::move(result));
            });
        ASSERT_TRUE(started.hasValue()) << started.error().message();
        ASSERT_EQ(bindings->videos.size(), 2U);
        ASSERT_EQ(bindings->audios.size(), 2U);
        std::uint32_t nextVideoFrame = 0;
        for (std::uint32_t block = 0; block < 210; ++block) {
            bindings->audios.at("microphone")->onAudioBlock(
                audioBlock(block, false));
            bindings->audios.at("system-audio")->onAudioBlock(
                audioBlock(block, true));
            const auto audioTimestamp =
                static_cast<std::int64_t>(block) * 10'000'000LL;
            while (nextVideoFrame < 63 &&
                   static_cast<std::int64_t>(nextVideoFrame) *
                           1'000'000'000LL / 30LL <=
                       audioTimestamp) {
                bindings->videos.at("screen")->onVideoFrame(
                    videoFrame(nextVideoFrame, false));
                bindings->videos.at("camera")->onVideoFrame(
                    videoFrame(nextVideoFrame, true));
                ++nextVideoFrame;
            }
        }
        ASSERT_EQ(nextVideoFrame, 63U);
        engine.stopAsync(at(2'100'000'000));
        ASSERT_EQ(future.wait_for(30s), std::future_status::ready);
        auto completed = future.get();
        ASSERT_TRUE(completed.hasValue()) << completed.error().message();
        completion.emplace(std::move(completed).value());
        EXPECT_EQ(completion->trackCount, 4U);
        EXPECT_GE(completion->segmentsPublished, 4U);
        ASSERT_FALSE(completion->terminalError.has_value())
            << completion->terminalError->message();
    }
    ASSERT_GE(bindings->detachCalls, 1);
    ASSERT_TRUE(completion.has_value());
    ASSERT_TRUE(packageStore
                    ->completeRecording(packagePath, completion->session,
                                        utc("2026-07-18T01:00:03Z"))
                    .hasValue());
    packageStore.reset();
    bindings.reset();

    creator::ffmpeg_adapter::FfmpegMediaProbe mediaProbe;
    std::uint64_t eventSequence{};
    RecordingTimelineReconciler reconciler{
        mediaProbe,
        [&eventSequence] {
            return "r1-06-event-" + std::to_string(++eventSequence);
        },
        [] { return utc("2026-07-18T01:00:04Z"); }};
    auto imported = reconciler.reconcile(packagePath, sessionId);
    ASSERT_TRUE(imported.hasValue()) << imported.error().message();
    EXPECT_TRUE(imported.value().imported);
    EXPECT_EQ(imported.value().trackCount, 4U);
    EXPECT_EQ(imported.value().markerCount, 5U);
    EXPECT_GE(imported.value().assetCount, 4U);

    {
        StudioWorkflowController reopened{sqliteStudioFactory(),
                                          std::make_unique<NoopReconciler>(),
                                          [] { return "reopen-studio-id"; }};
        reopened.openProject(QUrl::fromLocalFile(
            QString::fromStdWString(packagePath.wstring())));
        ASSERT_TRUE(waitUntil([&] { return !reopened.busy(); }));
        EXPECT_EQ(modelRows(reopened.sceneModel()), expectedScenes);
        EXPECT_EQ(modelRows(reopened.sourceModel()), expectedSources);
        EXPECT_EQ(modelRows(reopened.activeSourceModel()), expectedActiveSources);
        EXPECT_EQ(reopened.activeSceneId(), QStringLiteral("presentation"));
        EXPECT_EQ(reopened.markerCount(), 0);
    }

    auto editorState = loadEditorState(packagePath);
    ASSERT_TRUE(editorState.has_value());
    EXPECT_EQ(editorState->snapshot.timeline.markers().size(), 5U);
    EXPECT_EQ(editorState->snapshot.timeline.tracks().size(), 4U);
    EXPECT_GE(editorState->assets.size(), 4U);
    std::size_t clipCount{};
    for (const auto& track : editorState->snapshot.timeline.tracks()) {
        clipCount += track.clips().size();
    }
    EXPECT_GT(clipCount, 4U);
    for (std::size_t marker = 0;
         marker < editorState->snapshot.timeline.markers().size(); ++marker) {
        EXPECT_EQ(editorState->snapshot.timeline.markers()[marker].position(),
                  at(1'550'000'000LL +
                     static_cast<std::int64_t>(marker) * 100'000'000LL));
        EXPECT_EQ(editorState->snapshot.timeline.markers()[marker].label(),
                  "강조 " + std::to_string(marker + 1));
    }
    auto exactEditorReopen = loadEditorState(packagePath);
    ASSERT_TRUE(exactEditorReopen.has_value());
    EXPECT_EQ(exactEditorReopen->assets, editorState->assets);
    EXPECT_EQ(exactEditorReopen->snapshot, editorState->snapshot);

    creator::mlt_adapter::MltEditEngine mlt{
        {.runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
         .previewWidth = 320,
         .previewHeight = 180}};
    auto loaded = mlt.load(editorState->snapshot);
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    auto frame = mlt.requestFrame(at(750'000'000));
    ASSERT_TRUE(frame.hasValue()) << frame.error().message();
    const auto* pixels = static_cast<const std::uint8_t*>(
        frame.value().frame().platformHandle.get());
    ASSERT_NE(pixels, nullptr);
    std::size_t coloredPixels{};
    for (std::size_t offset = 0;
         offset < static_cast<std::size_t>(frame.value().frame().width) *
                      frame.value().frame().height * 4U;
         offset += 4U) {
        if (pixels[offset] > 80U || pixels[offset + 2U] > 80U) ++coloredPixels;
    }
    EXPECT_GT(coloredPixels, 1'000U);
    auto mixed = mlt.requestMixedAudio(at(750'000'000), 48'000, 2, 960);
    ASSERT_TRUE(mixed.hasValue()) << mixed.error().message();
    EXPECT_GT(meanAbsolute(mixed.value()), 0.001);

    const auto databaseBeforeNoop = fileBytes(databasePath);
    ASSERT_FALSE(databaseBeforeNoop.empty());
    auto second = reconciler.reconcile(packagePath, sessionId);
    ASSERT_TRUE(second.hasValue()) << second.error().message();
    EXPECT_FALSE(second.value().imported);
    EXPECT_EQ(second.value().revision, imported.value().revision);
    EXPECT_EQ(fileBytes(databasePath), databaseBeforeNoop);

    EditorSessionWorker editWorker;
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(
        &editWorker, &EditorSessionWorker::opened, &editWorker,
        [&](quint64, EditorSessionResultPtr value) {
            openedResult = std::move(value);
        },
        Qt::DirectConnection);
    QObject::connect(
        &editWorker, &EditorSessionWorker::edited, &editWorker,
        [&](quint64, quint64, EditorSessionResultPtr value) {
            editedResult = std::move(value);
        },
        Qt::DirectConnection);
    editWorker.openProject(2, packagePath);
    ASSERT_TRUE(openedResult && openedResult->hasValue());
    editWorker.edit(2, 1,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Undo});
    ASSERT_TRUE(editedResult && editedResult->hasValue());
    EXPECT_TRUE(editedResult->value().state.snapshot.timeline.tracks().empty());
    editedResult.reset();
    editWorker.edit(2, 2,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Redo});
    ASSERT_TRUE(editedResult && editedResult->hasValue());
    EXPECT_EQ(editedResult->value().state.snapshot.timeline.tracks().size(), 4U);
}

TEST(R1StudioWorkflowAcceptanceTest,
     MissingPhysicalMediaLeavesNoPartialImportAndRetriesCleanly) {
    PhysicalRoot root{"failure-retry"};
    const fs::path packagePath = root.path() / fs::path{u8"복구 프로젝트.cstudio"};
    const auto packages = std::make_shared<ProjectPackageStore>();
    auto created = packages->create(packagePath, "복구 프로젝트");
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    auto createdPackage = std::move(created).value();
    const auto projectId = createdPackage.package.manifest.projectId;
    const auto databasePath = createdPackage.databasePath;
    auto databaseLease = createdPackage.databaseIdentityLease;
    ASSERT_TRUE(databaseLease);
    {
        auto timelines = creator::project_store::SqliteTimelineStore::open(
            databasePath, projectId,
            [databaseLease] { return databaseLease->verifyCurrentIdentity(); });
        ASSERT_TRUE(timelines.hasValue()) << timelines.error().message();
        auto timeline = creator::domain::Timeline::create(
            creator::domain::TimelineId::create("main").value(), "Main",
            creator::core::FrameRate::create(60, 1).value());
        ASSERT_TRUE(timeline.hasValue()) << timeline.error().message();
        ASSERT_TRUE(timelines.value().createTimeline(timeline.value()).hasValue());
    }

    const auto sessionId = SessionId::create("failure-retry-session").value();
    const auto microphone = SourceId::create("microphone").value();
    const std::vector<LiveCaptureSource> liveSources{
        {microphone, TrackRole::Microphone}};
    ASSERT_TRUE(packages
                    ->beginRecording(packagePath, sessionId, at(0),
                                     utc("2026-07-18T03:00:00Z"))
                    .hasValue());
    {
        auto opened = SqliteStudioStore::open(
            databasePath, projectId,
            [databaseLease] { return databaseLease->verifyCurrentIdentity(); });
        ASSERT_TRUE(opened.hasValue()) << opened.error().message();
        auto studio = std::move(opened).value();
        auto scenes = creator::domain::defaultStudioScenes();
        ASSERT_TRUE(scenes.hasValue());
        ASSERT_TRUE(studio.seedDefaultsIfEmpty(scenes.value()).hasValue());
        ASSERT_TRUE(studio
                        .prepareRecording(
                            sessionId,
                            {{microphone, StudioSourceRole::Microphone}},
                            SceneId::create("presentation").value())
                        .hasValue());
    }

    auto bindings = std::make_shared<CaptureBindings>();
    bindings->sources = liveSources;
    std::optional<LiveRecordingCompletion> completion;
    {
        FfmpegLiveRecordingEngine engine{bindings, packages};
        auto promise =
            std::make_shared<std::promise<Result<LiveRecordingCompletion>>>();
        auto future = promise->get_future();
        auto started = engine.start(
            LiveRecordingStart{.sessionId = sessionId,
                               .packagePath = packagePath,
                               .startedAt = at(0),
                               .sources = liveSources},
            [promise](auto result) { promise->set_value(std::move(result)); });
        ASSERT_TRUE(started.hasValue()) << started.error().message();
        for (std::uint32_t block = 0; block < 100; ++block) {
            bindings->audios.at("microphone")->onAudioBlock(
                audioBlock(block, false));
        }
        engine.stopAsync(at(1'000'000'000));
        ASSERT_EQ(future.wait_for(30s), std::future_status::ready);
        auto finished = future.get();
        ASSERT_TRUE(finished.hasValue()) << finished.error().message();
        completion.emplace(std::move(finished).value());
        ASSERT_FALSE(completion->terminalError.has_value());
    }
    ASSERT_TRUE(completion.has_value());
    ASSERT_EQ(completion->session.segments().size(), 1U);
    ASSERT_TRUE(packages
                    ->completeRecording(packagePath, completion->session,
                                        utc("2026-07-18T03:00:02Z"))
                    .hasValue());

    const fs::path mediaPath =
        packagePath / completion->session.segments().front().relativePath;
    const fs::path heldPath = mediaPath.parent_path() / "held-for-retry.mka";
    creator::ffmpeg_adapter::FfmpegMediaProbe mediaProbe;
    std::uint64_t eventSequence{};
    RecordingTimelineReconciler reconciler{
        mediaProbe,
        [&eventSequence] {
            return "failure-retry-event-" + std::to_string(++eventSequence);
        },
        [] { return utc("2026-07-18T03:00:03Z"); }};
    const auto databaseBeforeFailures = fileBytes(databasePath);
    ASSERT_FALSE(databaseBeforeFailures.empty());
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::error_code moveError;
        fs::rename(mediaPath, heldPath, moveError);
        ASSERT_FALSE(moveError) << moveError.message();
        const auto failed = reconciler.reconcile(packagePath, sessionId);
        EXPECT_FALSE(failed.hasValue());
        EXPECT_EQ(fileBytes(databasePath), databaseBeforeFailures);
        auto editor = loadEditorState(packagePath);
        ASSERT_TRUE(editor.has_value());
        EXPECT_TRUE(editor->assets.empty());
        EXPECT_TRUE(editor->snapshot.timeline.tracks().empty());
        fs::rename(heldPath, mediaPath, moveError);
        ASSERT_FALSE(moveError) << moveError.message();
    }

    const fs::path validBackup =
        mediaPath.parent_path() / "valid-for-corrupt-retry.mka";
    std::error_code copyError;
    fs::copy_file(mediaPath, validBackup,
                  fs::copy_options::overwrite_existing, copyError);
    ASSERT_FALSE(copyError) << copyError.message();
    {
        std::ofstream corrupt{mediaPath,
                              std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(corrupt.is_open());
        corrupt << "not a media container";
        ASSERT_TRUE(corrupt.good());
    }
    const auto corruptFailure = reconciler.reconcile(packagePath, sessionId);
    EXPECT_FALSE(corruptFailure.hasValue());
    EXPECT_EQ(fileBytes(databasePath), databaseBeforeFailures);
    auto editorAfterCorruption = loadEditorState(packagePath);
    ASSERT_TRUE(editorAfterCorruption.has_value());
    EXPECT_TRUE(editorAfterCorruption->assets.empty());
    EXPECT_TRUE(editorAfterCorruption->snapshot.timeline.tracks().empty());
    fs::copy_file(validBackup, mediaPath,
                  fs::copy_options::overwrite_existing, copyError);
    ASSERT_FALSE(copyError) << copyError.message();
    fs::remove(validBackup, copyError);
    ASSERT_FALSE(copyError) << copyError.message();

    const auto retried = reconciler.reconcile(packagePath, sessionId);
    ASSERT_TRUE(retried.hasValue()) << retried.error().message();
    EXPECT_TRUE(retried.value().imported);
    EXPECT_EQ(retried.value().trackCount, 1U);
    EXPECT_EQ(retried.value().assetCount, 1U);
    const auto databaseAfterImport = fileBytes(databasePath);
    const auto noOp = reconciler.reconcile(packagePath, sessionId);
    ASSERT_TRUE(noOp.hasValue()) << noOp.error().message();
    EXPECT_FALSE(noOp.value().imported);
    EXPECT_EQ(fileBytes(databasePath), databaseAfterImport);

    createdPackage.databaseIdentityLease.reset();
    databaseLease.reset();
    std::uint64_t controllerIdentity{};
    for (int attempt = 0; attempt < 3; ++attempt) {
        StudioWorkflowController controller{
            sqliteStudioFactory(), std::make_unique<NoopReconciler>(),
            [&controllerIdentity] {
                return "destruction-id-" +
                       std::to_string(++controllerIdentity);
            }};
        controller.openProject(QUrl::fromLocalFile(
            QString::fromStdWString(packagePath.wstring())));
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        EXPECT_TRUE(controller.statusMessage().isEmpty())
            << controller.statusMessage().toStdString();
        controller.reopenProject();
    }
}

TEST(R1StudioWorkflowAcceptanceTest,
     RealStudioWorkerKeepsUiResponsiveAcrossSwitchAndMarkerScale) {
    PhysicalRoot root{"scale"};
    const fs::path packagePath = root.path() / "scale.cstudio";
    ProjectPackageStore packages;
    auto created = packages.create(packagePath, "Scale");
    ASSERT_TRUE(created.hasValue()) << created.error().message();
    auto createdPackage = std::move(created).value();
    const auto scaleProjectId = createdPackage.package.manifest.projectId;
    const auto scaleDatabasePath = createdPackage.databasePath;
    auto scaleDatabaseLease = createdPackage.databaseIdentityLease;
    ASSERT_TRUE(scaleDatabaseLease);
    {
        auto timelines = creator::project_store::SqliteTimelineStore::open(
            scaleDatabasePath, scaleProjectId,
            [scaleDatabaseLease] {
                return scaleDatabaseLease->verifyCurrentIdentity();
            });
        ASSERT_TRUE(timelines.hasValue()) << timelines.error().message();
        auto timeline = creator::domain::Timeline::create(
            creator::domain::TimelineId::create("main").value(), "Main",
            creator::core::FrameRate::create(60, 1).value());
        ASSERT_TRUE(timeline.hasValue());
        ASSERT_TRUE(timelines.value().createTimeline(timeline.value()).hasValue());
    }
    createdPackage.databaseIdentityLease.reset();
    scaleDatabaseLease.reset();
    const auto scaleSession = SessionId::create("scale-session").value();
    ASSERT_TRUE(packages
                    .beginRecording(packagePath, scaleSession, at(0),
                                    utc("2026-07-18T02:00:00Z"))
                    .hasValue());

    const auto scaleSource = SourceId::create("microphone").value();
    std::vector<creator::domain::SegmentInfo> scaleSegments;
    scaleSegments.reserve(120);
    const fs::path firstRelative =
        fs::path{"media/microphone/microphone/segment_0.mka"};
    auto encoded = encodeFifteenSecondAudio(packagePath / firstRelative,
                                            scaleSource);
    ASSERT_TRUE(encoded.hasValue()) << encoded.error().message();
    const auto segmentDuration = encoded.value().endTime - at(0);
    EXPECT_LE(segmentDuration, 15s);
    EXPECT_GT(segmentDuration, 14s);
    for (std::uint64_t index = 0; index < 120; ++index) {
        const auto start = at(static_cast<std::int64_t>(index) *
                              15'000'000'000LL);
        const fs::path relativePath =
            fs::path{"media/microphone/microphone"} /
            ("segment_" + std::to_string(index) + ".mka");
        if (index > 0) {
            std::error_code copyError;
            fs::copy_file(packagePath / firstRelative,
                          packagePath / relativePath,
                          fs::copy_options::overwrite_existing, copyError);
            ASSERT_FALSE(copyError) << copyError.message();
        }
        const creator::domain::SegmentInfo writing{
            .index = index,
            .sourceId = scaleSource,
            .startTime = start,
            .duration = DurationNs::zero(),
            .status = creator::domain::SegmentStatus::Writing,
            .relativePath = relativePath.generic_string()};
        ASSERT_TRUE(packages.beginSegment(packagePath, scaleSession, writing)
                        .hasValue());
        auto ready = writing;
        ready.duration = segmentDuration;
        ready.status = creator::domain::SegmentStatus::Ready;
        ASSERT_TRUE(packages.markSegmentReady(packagePath, scaleSession, ready)
                        .hasValue());
        scaleSegments.push_back(std::move(ready));
    }

    std::chrono::milliseconds maximumUiGap{};
    std::atomic<std::uint64_t> nextId{};
    {
        StudioWorkflowController controller{
            sqliteStudioFactory(), std::make_unique<NoopReconciler>(),
            [&nextId] {
                return "acceptance-id-" + std::to_string(++nextId);
            }};
        controller.openProject(QUrl::fromLocalFile(
            QString::fromStdWString(packagePath.wstring())));
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        ASSERT_EQ(controller.sceneModel()->rowCount(), 3);
        const QVariantList sources{
            QVariantMap{{QStringLiteral("sourceId"),
                         QStringLiteral("microphone")},
                        {QStringLiteral("role"),
                         QStringLiteral("microphone")}}};
        controller.prepareRecording(QStringLiteral("scale-session"), sources,
                                    0);
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
        ASSERT_TRUE(controller.recording());

        auto previousTick = std::chrono::steady_clock::now();
        for (int operation = 0; operation < 60; ++operation) {
            controller.switchScene(
                operation % 2 == 0 ? QStringLiteral("screen")
                                   : QStringLiteral("presentation"),
                static_cast<qint64>(operation) * 30'000'000'000LL);
            ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
            for (int marker = 0; marker < 5; ++marker) {
                controller.addMarker(
                    QString::fromUtf8("표시"),
                    static_cast<qint64>(operation) * 30'000'000'000LL +
                        static_cast<qint64>(marker) * 2'000'000'000LL);
                ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
                const auto now = std::chrono::steady_clock::now();
                maximumUiGap = std::max(
                    maximumUiGap,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - previousTick));
                previousTick = now;
            }
        }
        EXPECT_EQ(controller.markerCount(), 300);
        EXPECT_LT(maximumUiGap.count(), 250);

        creator::domain::RecordingSession session{scaleSession};
        ASSERT_TRUE(session.start(at(0)).hasValue());
        for (const auto& segment : scaleSegments) {
            ASSERT_TRUE(session.addSegment(segment).hasValue());
        }
        ASSERT_TRUE(session.stop(at(1'800'000'000'000LL)).hasValue());
        ASSERT_TRUE(packages
                        .completeRecording(packagePath, session,
                                           utc("2026-07-18T02:30:00Z"))
                        .hasValue());
        controller.completeRecording();
        ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }, 30s));
        EXPECT_FALSE(controller.recording());
    }

#ifdef _WIN32
    DWORD handlesBefore{};
    ASSERT_NE(GetProcessHandleCount(GetCurrentProcess(), &handlesBefore), FALSE);
    PROCESS_MEMORY_COUNTERS_EX memoryBefore{};
    memoryBefore.cb = sizeof(memoryBefore);
    ASSERT_NE(K32GetProcessMemoryInfo(
                  GetCurrentProcess(),
                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryBefore),
                  sizeof(memoryBefore)),
              FALSE);
#endif

    creator::ffmpeg_adapter::FfmpegMediaProbe mediaProbe;
    std::uint64_t eventSequence{};
    RecordingTimelineReconciler reconciler{
        mediaProbe,
        [&eventSequence] {
            return "scale-event-" + std::to_string(++eventSequence);
        },
        [] { return utc("2026-07-18T02:31:00Z"); }};
    const auto importStarted = std::chrono::steady_clock::now();
    auto imported = reconciler.reconcile(packagePath, scaleSession);
    const auto importElapsed = std::chrono::steady_clock::now() - importStarted;
    ASSERT_TRUE(imported.hasValue()) << imported.error().message();
    EXPECT_EQ(imported.value().assetCount, 120U);
    EXPECT_EQ(imported.value().trackCount, 1U);
    EXPECT_EQ(imported.value().markerCount, 300U);

    auto editor = loadEditorState(packagePath);
    ASSERT_TRUE(editor.has_value());
    ASSERT_EQ(editor->snapshot.timeline.tracks().size(), 1U);
    EXPECT_EQ(editor->snapshot.timeline.tracks().front().clips().size(), 120U);
    EXPECT_EQ(editor->snapshot.timeline.markers().size(), 300U);
    const auto graphStarted = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration graphElapsed{};
    std::chrono::steady_clock::duration frameElapsed{};
    {
        creator::mlt_adapter::MltEditEngine mlt{
            {.runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
             .previewWidth = 320,
             .previewHeight = 180}};
        auto loaded = mlt.load(editor->snapshot);
        ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
        graphElapsed = std::chrono::steady_clock::now() - graphStarted;
        EXPECT_LT(graphElapsed, 10s);
        const auto frameStarted = std::chrono::steady_clock::now();
        auto frame = mlt.requestFrame(at(900'000'000'000LL));
        ASSERT_TRUE(frame.hasValue()) << frame.error().message();
        frameElapsed = std::chrono::steady_clock::now() - frameStarted;
        EXPECT_LT(frameElapsed, 3s);
        auto pcm = mlt.requestMixedAudio(at(900'000'000'000LL), 48'000, 2,
                                         960);
        ASSERT_TRUE(pcm.hasValue()) << pcm.error().message();
        EXPECT_GT(meanAbsolute(pcm.value()), 0.001);
    }
    EXPECT_LT(importElapsed, 30s);

#ifdef _WIN32
    DWORD handlesAfter{};
    ASSERT_NE(GetProcessHandleCount(GetCurrentProcess(), &handlesAfter), FALSE);
    EXPECT_LE(handlesAfter, 16'384U);
    EXPECT_LE(handlesAfter, handlesBefore + 6'144U);
#endif

    {
        creator::mlt_adapter::MltEditEngine reopenedMlt{
            {.runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
             .previewWidth = 320,
             .previewHeight = 180}};
        auto loaded = reopenedMlt.load(editor->snapshot);
        ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
        auto frame = reopenedMlt.requestFrame(at(900'000'000'000LL));
        ASSERT_TRUE(frame.hasValue()) << frame.error().message();
        auto pcm = reopenedMlt.requestMixedAudio(
            at(900'000'000'000LL), 48'000, 2, 960);
        ASSERT_TRUE(pcm.hasValue()) << pcm.error().message();
        EXPECT_GT(meanAbsolute(pcm.value()), 0.001);
    }

#ifdef _WIN32
    DWORD handlesAfterReopen{};
    ASSERT_NE(GetProcessHandleCount(GetCurrentProcess(), &handlesAfterReopen),
              FALSE);
    EXPECT_LE(handlesAfterReopen, 16'384U);
    EXPECT_LE(handlesAfterReopen, handlesAfter + 192U);
    PROCESS_MEMORY_COUNTERS_EX memoryAfter{};
    memoryAfter.cb = sizeof(memoryAfter);
    ASSERT_NE(K32GetProcessMemoryInfo(
                  GetCurrentProcess(),
                  reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memoryAfter),
                  sizeof(memoryAfter)),
              FALSE);
    const auto workingSetDelta =
        memoryAfter.WorkingSetSize > memoryBefore.WorkingSetSize
            ? memoryAfter.WorkingSetSize - memoryBefore.WorkingSetSize
            : 0U;
    EXPECT_LT(workingSetDelta, 768ULL * 1024ULL * 1024ULL);
    std::cout
        << "[ R1-06 METRICS ] import_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(importElapsed)
               .count()
        << " graph_build_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(graphElapsed)
               .count()
        << " frame_request_ms="
        << std::chrono::duration_cast<std::chrono::milliseconds>(frameElapsed)
               .count()
        << " max_ui_gap_ms=" << maximumUiGap.count()
        << " handles_before=" << handlesBefore
        << " handles_after_first=" << handlesAfter
        << " handles_after_reopen=" << handlesAfterReopen
        << " working_set_delta_bytes=" << workingSetDelta << '\n';
#endif
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
