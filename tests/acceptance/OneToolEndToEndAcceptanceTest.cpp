// One-tool end-to-end acceptance: prove the full post-recording flow is real.
//
// A single headless run that chains the four user-visible steps the product
// promises after a take is captured:
//   1. RECORD -> EDITOR: record four separated physical sources (screen, camera,
//      microphone, system audio) through the real FfmpegLiveRecordingEngine,
//      finalize the .cstudio package, reconcile the segments into a timeline, and
//      re-open that timeline so the recorded media appears as editor clips.
//   2. CAPTIONS (whisper): run the real WhisperTranscriptionProvider on a known
//      speech clip (the upstream jfk.wav sample), get a Transcript with
//      word/segment project-timebase timestamps, persist it with TranscriptStore
//      (the exact artifact EditorController::loadTranscript reads to populate the
//      QML caption panel), and read it back.
//   3. CUT: perform a real Split + ripple DeleteRange on the recorded timeline
//      through the edit engine (EditorSessionWorker/TimelineEditService) and
//      prove the timeline changed (clip count + duration), that it is durable
//      (survives a fresh reopen), and undoable/redoable.
//   4. EXPORT: render the edited timeline to a real H.264/AAC MP4 through the
//      ProjectExportEngine and ffprobe-verify it (codec, streams, duration).
//
// This is the enabled-preset + real-machine gate: it only builds and runs when
// CS_ENABLE_FFMPEG + CS_ENABLE_MLT + CS_ENABLE_WHISPER are all on. It reuses the
// recording helpers proven by R1StudioWorkflowAcceptanceTest.

#include "app/EditorSessionWorker.h"
#include "app/FfmpegLiveRecordingEngine.h"
#include "app/ILiveCaptureBindings.h"
#include "app/ProjectExportEngine.h"
#include "app/RecordingTimelineReconciler.h"
#include "core/Utc.h"
#include "domain/RecordingSession.h"
#include "domain/StudioScene.h"
#include "domain/Timeline.h"
#include "edit_engine/EditEngineTypes.h"
#include "ffmpeg_adapter/BgraFrameMappers.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"
#include "project_store/SqliteTimelineStore.h"
#include "transcription/AudioInput.h"
#include "transcription/Transcript.h"
#include "transcription/TranscriptStore.h"
#include "whisper_adapter/WhisperTranscriptionProvider.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using creator::app::EditorSessionResultPtr;
using creator::app::EditorSessionState;
using creator::app::EditorSessionWorker;
using creator::app::FfmpegLiveRecordingEngine;
using creator::app::ILiveCaptureBindings;
using creator::app::LiveCaptureSource;
using creator::app::LiveRecordingCompletion;
using creator::app::LiveRecordingStart;
using creator::app::ProjectExportEngine;
using creator::app::RecordingTimelineReconciler;
using creator::core::AppError;
using creator::core::DurationNs;
using creator::core::ErrorCode;
using creator::core::Result;
using creator::core::TimestampNs;
using creator::domain::SceneId;
using creator::domain::SessionId;
using creator::domain::SourceId;
using creator::domain::StudioSourceRole;
using creator::recorder::TrackRole;

creator::core::Utc utc(std::string_view value) {
    return creator::core::Utc::parseRfc3339(value).value();
}

TimestampNs at(std::int64_t nanoseconds) {
    return TimestampNs{DurationNs{nanoseconds}};
}

class PhysicalRoot final {
public:
    explicit PhysicalRoot(std::string suffix) {
        path_ = fs::temp_directory_path() /
                fs::path{u8"creator-studio-onetool"} /
                (std::to_string(QCoreApplication::applicationPid()) + "-" +
                 std::move(suffix));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_);
    }
    ~PhysicalRoot() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

// Direct, synchronous capture bindings (identical contract to the R1 studio
// workflow acceptance test): the driver pushes frames straight into the sinks.
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
                return AppError{ErrorCode::InvalidArgument, "video sink missing"};
            }
            videos[source.sourceId.value()] = std::move(videoSink);
        } else {
            if (!audioSink) {
                return AppError{ErrorCode::InvalidArgument, "audio sink missing"};
            }
            audios[source.sourceId.value()] = std::move(audioSink);
        }
        return creator::core::ok();
    }
    void detachAll() noexcept override {
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
};

creator::media::VideoFrame videoFrame(std::uint32_t index, bool camera) {
    constexpr std::uint32_t width = 320;
    constexpr std::uint32_t height = 180;
    auto buffer = creator::ffmpeg_adapter::CpuBgraFrameBuffer::create(width, height)
                      .value();
    for (std::size_t offset = 0; offset < buffer->size(); offset += 4U) {
        buffer->data()[offset] = camera ? 230U : 12U;
        buffer->data()[offset + 1U] = 20U;
        buffer->data()[offset + 2U] = camera ? 12U : 230U;
        buffer->data()[offset + 3U] = 255U;
    }
    return creator::media::VideoFrame{
        .timestamp = at(static_cast<std::int64_t>(index) * 1'000'000'000LL / 30LL),
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
        samples[sample] = static_cast<float>(phase) / (systemAudio ? 40.0F : 32.0F);
    }
    return creator::media::AudioBlock{
        .timestamp = at(static_cast<std::int64_t>(index) * 10'000'000LL),
        .sampleRate = 48'000,
        .channels = 2,
        .frameCount = frames,
        .samples = std::move(samples),
    };
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

// Total number of clips across every track, and the timeline's played length
// (the largest clip end on any track). These are the two numbers a cut moves.
std::size_t totalClipCount(const creator::domain::Timeline& timeline) {
    std::size_t count{};
    for (const auto& track : timeline.tracks()) count += track.clips().size();
    return count;
}

std::int64_t timelineDurationNs(const creator::domain::Timeline& timeline) {
    std::int64_t end{};
    for (const auto& track : timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            end = std::max(end, clip.timelineRange().end().time_since_epoch().count());
        }
    }
    return end;
}

// Minimal PCM16 mono WAV reader (jfk.wav is 16 kHz mono s16le), same as the
// whisper adapter unit test.
std::optional<std::vector<float>> readPcm16MonoWav(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    if (bytes.size() < 44) return std::nullopt;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const std::string id(bytes.data() + offset, 4);
        std::uint32_t chunkSize = 0;
        std::memcpy(&chunkSize, bytes.data() + offset + 4, 4);
        const std::size_t body = offset + 8;
        if (id == "data") {
            std::vector<float> samples;
            const std::size_t count =
                std::min<std::size_t>(chunkSize, bytes.size() - body) / 2;
            samples.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                std::int16_t value = 0;
                std::memcpy(&value, bytes.data() + body + i * 2, 2);
                samples.push_back(static_cast<float>(value) / 32768.0f);
            }
            return samples;
        }
        offset = body + chunkSize + (chunkSize & 1u);
    }
    return std::nullopt;
}

TEST(OneToolEndToEndAcceptanceTest, RecordCaptionCutExportRealTake) {
    PhysicalRoot root{"take"};
    const fs::path packagePath = root.path() / fs::path{u8"원툴 강의.cstudio"};
    auto packageStore = std::make_shared<creator::project_store::ProjectPackageStore>();
    auto created = packageStore->create(packagePath, "원툴 강의");
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

    // ---- STEP 1: RECORD four separated sources through the real engine -------
    const auto sessionId = SessionId::create("onetool-session").value();
    const auto screen = SourceId::create("screen").value();
    const auto camera = SourceId::create("camera").value();
    const auto microphone = SourceId::create("microphone").value();
    const auto systemAudio = SourceId::create("system-audio").value();
    const std::vector<LiveCaptureSource> liveSources{
        {screen, TrackRole::Screen},
        {camera, TrackRole::Camera},
        {microphone, TrackRole::Microphone},
        {systemAudio, TrackRole::SystemAudio}};
    ASSERT_TRUE(packageStore
                    ->beginRecording(packagePath, sessionId, at(0),
                                     utc("2026-07-21T01:00:00Z"))
                    .hasValue());

    // The recorder captures media; the Studio store owns the scene graph and the
    // per-session source roles that RecordingTimelineReconciler maps into tracks.
    // In the app this is done by the Studio workflow; drive the same store here so
    // the recorded take reconciles into an editor timeline (record -> editor).
    {
        auto openedStudio = creator::project_store::SqliteStudioStore::open(
            databasePath, projectId,
            [databaseLease] { return databaseLease->verifyCurrentIdentity(); });
        ASSERT_TRUE(openedStudio.hasValue()) << openedStudio.error().message();
        auto studio = std::move(openedStudio).value();
        auto scenes = creator::domain::defaultStudioScenes();
        ASSERT_TRUE(scenes.hasValue()) << scenes.error().message();
        ASSERT_TRUE(studio.seedDefaultsIfEmpty(scenes.value()).hasValue());
        ASSERT_TRUE(studio
                        .prepareRecording(
                            sessionId,
                            {{screen, StudioSourceRole::Screen},
                             {camera, StudioSourceRole::Camera},
                             {microphone, StudioSourceRole::Microphone},
                             {systemAudio, StudioSourceRole::SystemAudio}},
                            SceneId::create("presentation").value())
                        .hasValue());
    }
    databaseLease.reset();

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
            [promise](auto result) { promise->set_value(std::move(result)); });
        ASSERT_TRUE(started.hasValue()) << started.error().message();
        ASSERT_EQ(bindings->videos.size(), 2U);
        ASSERT_EQ(bindings->audios.size(), 2U);
        std::uint32_t nextVideoFrame = 0;
        for (std::uint32_t block = 0; block < 210; ++block) {
            bindings->audios.at("microphone")->onAudioBlock(audioBlock(block, false));
            bindings->audios.at("system-audio")->onAudioBlock(audioBlock(block, true));
            const auto audioTimestamp = static_cast<std::int64_t>(block) * 10'000'000LL;
            while (nextVideoFrame < 63 &&
                   static_cast<std::int64_t>(nextVideoFrame) * 1'000'000'000LL /
                           30LL <=
                       audioTimestamp) {
                bindings->videos.at("screen")->onVideoFrame(
                    videoFrame(nextVideoFrame, false));
                bindings->videos.at("camera")->onVideoFrame(
                    videoFrame(nextVideoFrame, true));
                ++nextVideoFrame;
            }
        }
        engine.stopAsync(at(2'100'000'000));
        ASSERT_EQ(future.wait_for(30s), std::future_status::ready);
        auto completed = future.get();
        ASSERT_TRUE(completed.hasValue()) << completed.error().message();
        completion.emplace(std::move(completed).value());
        EXPECT_EQ(completion->trackCount, 4U);
        ASSERT_FALSE(completion->terminalError.has_value())
            << completion->terminalError->message();
    }
    ASSERT_TRUE(completion.has_value());
    ASSERT_TRUE(packageStore
                    ->completeRecording(packagePath, completion->session,
                                        utc("2026-07-21T01:00:03Z"))
                    .hasValue());
    packageStore.reset();
    bindings.reset();

    // Reconcile the recorded segments into a timeline (record -> editor).
    creator::ffmpeg_adapter::FfmpegMediaProbe mediaProbe;
    std::uint64_t eventSequence{};
    RecordingTimelineReconciler reconciler{
        mediaProbe,
        [&eventSequence] { return "onetool-event-" + std::to_string(++eventSequence); },
        [] { return utc("2026-07-21T01:00:04Z"); }};
    auto imported = reconciler.reconcile(packagePath, sessionId);
    ASSERT_TRUE(imported.hasValue()) << imported.error().message();
    EXPECT_TRUE(imported.value().imported);
    EXPECT_EQ(imported.value().trackCount, 4U);

    auto editorState = loadEditorState(packagePath);
    ASSERT_TRUE(editorState.has_value());
    const auto& recordedTimeline = editorState->snapshot.timeline;
    ASSERT_GE(recordedTimeline.tracks().size(), 4U);
    const std::size_t recordedClips = totalClipCount(recordedTimeline);
    ASSERT_GT(recordedClips, 0U);
    std::cout << "[ ONE-TOOL PROOF ] step1 record->editor: tracks="
              << recordedTimeline.tracks().size() << " clips=" << recordedClips
              << " assets=" << editorState->assets.size()
              << " timeline_ns=" << timelineDurationNs(recordedTimeline) << '\n';

    // ---- STEP 2: CAPTIONS via the real whisper.cpp provider -----------------
    const fs::path sample = fs::path{CS_TEST_WHISPER_SAMPLE};
    ASSERT_TRUE(fs::exists(sample)) << "jfk.wav sample missing: " << sample;
    auto pcm = readPcm16MonoWav(sample);
    ASSERT_TRUE(pcm.has_value() && !pcm->empty());
    creator::whisper_adapter::WhisperProviderConfig whisperConfig;
    whisperConfig.runtimeRoot = fs::path{CS_TEST_WHISPER_ROOT};
    whisperConfig.threadCount = 4;
    auto provider =
        creator::whisper_adapter::WhisperTranscriptionProvider::create(whisperConfig);
    ASSERT_TRUE(provider.hasValue()) << provider.error().message();
    const auto audio =
        creator::transcription::AudioInput::create(*pcm, 16'000, 1).value();
    const creator::transcription::TranscriptionOptions options{microphone, "en"};
    auto transcribed = provider.value()->transcribe(audio, options);
    ASSERT_TRUE(transcribed.hasValue()) << transcribed.error().message();
    const auto& transcript = transcribed.value();
    ASSERT_FALSE(transcript.segments().empty());
    ASSERT_FALSE(transcript.fullText().empty());
    std::cout << "[ ONE-TOOL PROOF ] step2 whisper text: \"" << transcript.fullText()
              << "\"\n";
    std::size_t printedWords{};
    for (const auto& segment : transcript.segments()) {
        std::cout << "[ ONE-TOOL PROOF ]   segment ["
                  << segment.range().start().time_since_epoch().count() << "ns.."
                  << segment.range().end().time_since_epoch().count()
                  << "ns] \"" << segment.text() << "\"\n";
        for (const auto& word : segment.words()) {
            if (printedWords++ >= 8U) break;
            std::cout << "[ ONE-TOOL PROOF ]     word @"
                      << word.range().start().time_since_epoch().count() << "ns \""
                      << word.text() << "\" conf=" << word.confidence() << '\n';
        }
    }
    // Persist the transcript exactly as EditorController::loadTranscript expects,
    // then read it back (that is the caption-panel data path in the app).
    creator::transcription::TranscriptStore transcriptStore{packagePath / "transcripts"};
    auto written = transcriptStore.write("microphone", transcript);
    ASSERT_TRUE(written.hasValue()) << written.error().message();
    auto reloaded = transcriptStore.read(written.value());
    ASSERT_TRUE(reloaded.hasValue()) << reloaded.error().message();
    EXPECT_EQ(reloaded.value().segments().size(), transcript.segments().size());
    EXPECT_EQ(reloaded.value().fullText(), transcript.fullText());
    std::cout << "[ ONE-TOOL PROOF ] step2 caption artifact: "
              << written.value().filename().string() << " segments="
              << reloaded.value().segments().size() << '\n';

    // ---- STEP 3: CUT (Split + ripple DeleteRange) through the edit engine ----
    // Pick the longest clip on a Video track to cut inside of.
    creator::domain::TrackId cutTrack = recordedTimeline.tracks().front().id();
    creator::domain::ClipId cutClip = creator::domain::ClipId::create("x").value();
    std::int64_t bestStart = 0, bestEnd = 0, bestSpan = -1;
    for (const auto& track : recordedTimeline.tracks()) {
        if (track.kind() != creator::domain::TrackKind::Video) continue;
        for (const auto& clip : track.clips()) {
            const auto s = clip.timelineRange().start().time_since_epoch().count();
            const auto e = clip.timelineRange().end().time_since_epoch().count();
            if (e - s > bestSpan) {
                bestSpan = e - s;
                bestStart = s;
                bestEnd = e;
                cutTrack = track.id();
                cutClip = clip.id();
            }
        }
    }
    ASSERT_GT(bestSpan, 400'000'000) << "no video clip long enough to cut";
    const std::int64_t splitAt = bestStart + bestSpan / 2;
    const std::int64_t delStart = splitAt + bestSpan / 8;
    const std::int64_t delSpan = bestSpan / 8;  // strictly inside [splitAt, end]
    const auto deleteRange =
        creator::domain::TimeRange::create(at(delStart), DurationNs{delSpan}).value();

    EditorSessionWorker editWorker;
    EditorSessionResultPtr openedResult;
    EditorSessionResultPtr editedResult;
    QObject::connect(
        &editWorker, &EditorSessionWorker::opened, &editWorker,
        [&](quint64, EditorSessionResultPtr v) { openedResult = std::move(v); },
        Qt::DirectConnection);
    QObject::connect(
        &editWorker, &EditorSessionWorker::edited, &editWorker,
        [&](quint64, quint64, EditorSessionResultPtr v) { editedResult = std::move(v); },
        Qt::DirectConnection);
    editWorker.openProject(3, packagePath);
    ASSERT_TRUE(openedResult && openedResult->hasValue())
        << (openedResult ? openedResult->error().message() : "no open result");
    const std::size_t beforeClips =
        totalClipCount(openedResult->value().state.snapshot.timeline);
    const std::int64_t beforeDuration =
        timelineDurationNs(openedResult->value().state.snapshot.timeline);

    // Split the chosen clip.
    editedResult.reset();
    editWorker.edit(3, 1,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Split,
                        .trackId = cutTrack,
                        .clipId = cutClip,
                        .position = at(splitAt)});
    ASSERT_TRUE(editedResult && editedResult->hasValue())
        << (editedResult ? editedResult->error().message() : "no split result");
    const std::size_t afterSplitClips =
        totalClipCount(editedResult->value().state.snapshot.timeline);
    EXPECT_EQ(afterSplitClips, beforeClips + 1U) << "split must create one clip";

    // Ripple-delete a span inside the right half; this must shorten the timeline.
    editedResult.reset();
    editWorker.edit(3, 2,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::DeleteRange,
                        .range = deleteRange,
                        .ripple = true});
    ASSERT_TRUE(editedResult && editedResult->hasValue())
        << (editedResult ? editedResult->error().message() : "no delete result");
    const auto cutSnapshot = editedResult->value().state.snapshot;
    const std::int64_t afterDuration = timelineDurationNs(cutSnapshot.timeline);
    EXPECT_LT(afterDuration, beforeDuration) << "ripple delete must shorten timeline";
    // The removed span should match delSpan within a couple of frame snaps.
    EXPECT_NEAR(static_cast<double>(beforeDuration - afterDuration),
                static_cast<double>(delSpan), 1'000'000'000.0 / 15.0);
    EXPECT_TRUE(editedResult->value().state.canUndo);
    std::cout << "[ ONE-TOOL PROOF ] step3 cut: clips " << beforeClips << " -> "
              << afterSplitClips << " (split) ; duration_ns " << beforeDuration
              << " -> " << afterDuration << " (ripple -" << (beforeDuration - afterDuration)
              << ")\n";

    // Undo both edits -> back to the recorded timeline.
    editedResult.reset();
    editWorker.edit(3, 3,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Undo});
    ASSERT_TRUE(editedResult && editedResult->hasValue());
    editedResult.reset();
    editWorker.edit(3, 4,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Undo});
    ASSERT_TRUE(editedResult && editedResult->hasValue());
    EXPECT_EQ(timelineDurationNs(editedResult->value().state.snapshot.timeline),
              beforeDuration);
    EXPECT_EQ(totalClipCount(editedResult->value().state.snapshot.timeline),
              beforeClips);
    // Redo both -> cut re-applied.
    editedResult.reset();
    editWorker.edit(3, 5,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Redo});
    ASSERT_TRUE(editedResult && editedResult->hasValue());
    editedResult.reset();
    editWorker.edit(3, 6,
                    creator::app::EditorEditRequest{
                        .kind = creator::app::EditorEditKind::Redo});
    ASSERT_TRUE(editedResult && editedResult->hasValue());
    EXPECT_EQ(timelineDurationNs(editedResult->value().state.snapshot.timeline),
              afterDuration);
    std::cout << "[ ONE-TOOL PROOF ] step3 undo/redo verified\n";

    // Durability: a completely fresh reopen must still show the cut.
    auto durable = loadEditorState(packagePath);
    ASSERT_TRUE(durable.has_value());
    EXPECT_EQ(timelineDurationNs(durable->snapshot.timeline), afterDuration)
        << "cut must survive reopen (durable)";

    // ---- STEP 4: EXPORT the edited timeline to a real H.264/AAC MP4 ----------
    ProjectExportEngine exportEngine{fs::path{CS_TEST_MLT_ROOT}};
    const fs::path exportDir = root.path() / "export";
    fs::create_directories(exportDir);
    const fs::path destination = exportDir / fs::path{u8"완성-원툴.mp4"};
    auto request = creator::edit_engine::RenderRequest::create(
        projectId, durable->snapshot, destination,
        creator::edit_engine::RenderPreset::h2641080p30().value(),
        creator::edit_engine::RenderOverwritePolicy::ReplaceExisting);
    ASSERT_TRUE(request.hasValue()) << request.error().message();
    auto job = exportEngine.render(request.value());
    ASSERT_TRUE(job.hasValue()) << job.error().message();
    creator::edit_engine::RenderJobState state =
        creator::edit_engine::RenderJobState::Pending;
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto progress = job.value()->progress();
        ASSERT_TRUE(progress.hasValue()) << progress.error().message();
        state = progress.value().state();
        if (state == creator::edit_engine::RenderJobState::Completed ||
            state == creator::edit_engine::RenderJobState::Failed ||
            state == creator::edit_engine::RenderJobState::Cancelled) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_EQ(state, creator::edit_engine::RenderJobState::Completed)
        << job.value()->diagnostic();
    ASSERT_TRUE(fs::is_regular_file(destination));

    auto media = mediaProbe.probe(exportDir, destination.filename());
    ASSERT_TRUE(media.hasValue()) << media.error().message();
    EXPECT_EQ(media.value().codecName, "h264");
    ASSERT_TRUE(media.value().video.has_value());
    EXPECT_EQ(media.value().video->width, 1920U);
    EXPECT_EQ(media.value().video->height, 1080U);
    ASSERT_TRUE(media.value().audio.has_value());
    EXPECT_EQ(media.value().audio->sampleRate, 48'000U);
    EXPECT_EQ(media.value().audio->channels, 2U);
    // The exported MP4's duration should track the edited (cut) timeline length.
    const std::int64_t exportedNs = media.value().duration.count();
    EXPECT_GT(exportedNs, 0);
    EXPECT_NEAR(static_cast<double>(exportedNs), static_cast<double>(afterDuration),
                300'000'000.0);
    // Optional proof hook: persist the exported MP4 outside the auto-cleaned
    // PhysicalRoot so an external ffprobe can independently verify h264+aac.
    if (const char* keep = std::getenv("CS_ONETOOL_EXPORT_COPY");
        keep != nullptr && keep[0] != '\0') {
        std::error_code copyError;
        fs::create_directories(fs::path{keep}.parent_path(), copyError);
        fs::copy_file(destination, fs::path{keep},
                      fs::copy_options::overwrite_existing, copyError);
        std::cout << "[ ONE-TOOL PROOF ] step4 export copied to: " << keep
                  << (copyError ? (" (copy error: " + copyError.message() + ")")
                                : std::string{})
                  << '\n';
    }
    std::cout << "[ ONE-TOOL PROOF ] step4 export: " << destination.string()
              << " codec=" << media.value().codecName << " "
              << media.value().video->width << "x" << media.value().video->height
              << " audio=" << media.value().audio->sampleRate << "/"
              << media.value().audio->channels
              << " bytes=" << media.value().byteSize
              << " duration_ns=" << exportedNs << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
