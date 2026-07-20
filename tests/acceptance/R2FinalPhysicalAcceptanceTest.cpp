#include "app/CreatorIntelligenceController.h"
#include "app/EditorController.h"
#include "app/ExportController.h"
#include "app/MltCreatorIntelligenceAudioLoader.h"
#include "app/ProjectController.h"
#include "app/ProjectExportEngine.h"
#include "app/TranscriptionProviderFactory.h"
#include "audio_dsp/AudioBuffer.h"
#include "audio_dsp/AudioCleanupChain.h"
#include "audio_dsp/AudioFormat.h"
#include "audio_dsp/LoudnessMeter.h"
#include "core/Timebase.h"
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#include "mlt_adapter/MltEditEngine.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteRenderJobStore.h"
#include "project_store/SqliteTimelineStore.h"
#include "rnnoise_adapter/RnnoiseDenoiseProcessor.h"
#include "transcription/TranscriptStore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSignalSpy>

#include <gtest/gtest.h>

#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <map>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using creator::core::DurationNs;
using creator::core::TimestampNs;

std::string utf8Path(const fs::path& path) {
    const auto encoded = path.generic_u8string();
    return std::string{encoded.begin(), encoded.end()};
}

bool waitUntil(const std::function<bool()>& predicate,
               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        std::this_thread::sleep_for(5ms);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    return predicate();
}

DWORD processHandleCount() {
    DWORD value = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &value) == FALSE
               ? std::numeric_limits<DWORD>::max()
               : value;
}

struct ProcessBounds final {
    DWORD initialHandles{};
    DWORD maximumHandles{};
    std::size_t initialWorkingSet{};
    std::size_t maximumWorkingSet{};
    bool valid{true};

    void sample() {
        DWORD handles = 0;
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        if (GetProcessHandleCount(GetCurrentProcess(), &handles) == FALSE ||
            GetProcessMemoryInfo(
                GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                sizeof(memory)) == FALSE) {
            valid = false;
            return;
        }
        if (initialHandles == 0) {
            initialHandles = handles;
            maximumHandles = handles;
            initialWorkingSet = memory.WorkingSetSize;
            maximumWorkingSet = memory.WorkingSetSize;
        }
        maximumHandles = std::max(maximumHandles, handles);
        maximumWorkingSet = std::max(maximumWorkingSet,
                                     static_cast<std::size_t>(memory.WorkingSetSize));
    }

    void print() const {
        std::cout << "[ R2-07 PROCESS ] handles_initial=" << initialHandles
                  << " handles_max=" << maximumHandles
                  << " handles_growth=" << maximumHandles - initialHandles
                  << " working_set_initial=" << initialWorkingSet
                  << " working_set_max=" << maximumWorkingSet
                  << " working_set_growth="
                  << maximumWorkingSet - initialWorkingSet << std::endl;
    }
};

creator::mlt_adapter::MltEditEngineConfig::AudioProcessorFactory
physicalAudioFactory() {
    const auto runtimeRoot = fs::path{CS_TEST_RNNOISE_ROOT};
    return [runtimeRoot]()
        -> creator::core::Result<std::unique_ptr<
            creator::audio_dsp::IAudioProcessor>> {
        auto denoise =
            creator::rnnoise_adapter::createRnnoiseDenoiseProcessor(runtimeRoot);
        if (!denoise.hasValue()) return denoise.error();
        auto format = creator::audio_dsp::AudioFormat::create(48'000, 2);
        if (!format.hasValue()) return format.error();
        auto chain = creator::audio_dsp::makeAudioCleanupChain(
            format.value(), std::move(denoise).value());
        if (!chain.hasValue()) return chain.error();
        return std::unique_ptr<creator::audio_dsp::IAudioProcessor>{
            std::move(chain).value()};
    };
}

creator::core::Result<creator::edit_engine::TimelineSnapshot>
firstAudioWindow(const creator::edit_engine::TimelineSnapshot& source,
                 DurationNs maximumDuration) {
    auto timeline = creator::domain::Timeline::create(
        source.timeline.id(), source.timeline.name() + " (bounded local AI)",
        source.timeline.frameRate());
    if (!timeline.hasValue()) return timeline.error();
    const TimestampNs cutoff{maximumDuration};
    std::size_t keptClips = 0;
    for (const auto& sourceTrack : source.timeline.tracks()) {
        auto track = creator::domain::Track::create(
            sourceTrack.id(), sourceTrack.kind(), sourceTrack.name(),
            sourceTrack.enabled(), false);
        if (!track.hasValue()) return track.error();
        auto added = timeline.value().addTrack(std::move(track).value());
        if (!added.hasValue()) return added.error();
        for (const auto& sourceClip : sourceTrack.clips()) {
            if (sourceClip.kind() != creator::domain::ClipKind::Asset ||
                !sourceClip.hasAudio() ||
                sourceClip.timelineRange().start() >= cutoff) {
                continue;
            }
            const auto end = std::min(sourceClip.timelineRange().end(), cutoff);
            const auto duration = end - sourceClip.timelineRange().start();
            if (duration <= DurationNs{}) continue;
            auto timelineRange = creator::domain::TimeRange::create(
                sourceClip.timelineRange().start(), duration);
            auto sourceRange = creator::domain::TimeRange::create(
                sourceClip.sourceRange().start(), duration);
            if (!timelineRange.hasValue()) return timelineRange.error();
            if (!sourceRange.hasValue()) return sourceRange.error();
            auto clipped = sourceClip.withIdentityAndRanges(
                sourceClip.id(), sourceRange.value(), timelineRange.value());
            if (!clipped.hasValue()) return clipped.error();
            auto inserted = timeline.value().insertClip(
                sourceTrack.id(), std::move(clipped).value());
            if (!inserted.hasValue()) return inserted.error();
            ++keptClips;
        }
        if (sourceTrack.locked()) {
            auto locked = timeline.value().setTrackLocked(sourceTrack.id(), true);
            if (!locked.hasValue()) return locked.error();
        }
    }
    if (keptClips == 0) {
        return creator::core::AppError{
            creator::core::ErrorCode::InvalidState,
            "retained package has no audio in the selected local-AI window"};
    }
    auto snapshot = creator::edit_engine::TimelineSnapshot{
        std::move(timeline).value(), source.revision, source.assets,
        source.mediaRoot, source.canvasWidth, source.canvasHeight, {}};
    auto valid = creator::edit_engine::validateTimelineSnapshot(snapshot);
    if (!valid.hasValue()) return valid.error();
    std::cout << "[ R2-07 AI WINDOW ] clips=" << keptClips
              << " duration_ns=" << maximumDuration.count() << std::endl;
    return snapshot;
}

std::uint64_t samplesAtFrame(std::uint64_t frame, std::uint32_t frequency,
                             creator::core::FrameRate rate) {
    const auto numerator = static_cast<std::uint64_t>(rate.numerator());
    const auto denominator = static_cast<std::uint64_t>(rate.denominator());
    const auto samplesPerNumerator = denominator * frequency;
    return (frame / numerator) * samplesPerNumerator +
           ((frame % numerator) * samplesPerNumerator) / numerator;
}

std::int64_t addressableAudioFrames(DurationNs duration,
                                    creator::core::FrameRate rate) {
    return std::max<std::int64_t>(
        1, creator::core::timestampToFrame(TimestampNs{duration}, rate));
}

creator::core::Result<std::pair<double, double>> measurePublishedAudio(
    const fs::path& output,
    const creator::media::MediaProbeResult& media,
    ProcessBounds& bounds) {
    if (!media.video.has_value() || !media.audio.has_value()) {
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "published output lacks video or audio"};
    }
    const auto rate = creator::core::FrameRate::create(30, 1).value();
    auto asset = creator::domain::MediaAsset::create(
        creator::domain::AssetId::create("r2-published-output").value(),
        creator::domain::MediaKind::Video,
        utf8Path(output.filename()), media.duration,
        media.video, media.audio, media.byteSize, media.sha256,
        creator::domain::AssetAvailability::Available);
    if (!asset.hasValue()) return asset.error();
    auto timeline = creator::domain::Timeline::create(
        creator::domain::TimelineId::create("r2-output-meter").value(),
        "R2 output meter", rate);
    if (!timeline.hasValue()) return timeline.error();
    const auto trackId = creator::domain::TrackId::create("output").value();
    auto track = creator::domain::Track::create(
        trackId, creator::domain::TrackKind::Video, "Output", true, false);
    if (!track.hasValue()) return track.error();
    auto added = timeline.value().addTrack(std::move(track).value());
    if (!added.hasValue()) return added.error();
    auto range = creator::domain::TimeRange::create(TimestampNs{}, media.duration);
    if (!range.hasValue()) return range.error();
    auto clip = creator::domain::Clip::createAsset(
        creator::domain::ClipId::create("output-clip").value(), asset.value(),
        range.value(), range.value(), true, std::nullopt, std::nullopt);
    if (!clip.hasValue()) return clip.error();
    auto inserted = timeline.value().insertClip(trackId, std::move(clip).value());
    if (!inserted.hasValue()) return inserted.error();
    auto snapshot = creator::edit_engine::TimelineSnapshot{
        std::move(timeline).value(),
        creator::domain::TimelineRevision::create(1).value(),
        {std::move(asset).value()}, output.parent_path(), 16, 16, {}};

    creator::mlt_adapter::MltEditEngine reader{{
        .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
        .previewWidth = 16,
        .previewHeight = 16}};
    auto loaded = reader.load(snapshot);
    if (!loaded.hasValue()) return loaded.error();
    const auto format = creator::audio_dsp::AudioFormat::create(48'000, 2).value();
    auto meter = creator::audio_dsp::LoudnessMeter::create(format).value();
    const auto totalFrames = addressableAudioFrames(media.duration, rate);
    const auto wholeSeconds = media.duration.count() / 1'000'000'000LL;
    const auto remainder = media.duration.count() % 1'000'000'000LL;
    const auto totalSamples = static_cast<std::uint64_t>(wholeSeconds) * 48'000U +
                              static_cast<std::uint64_t>(remainder) * 48'000U /
                                  1'000'000'000U;
    for (std::int64_t frame = 0; frame < totalFrames; ++frame) {
        const auto first = samplesAtFrame(static_cast<std::uint64_t>(frame),
                                          48'000, rate);
        const auto next = std::min(
            samplesAtFrame(static_cast<std::uint64_t>(frame + 1), 48'000, rate),
            totalSamples);
        if (next <= first) continue;
        auto block = reader.requestMixedAudio(
            creator::core::frameToTimestamp(frame, rate), 48'000, 2,
            static_cast<int>(next - first));
        if (!block.hasValue()) return block.error();
        creator::audio_dsp::AudioBuffer view{
            block.value().data(), block.value().size() / 2U, format};
        auto measured = meter.addBlock(view);
        if (!measured.hasValue()) return measured.error();
        if (frame % 300 == 0) bounds.sample();
    }
    return std::pair{meter.integratedLufs(), meter.truePeakDbtp()};
}

TEST(R2FinalPhysicalAcceptanceTest,
     LoudnessMeterRequestsOnlyAddressableCompleteFrames) {
    const auto rate = creator::core::FrameRate::create(30, 1).value();
    EXPECT_EQ(addressableAudioFrames(DurationNs{1}, rate), 1);
    EXPECT_EQ(addressableAudioFrames(DurationNs{1'000'000'000}, rate), 30);
    EXPECT_EQ(addressableAudioFrames(DurationNs{1'000'000'001}, rate), 30);
    EXPECT_EQ(addressableAudioFrames(DurationNs{1'799'573'333'000}, rate),
              53'987);
}

TEST(R2FinalPhysicalAcceptanceTest,
     MeasuresExistingUnicodePublishedOutputWhenRequested) {
    const auto outputText = qEnvironmentVariable("CS_R2_PHYSICAL_OUTPUT");
    if (outputText.isEmpty()) {
        GTEST_SKIP() << "Set CS_R2_PHYSICAL_OUTPUT to a published R2 MP4";
    }
    const fs::path output{outputText.toStdWString()};
    creator::ffmpeg_adapter::FfmpegMediaProbe probe;
    auto media = probe.probe(output.parent_path(), output.filename());
    ASSERT_TRUE(media.hasValue()) << media.error().message();
    ASSERT_TRUE(media.value().video.has_value());
    ASSERT_TRUE(media.value().audio.has_value());
    EXPECT_EQ(media.value().video->width, 1'920);
    EXPECT_EQ(media.value().video->height, 1'080);
    EXPECT_EQ(media.value().audio->sampleRate, 48'000);
    EXPECT_EQ(media.value().audio->channels, 2);
    ProcessBounds bounds;
    bounds.sample();
    auto loudness = measurePublishedAudio(output, media.value(), bounds);
    ASSERT_TRUE(loudness.hasValue()) << loudness.error().message();
    std::cout << "[ R2-07 OUTPUT RECHECK ] path=" << utf8Path(output)
              << " lufs=" << loudness.value().first
              << " true_peak_dbtp=" << loudness.value().second << std::endl;
    if (std::isfinite(loudness.value().first)) {
        EXPECT_NEAR(loudness.value().first, -14.0, 1.0);
        EXPECT_LE(loudness.value().second, -0.2);
    } else {
        EXPECT_EQ(loudness.value().first,
                  creator::audio_dsp::LoudnessMeter::kNoMeasurement);
    }
}

TEST(R2FinalPhysicalAcceptanceTest, ReportsRetainedTimelineDecoderShape) {
    const auto packageText = qEnvironmentVariable("CS_R2_PHYSICAL_PACKAGE");
    if (packageText.isEmpty()) {
        GTEST_SKIP() << "Set CS_R2_PHYSICAL_PACKAGE to a copied retained package";
    }
    const fs::path packagePath{packageText.toStdWString()};
    creator::project_store::ProjectPackageStore packages;
    auto opened = packages.open(packagePath);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    const auto lease = opened.value().databaseIdentityLease;
    ASSERT_TRUE(lease);
    auto store = creator::project_store::SqliteTimelineStore::open(
        opened.value().databasePath, opened.value().package.manifest.projectId,
        [lease] { return lease->verifyCurrentIdentity(); });
    ASSERT_TRUE(store.hasValue()) << store.error().message();
    auto assets = store.value().assets();
    ASSERT_TRUE(assets.hasValue()) << assets.error().message();
    auto persisted = store.value().loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue()) << persisted.error().message();
    std::map<std::string, std::string> pathsByAsset;
    std::map<std::string, std::size_t> assetsByExtension;
    for (const auto& asset : assets.value()) {
        pathsByAsset.emplace(asset.id().value(), asset.relativePath());
        ++assetsByExtension[fs::path{asset.relativePath()}.extension().string()];
    }
    std::map<std::string, std::size_t> clipsByExtension;
    std::size_t clips = 0;
    std::size_t visual = 0;
    std::size_t envelopes = 0;
    for (const auto& track : persisted.value().timeline.tracks()) {
        for (const auto& clip : track.clips()) {
            ++clips;
            if (clip.visualTransform().has_value()) ++visual;
            if (clip.audioEnvelope().has_value()) ++envelopes;
            if (clip.assetId().has_value()) {
                const auto path = pathsByAsset.find(clip.assetId()->value());
                ASSERT_NE(path, pathsByAsset.end());
                ++clipsByExtension[fs::path{path->second}.extension().string()];
            }
        }
    }
    std::cout << "[ R2-07 TIMELINE ] assets=" << assets.value().size()
              << " tracks=" << persisted.value().timeline.tracks().size()
              << " clips=" << clips << " visual_filters=" << visual
              << " audio_envelopes=" << envelopes << std::endl;
    for (const auto& [extension, count] : assetsByExtension) {
        std::cout << "[ R2-07 ASSETS ] extension=" << extension
                  << " count=" << count << std::endl;
    }
    for (const auto& [extension, count] : clipsByExtension) {
        std::cout << "[ R2-07 CLIPS ] extension=" << extension
                  << " count=" << count << std::endl;
    }
}

TEST(R2FinalPhysicalAcceptanceTest,
     LoadsRetainedTimelineWithBoundedMltDecoderHandles) {
    const auto packageText = qEnvironmentVariable("CS_R2_PHYSICAL_PACKAGE");
    if (packageText.isEmpty()) {
        GTEST_SKIP() << "Set CS_R2_PHYSICAL_PACKAGE to a copied retained package";
    }
    const fs::path packagePath{packageText.toStdWString()};
    creator::project_store::ProjectPackageStore packages;
    auto opened = packages.open(packagePath);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    const auto lease = opened.value().databaseIdentityLease;
    ASSERT_TRUE(lease);
    auto store = creator::project_store::SqliteTimelineStore::open(
        opened.value().databasePath, opened.value().package.manifest.projectId,
        [lease] { return lease->verifyCurrentIdentity(); });
    ASSERT_TRUE(store.hasValue()) << store.error().message();
    auto assets = store.value().assets();
    ASSERT_TRUE(assets.hasValue()) << assets.error().message();
    auto persisted = store.value().loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue()) << persisted.error().message();
    auto revision = creator::domain::TimelineRevision::create(
        persisted.value().revision);
    ASSERT_TRUE(revision.hasValue()) << revision.error().message();
    auto snapshot = creator::edit_engine::TimelineSnapshot{
        persisted.value().timeline, revision.value(), assets.value(),
        packagePath, opened.value().package.manifest.canvas.width,
        opened.value().package.manifest.canvas.height, {}};
    ASSERT_TRUE(creator::edit_engine::validateTimelineSnapshot(snapshot).hasValue());

    ProcessBounds bounds;
    bounds.sample();
    const auto beforePreflight = processHandleCount();
    auto preflight = creator::mlt_adapter::MltEditEngine::preflightRuntime(
        fs::path{CS_TEST_STAGED_MLT_ROOT});
    ASSERT_TRUE(preflight.hasValue()) << preflight.error().message();
    const auto afterPreflight = processHandleCount();
    auto initialized = creator::mlt_adapter::MltEditEngine::initializeRuntime(
        fs::path{CS_TEST_STAGED_MLT_ROOT});
    ASSERT_TRUE(initialized.hasValue()) << initialized.error().message();
    const auto afterFactory = processHandleCount();
    ASSERT_NE(beforePreflight, std::numeric_limits<DWORD>::max());
    ASSERT_NE(afterPreflight, std::numeric_limits<DWORD>::max());
    ASSERT_NE(afterFactory, std::numeric_limits<DWORD>::max());
    std::cout << "[ R2-07 MLT INIT ] before=" << beforePreflight
              << " after_preflight=" << afterPreflight
              << " after_factory=" << afterFactory << std::endl;
    creator::mlt_adapter::MltEditEngine engine{{
        .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
        .previewWidth = 320,
        .previewHeight = 180}};
    auto loaded = engine.load(snapshot);
    bounds.sample();
    ASSERT_TRUE(loaded.hasValue()) << loaded.error().message();
    auto diagnostics = engine.diagnostics();
    ASSERT_TRUE(diagnostics.hasValue()) << diagnostics.error().message();
    std::cout << "[ R2-07 MLT GRAPH ] media_producers="
              << diagnostics.value().mediaProducerCount
              << " handles_initial=" << bounds.initialHandles
              << " handles_after_load=" << bounds.maximumHandles
              << " handles_growth="
              << bounds.maximumHandles - bounds.initialHandles << std::endl;
    EXPECT_LE(diagnostics.value().mediaProducerCount, 16U);
    EXPECT_LE(bounds.maximumHandles - bounds.initialHandles, 1'000U);
}

TEST(R2FinalPhysicalAcceptanceTest,
     ReopensThirtyMinutePackageRunsLocalAiAndPublishesNormalizedRetry) {
    const auto packageText = qEnvironmentVariable("CS_R2_PHYSICAL_PACKAGE");
    if (packageText.isEmpty()) {
        GTEST_SKIP() << "Set CS_R2_PHYSICAL_PACKAGE to a copied retained package";
    }
    const fs::path packagePath{packageText.toStdWString()};
    ASSERT_TRUE(fs::is_directory(packagePath)) << packagePath.string();
    ASSERT_TRUE(fs::is_regular_file(packagePath / "manifest.json"));

    ProcessBounds bounds;
    bounds.sample();
    const auto registry = packagePath.parent_path() / "r2-recent-projects.json";
    creator::app::ProjectController project{
        std::make_unique<creator::project_store::ProjectPackageStore>(),
        registry, false};
    project.openProject(QUrl::fromLocalFile(packageText));
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return !project.busy();
    }, 60s)) << project.statusMessage().toStdString();
    ASSERT_TRUE(project.hasOpenProject()) << project.statusMessage().toStdString();

    auto cleanupFactory = physicalAudioFactory();
    auto editorEngine = std::make_unique<creator::mlt_adapter::MltEditEngine>(
        creator::mlt_adapter::MltEditEngineConfig{
            .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
            .previewWidth = 320,
            .previewHeight = 180,
            .audioProcessingFactory = cleanupFactory});
    creator::app::EditorController editor{std::move(editorEngine)};
    editor.openProject(project.projectUrl());
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return !editor.busy() && !editor.sessionBusy();
    }, 15min)) << editor.statusMessage().toStdString();
    auto fullSnapshot = editor.exportSnapshot();
    ASSERT_TRUE(fullSnapshot.has_value());
    ASSERT_GE(editor.timelineDurationNs(), 29LL * 60LL * 1'000'000'000LL);
    const auto originalRevision = editor.timelineRevision();

    auto bounded = firstAudioWindow(*fullSnapshot, 30s);
    ASSERT_TRUE(bounded.hasValue()) << bounded.error().message();
    auto productLoader = creator::app::makeMltCreatorIntelligenceAudioLoader(
        fs::path{CS_TEST_STAGED_MLT_ROOT}, cleanupFactory);
    auto provider = creator::app::makeTranscriptionProvider(
        creator::app::TranscriptionProviderOptions{
            .whisperRuntimeRoot = fs::path{CS_TEST_WHISPER_ROOT},
            .threadCount = 0});
    creator::app::CreatorIntelligenceController intelligence{
        std::move(provider),
        [productLoader = std::move(productLoader), bounded = bounded.value()](
            const creator::edit_engine::TimelineSnapshot&, std::stop_token stop) {
            return productLoader(bounded, stop);
        },
        [&editor] { return editor.exportSnapshot(); },
        [&editor](const creator::transcription::Transcript& transcript,
                  std::int64_t revision) {
            return editor.approveTranscriptProposal(transcript, revision);
        },
        [&editor](const creator::domain::TimeRange& range,
                  std::int64_t revision) {
            return editor.approveCutProposal(range, revision, true);
        }};
    ASSERT_TRUE(intelligence.analyzeProject());
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return !intelligence.busy();
    }, 20min)) << intelligence.statusMessage().toStdString();
    ASSERT_TRUE(intelligence.hasPendingProposal())
        << intelligence.statusMessage().toStdString();
    EXPECT_EQ(editor.timelineRevision(), originalRevision);
    std::int64_t previousEnd = 0;
    for (const auto& rowValue : intelligence.transcriptProposal()) {
        const auto row = rowValue.toMap();
        const auto start = row.value(QStringLiteral("startNs")).toLongLong();
        const auto duration =
            row.value(QStringLiteral("durationNs")).toLongLong();
        EXPECT_GE(start, previousEnd);
        EXPECT_GT(duration, 0);
        EXPECT_LE(start + duration, 30LL * 1'000'000'000LL);
        EXPECT_FALSE(row.value(QStringLiteral("text")).toString().isEmpty());
        previousEnd = start + duration;
    }
    std::cout << "[ R2-07 WHISPER ] transcript_segments="
              << intelligence.transcriptProposal().size()
              << " cut_proposals=" << intelligence.cutSuggestions().size()
              << std::endl;
    ASSERT_TRUE(intelligence.approveTranscript())
        << intelligence.statusMessage().toStdString();
    EXPECT_EQ(editor.timelineRevision(), originalRevision);
    intelligence.rejectProposal();
    EXPECT_FALSE(intelligence.hasPendingProposal());
    editor.save();
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return !editor.sessionBusy() && editor.clean();
    }, 60s)) << editor.statusMessage().toStdString();

    creator::transcription::TranscriptStore transcriptStore{
        packagePath / "transcripts"};
    auto durableTranscript =
        transcriptStore.read(packagePath / "transcripts" / "local-ai.json");
    ASSERT_TRUE(durableTranscript.hasValue())
        << durableTranscript.error().message();

    auto reopenedEngine =
        std::make_unique<creator::mlt_adapter::MltEditEngine>(
            creator::mlt_adapter::MltEditEngineConfig{
                .runtimeRoot = fs::path{CS_TEST_STAGED_MLT_ROOT},
                .previewWidth = 320,
                .previewHeight = 180,
                .audioProcessingFactory = cleanupFactory});
    creator::app::EditorController reopened{std::move(reopenedEngine)};
    reopened.openProject(project.projectUrl());
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return !reopened.busy() && !reopened.sessionBusy();
    }, 15min)) << reopened.statusMessage().toStdString();
    ASSERT_TRUE(reopened.exportSnapshot().has_value());
    EXPECT_EQ(reopened.timelineRevision(), originalRevision);

    const auto projectId = creator::domain::ProjectId::create(
        project.projectId().toUtf8().toStdString());
    ASSERT_TRUE(projectId.hasValue()) << projectId.error().message();
    creator::app::ExportController exporter{
        std::make_unique<creator::app::ProjectExportEngine>(
            fs::path{CS_TEST_STAGED_MLT_ROOT}, cleanupFactory)};
    QSignalSpy finished{&exporter,
                        &creator::app::ExportController::exportFinished};
    const auto cancelledPath = packagePath.parent_path() / "r2-cancelled.mp4";
    const auto finalPath = packagePath.parent_path() / fs::path{u8"R2 최종 강의.mp4"};
    std::error_code ignored;
    fs::remove(cancelledPath, ignored);
    ignored.clear();
    fs::remove(finalPath, ignored);
    const auto preset = creator::edit_engine::RenderPreset::h2641080p30().value();
    auto cancelledRequest = creator::edit_engine::RenderRequest::create(
        projectId.value(), *reopened.exportSnapshot(), cancelledPath, preset,
        creator::edit_engine::RenderOverwritePolicy::FailIfExists);
    ASSERT_TRUE(cancelledRequest.hasValue())
        << cancelledRequest.error().message();
    const auto cancelledJobId = cancelledRequest.value().jobId();
    exporter.setRequest(cancelledRequest.value());
    exporter.startExport();
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return exporter.canCancel() || !exporter.busy();
    }, 120s)) << exporter.statusMessage().toStdString();
    ASSERT_TRUE(exporter.busy())
        << "export completed before the cancellation gate";
    exporter.cancelExport();
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return finished.count() >= 1;
    }, 5min)) << exporter.statusMessage().toStdString();
    EXPECT_FALSE(finished.takeFirst().front().toBool());
    EXPECT_EQ(exporter.state(), static_cast<int>(
                                    creator::edit_engine::RenderJobState::Cancelled));
    EXPECT_FALSE(fs::exists(cancelledPath));
    EXPECT_FALSE(fs::exists(packagePath.parent_path() /
                            (cancelledJobId.value() + ".partial.mp4")));

    auto completedRequest = creator::edit_engine::RenderRequest::create(
        projectId.value(), *reopened.exportSnapshot(), finalPath, preset,
        creator::edit_engine::RenderOverwritePolicy::FailIfExists);
    ASSERT_TRUE(completedRequest.hasValue())
        << completedRequest.error().message();
    const auto completedJobId = completedRequest.value().jobId();
    exporter.setRequest(completedRequest.value());
    exporter.startExport();
    ASSERT_TRUE(waitUntil([&] {
        bounds.sample();
        return finished.count() >= 1;
    }, 90min)) << exporter.statusMessage().toStdString();
    ASSERT_TRUE(finished.takeFirst().front().toBool())
        << exporter.statusMessage().toStdString();
    ASSERT_EQ(exporter.state(), static_cast<int>(
                                    creator::edit_engine::RenderJobState::Completed));
    ASSERT_TRUE(fs::is_regular_file(finalPath));
    EXPECT_FALSE(fs::exists(packagePath.parent_path() /
                            (completedJobId.value() + ".partial.mp4")));

    creator::ffmpeg_adapter::FfmpegMediaProbe probe;
    auto media = probe.probe(finalPath.parent_path(), finalPath.filename());
    ASSERT_TRUE(media.hasValue()) << media.error().message();
    EXPECT_EQ(media.value().codecName, "h264");
    ASSERT_TRUE(media.value().video.has_value());
    ASSERT_TRUE(media.value().audio.has_value());
    EXPECT_EQ(media.value().audio->sampleRate, 48'000);
    EXPECT_EQ(media.value().audio->channels, 2);
    EXPECT_LE(std::abs(media.value().duration.count() -
                       reopened.timelineDurationNs()),
              1'000'000'000LL);

    creator::project_store::ProjectPackageStore packages;
    auto opened = packages.open(packagePath);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    const auto lease = opened.value().databaseIdentityLease;
    ASSERT_TRUE(lease);
    auto jobs = creator::project_store::SqliteRenderJobStore::open(
        opened.value().databasePath, projectId.value(),
        [lease] { return lease->verifyCurrentIdentity(); });
    ASSERT_TRUE(jobs.hasValue()) << jobs.error().message();
    auto cancelledRecord = jobs.value().load(cancelledJobId);
    ASSERT_TRUE(cancelledRecord.hasValue())
        << cancelledRecord.error().message();
    ASSERT_TRUE(cancelledRecord.value().has_value());
    EXPECT_EQ(cancelledRecord.value()->progress.state(),
              creator::edit_engine::RenderJobState::Cancelled);
    auto completedRecord = jobs.value().load(completedJobId);
    ASSERT_TRUE(completedRecord.hasValue())
        << completedRecord.error().message();
    ASSERT_TRUE(completedRecord.value().has_value());
    EXPECT_EQ(completedRecord.value()->progress.state(),
              creator::edit_engine::RenderJobState::Completed);
    EXPECT_TRUE(completedRecord.value()->diagnostics.outputSha256.has_value());
    EXPECT_TRUE(completedRecord.value()->diagnostics.selectedEncoder.has_value());

    auto loudness = measurePublishedAudio(finalPath, media.value(), bounds);
    ASSERT_TRUE(loudness.hasValue()) << loudness.error().message();
    std::cout << "[ R2-07 OUTPUT ] path=" << utf8Path(finalPath)
              << " bytes=" << media.value().byteSize
              << " duration_ns=" << media.value().duration.count()
              << " lufs=" << loudness.value().first
              << " true_peak_dbtp=" << loudness.value().second
              << " sha256=" << media.value().sha256 << std::endl;
    if (std::isfinite(loudness.value().first)) {
        EXPECT_NEAR(loudness.value().first, -14.0, 1.0);
        EXPECT_LE(loudness.value().second, -0.2);
    } else {
        EXPECT_EQ(loudness.value().first,
                  creator::audio_dsp::LoudnessMeter::kNoMeasurement)
            << "only a fully gated/silent program may bypass normalization";
    }

    bounds.sample();
    bounds.print();
    EXPECT_TRUE(bounds.valid);
    EXPECT_LE(bounds.maximumHandles - bounds.initialHandles, 5'000U);
    EXPECT_LE(bounds.maximumWorkingSet - bounds.initialWorkingSet,
              2ULL * 1024ULL * 1024ULL * 1024ULL);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
