#include "app/TimelineEditService.h"
#include "app/EditorSessionTypes.h"

#include "core/Timebase.h"
#include "core/Utc.h"
#include "domain/MediaAsset.h"
#include "domain/ProjectManifest.h"
#include "domain/DeleteRangeCommand.h"
#include "domain/GeneratedClipCommands.h"
#include "domain/SetAudioEnvelopeCommand.h"
#include "domain/SetVisualTransformCommand.h"
#include "domain/SplitClipCommand.h"
#include "domain/TrimClipCommand.h"
#include "project_store/SqliteProjectDatabase.h"
#include "project_store/SqliteTimelineStore.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <locale>
#include <memory>
#include <string>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

std::uint64_t testProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

namespace fs = std::filesystem;

using creator::app::TimelineEditService;
using creator::app::EditorEditKind;
using creator::app::EditorEditRequest;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::core::Utc;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::AudioEnvelope;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CaptionCue;
using creator::domain::CommandId;
using creator::domain::CueId;
using creator::domain::DeleteRangeCommand;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::ProjectId;
using creator::domain::ProjectManifest;
using creator::domain::RgbaColor;
using creator::domain::SetAudioEnvelopeCommand;
using creator::domain::SetVisualTransformCommand;
using creator::domain::SplitClipCommand;
using creator::domain::TimeRange;
using creator::domain::TextAlignment;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::TitlePayload;
using creator::domain::TrimClipCommand;
using creator::domain::TrimEdge;
using creator::domain::VideoAssetMetadata;
using creator::domain::VisualTransform;
using creator::project_store::SqliteProjectDatabase;
using creator::project_store::SqliteTimelineStore;

Utc fixedUtc() {
    return Utc::parseRfc3339("2026-07-17T12:00:00Z").value();
}

ProjectId projectId() {
    return ProjectId::create("123e4567-e89b-42d3-a456-426614174100").value();
}

ProjectManifest manifest() {
    return ProjectManifest{
        .schemaVersion = ProjectManifest::kCurrentSchemaVersion,
        .projectId = projectId(),
        .name = "Durable timeline",
        .createdAt = fixedUtc(),
        .updatedAt = fixedUtc(),
        .canvas = {},
        .database = std::string{ProjectManifest::kDatabaseFileName},
        .directories = {},
        .requiredFeatures = {},
    };
}

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

MediaAsset videoAsset() {
    return MediaAsset::create(
               AssetId::create("screen-asset").value(), MediaKind::Video,
               "media/screen.mkv", DurationNs{1'000},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 42'000, "screen-fingerprint",
               AssetAvailability::Available)
        .value();
}

MediaAsset cameraAsset() {
    return MediaAsset::create(
               AssetId::create("camera-asset").value(), MediaKind::Video,
               "media/camera.mkv", DurationNs{1'000},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 21'000, "camera-fingerprint",
               AssetAvailability::Available)
        .value();
}

MediaAsset microphoneAsset() {
    return MediaAsset::create(
               AssetId::create("microphone-asset").value(), MediaKind::Audio,
               "audio/microphone.mka", DurationNs{1'000}, std::nullopt,
               AudioAssetMetadata{.sampleRate = 48'000, .channels = 2}, 8'000,
               "microphone-fingerprint", AssetAvailability::Available)
        .value();
}

Timeline initialTimeline() {
    auto result = Timeline::create(TimelineId::create("main").value(), "Main",
                                   FrameRate::create(60, 1).value());
    EXPECT_TRUE(result.hasValue());
    auto timeline = std::move(result).value();
    const auto trackId = TrackId::create("screen-track").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(trackId, TrackKind::Video, "Screen", true,
                                          false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            trackId,
                            Clip::createAsset(
                                ClipId::create("screen-clip").value(), videoAsset(),
                                TimeRange::create(at(100), DurationNs{600}).value(),
                                TimeRange::create(at(0), DurationNs{600}).value(), true,
                                std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    return timeline;
}

Timeline recordingTimeline() {
    auto timeline = Timeline::create(TimelineId::create("main").value(), "Recording",
                                     FrameRate::create(60, 1).value())
                        .value();
    const auto screenTrack = TrackId::create("screen-track").value();
    const auto cameraTrack = TrackId::create("camera-track").value();
    const auto microphoneTrack = TrackId::create("microphone-track").value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(screenTrack, TrackKind::Video, "Screen", true,
                                          false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(cameraTrack, TrackKind::Video, "Camera", true,
                                          false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(microphoneTrack, TrackKind::Audio,
                                          "Microphone", true, false)
                                .value())
                    .hasValue());
    const auto full = TimeRange::create(at(0), DurationNs{1'000}).value();
    const auto screenTransform = VisualTransform::create(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0)
                                     .value();
    const auto cameraTransform = VisualTransform::create(
        0.72, 0.68, 0.25, 0.25, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.95, 10)
                                     .value();
    const auto microphoneEnvelope = AudioEnvelope::create(
        -3.0, DurationNs{25}, DurationNs{50}, full.duration())
                                        .value();
    EXPECT_TRUE(timeline.insertClip(
                            screenTrack,
                            Clip::createAsset(
                                ClipId::create("screen-clip").value(), videoAsset(),
                                full, full, true, screenTransform, std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            cameraTrack,
                            Clip::createAsset(
                                ClipId::create("camera-clip").value(), cameraAsset(),
                                full, full, true, cameraTransform, std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            microphoneTrack,
                            Clip::createAsset(
                                ClipId::create("microphone-clip").value(),
                                microphoneAsset(), full, full, true, std::nullopt,
                                microphoneEnvelope)
                                .value())
                    .hasValue());
    return timeline;
}

Timeline signedZeroTimeline() {
    auto timeline = initialTimeline();
    const auto transform = VisualTransform::create(
        -0.0, 0.25, 1.0, 0.75, 1.0, 1.0, -0.0, -0.0, 0.0, 0.0, -0.0,
        1.0, 0)
                               .value();
    const auto* original = timeline.clip(
        TrackId::create("screen-track").value(),
        ClipId::create("screen-clip").value());
    EXPECT_NE(original, nullptr);
    auto replacement = Clip::createAsset(
        original->id(), videoAsset(), original->sourceRange(),
        original->timelineRange(), true, transform, std::nullopt);
    EXPECT_TRUE(replacement.hasValue());
    EXPECT_TRUE(timeline.replaceClip(
                            TrackId::create("screen-track").value(),
                            ClipId::create("screen-clip").value(),
                            std::move(replacement).value())
                    .hasValue());
    return timeline;
}

class CommaDecimal final : public std::numpunct<char> {
protected:
    [[nodiscard]] char do_decimal_point() const override { return ','; }
};

class ScopedGlobalLocale final {
public:
    ScopedGlobalLocale()
        : previous_(std::locale()),
          active_(std::locale::global(
              std::locale{std::locale::classic(), new CommaDecimal})) {}
    ~ScopedGlobalLocale() { std::locale::global(previous_); }

private:
    std::locale previous_;
    std::locale active_;
};

class TempProject final {
public:
    TempProject() {
        path_ = fs::temp_directory_path() /
                ("creator-studio-timeline-service-" +
                 std::to_string(testProcessId()) + "-" +
                 std::to_string(++nextId_) + ".sqlite3");
        fs::remove(path_);
        EXPECT_TRUE(SqliteProjectDatabase::create(path_, manifest()).hasValue());
    }

    ~TempProject() { fs::remove(path_); }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    inline static std::uint64_t nextId_{0};
    fs::path path_;
};

struct SequentialIds final {
    std::string operator()() { return "event-" + std::to_string(++next); }
    std::uint64_t next{0};
};

TitlePayload generatedTitle(std::string text) {
    return TitlePayload::create(
               std::move(text), "Malgun Gothic", 0.5, 0.9,
               RgbaColor::parse("#ffffffff").value(),
               RgbaColor::parse("#00000080").value(),
               TextAlignment::Center)
        .value();
}

CaptionCue generatedCue(std::string id, std::int64_t start,
                         std::int64_t duration, std::string text) {
    return CaptionCue::create(CueId::create(std::move(id)).value(),
                              DurationNs{start}, DurationNs{duration},
                              std::move(text)).value();
}

TEST(TimelineEditServiceTest, ExposesDurableHistoryCapabilities) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue()) << serviceResult.error().message();
    auto service = std::move(serviceResult).value();

    EXPECT_FALSE(service.canUndo());
    EXPECT_FALSE(service.canRedo());
    EXPECT_EQ(service.historyCursor(), 0U);

    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-capability").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("screen-right").value(), at(250)))
                    .hasValue());
    EXPECT_TRUE(service.canUndo());
    EXPECT_FALSE(service.canRedo());
    EXPECT_EQ(service.historyCursor(), 1U);

    ASSERT_TRUE(service.undo().hasValue());
    EXPECT_FALSE(service.canUndo());
    EXPECT_TRUE(service.canRedo());
    EXPECT_EQ(service.historyCursor(), 0U);
}

TEST(TimelineEditServiceTest, ReopensEveryEffectAndGeneratedCommandExactly) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.putAsset(cameraAsset()).hasValue());
    ASSERT_TRUE(store.putAsset(microphoneAsset()).hasValue());
    const auto initial = recordingTimeline();
    ASSERT_TRUE(store.createTimeline(initial).hasValue());
    SequentialIds ids;
    auto opened = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto service = std::move(opened).value();

    const auto cameraTransform = VisualTransform::create(
        0.66, 0.56, 0.30, 0.40, 1.0, 1.0, 12.0,
        0.1, 0.0, 0.0, 0.1, 0.75, 20).value();
    ASSERT_TRUE(service.execute(std::make_unique<SetVisualTransformCommand>(
        CommandId::create("visual").value(),
        TrackId::create("camera-track").value(),
        ClipId::create("camera-clip").value(), cameraTransform)).hasValue());
    const auto envelope = AudioEnvelope::create(
        -6.0, DurationNs{100}, DurationNs{150}, DurationNs{1'000}).value();
    ASSERT_TRUE(service.execute(std::make_unique<SetAudioEnvelopeCommand>(
        CommandId::create("audio").value(),
        TrackId::create("microphone-track").value(),
        ClipId::create("microphone-clip").value(), envelope)).hasValue());

    const auto titleTrack = TrackId::create("title-1").value();
    ASSERT_TRUE(service.execute(std::make_unique<creator::domain::AddTitleCommand>(
        CommandId::create("add-title-a").value(), titleTrack, "Titles",
        Clip::createTitle(
            ClipId::create("title-a").value(),
            TimeRange::create(at(100), DurationNs{300}).value(), true,
            generatedTitle("원래 제목"), std::nullopt).value())).hasValue());
    ASSERT_TRUE(service.execute(std::make_unique<creator::domain::EditTitleCommand>(
        CommandId::create("edit-title").value(), titleTrack,
        ClipId::create("title-a").value(), generatedTitle("수정 제목"))).hasValue());
    ASSERT_TRUE(service.execute(std::make_unique<creator::domain::AddTitleCommand>(
        CommandId::create("add-title-b").value(), titleTrack, "Titles",
        Clip::createTitle(
            ClipId::create("title-b").value(),
            TimeRange::create(at(500), DurationNs{200}).value(), true,
            generatedTitle("삭제 제목"), std::nullopt).value())).hasValue());
    ASSERT_TRUE(service.execute(
        std::make_unique<creator::domain::RemoveGeneratedClipCommand>(
            CommandId::create("remove-title-b").value(), titleTrack,
            ClipId::create("title-b").value())).hasValue());

    const auto captionTrack = TrackId::create("caption-1").value();
    const auto captionClip = ClipId::create("caption-a").value();
    const auto captionSpan = TimeRange::create(at(0), DurationNs{1'000}).value();
    ASSERT_TRUE(service.execute(
        std::make_unique<creator::domain::AddCaptionCueCommand>(
            CommandId::create("add-cue-a").value(), captionTrack, "Captions",
            captionClip, captionSpan, true, std::nullopt,
            generatedCue("cue-a", 0, 200, "첫 자막"))).hasValue());
    ASSERT_TRUE(service.execute(
        std::make_unique<creator::domain::AddCaptionCueCommand>(
            CommandId::create("add-cue-b").value(), captionTrack, "Captions",
            captionClip, captionSpan, true, std::nullopt,
            generatedCue("cue-b", 300, 200, "둘째 자막"))).hasValue());
    ASSERT_TRUE(service.execute(
        std::make_unique<creator::domain::EditCaptionCueCommand>(
            CommandId::create("edit-cue-b").value(), captionTrack, captionClip,
            CueId::create("cue-b").value(),
            generatedCue("cue-b", 350, 250, "수정 자막"))).hasValue());
    ASSERT_TRUE(service.execute(
        std::make_unique<creator::domain::RemoveCaptionCueCommand>(
            CommandId::create("remove-cue-a").value(), captionTrack, captionClip,
            CueId::create("cue-a").value())).hasValue());
    const auto final = service.snapshot();
    EXPECT_EQ(service.historyCursor(), 10U);

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    auto reopened = std::move(reopenedResult).value();
    EXPECT_EQ(reopened.snapshot(), final);
    for (int index = 0; index < 10; ++index) {
        ASSERT_TRUE(reopened.undo().hasValue()) << index;
    }
    EXPECT_EQ(reopened.snapshot(), initial);

    auto undoneStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(undoneStoreResult.hasValue());
    auto undoneStore = std::move(undoneStoreResult).value();
    auto undoneResult = TimelineEditService::open(
        undoneStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(undoneResult.hasValue()) << undoneResult.error().message();
    auto undone = std::move(undoneResult).value();
    EXPECT_EQ(undone.snapshot(), initial);
    EXPECT_EQ(undone.historyCursor(), 0U);
    for (int index = 0; index < 10; ++index) {
        ASSERT_TRUE(undone.redo().hasValue()) << index;
    }
    EXPECT_EQ(undone.snapshot(), final);
}

TEST(EditorSessionTypesTest, EditRequestDefaultsAreClosedAndNonRipple) {
    const EditorEditRequest request{.kind = EditorEditKind::Split};

    EXPECT_FALSE(request.trackId.has_value());
    EXPECT_FALSE(request.clipId.has_value());
    EXPECT_EQ(request.position, TimestampNs{});
    EXPECT_FALSE(request.range.has_value());
    EXPECT_FALSE(request.ripple);
}

TEST(TimelineEditServiceTest, ReopensAtEveryCursorAndKeepsUndoRedoAndCleanState) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue()) << serviceResult.error().message();
    auto service = std::move(serviceResult).value();
    EXPECT_EQ(service.revision(), 0);
    EXPECT_TRUE(service.isClean());

    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-1").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("screen-right").value(), at(250)))
                    .hasValue());
    EXPECT_EQ(service.revision(), 1);
    EXPECT_FALSE(service.isClean());

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    auto reopened = std::move(reopenedResult).value();
    EXPECT_EQ(reopened.snapshot(), service.snapshot());
    EXPECT_FALSE(reopened.isClean());

    ASSERT_TRUE(reopened.undo().hasValue());
    EXPECT_EQ(reopened.revision(), 2);
    EXPECT_EQ(reopened.snapshot(), initialTimeline());
    EXPECT_TRUE(reopened.isClean());

    auto undoneStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(undoneStoreResult.hasValue());
    auto undoneStore = std::move(undoneStoreResult).value();
    auto undoneResult = TimelineEditService::open(
        undoneStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(undoneResult.hasValue()) << undoneResult.error().message();
    auto undone = std::move(undoneResult).value();
    EXPECT_EQ(undone.snapshot(), initialTimeline());
    EXPECT_TRUE(undone.isClean());

    ASSERT_TRUE(undone.redo().hasValue());
    EXPECT_EQ(undone.revision(), 3);
    ASSERT_TRUE(undone.markExplicitSave().hasValue());
    EXPECT_TRUE(undone.isClean());

    auto savedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(savedStoreResult.hasValue());
    auto savedStore = std::move(savedStoreResult).value();
    auto savedResult = TimelineEditService::open(
        savedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(savedResult.hasValue()) << savedResult.error().message();
    EXPECT_EQ(savedResult.value().snapshot(), undone.snapshot());
    EXPECT_EQ(savedResult.value().revision(), 3);
    EXPECT_TRUE(savedResult.value().isClean());
}

TEST(TimelineEditServiceTest, FailedDurableCommitLeavesMemoryExactlyUnchanged) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    auto serviceResult = TimelineEditService::open(
        store, 100, [] { return std::string{"duplicate-event"}; },
        [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue());
    auto service = std::move(serviceResult).value();
    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-1").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("screen-right").value(), at(250)))
                    .hasValue());

    const auto before = service.snapshot();
    const auto beforeRevision = service.revision();
    const auto failed = service.execute(std::make_unique<TrimClipCommand>(
        CommandId::create("trim-1").value(),
        TrackId::create("screen-track").value(),
        ClipId::create("screen-right").value(), TrimEdge::Trailing, at(500)));
    ASSERT_FALSE(failed.hasValue());
    EXPECT_EQ(service.snapshot(), before);
    EXPECT_EQ(service.revision(), beforeRevision);

    auto persisted = store.loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue());
    EXPECT_EQ(persisted.value().timeline, before);
    EXPECT_EQ(persisted.value().revision, beforeRevision);
}

TEST(TimelineEditServiceTest, NewEditAfterUndoPersistsWithoutTheOldRedoBranch) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue());
    auto service = std::move(serviceResult).value();
    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-branch").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("discarded-right").value(), at(250)))
                    .hasValue());
    ASSERT_TRUE(service.undo().hasValue());
    ASSERT_TRUE(service.execute(std::make_unique<TrimClipCommand>(
                                    CommandId::create("trim-branch").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    TrimEdge::Trailing, at(500)))
                    .hasValue());
    EXPECT_FALSE(service.redo().hasValue());
    const auto branched = service.snapshot();

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    EXPECT_EQ(reopenedResult.value().snapshot(), branched);
    EXPECT_EQ(reopenedResult.value().revision(), 3);
    EXPECT_FALSE(reopenedResult.value().redo().hasValue());
}

TEST(TimelineEditServiceTest, RecordingEditWorkflowSurvivesCloseAndReopenExactly) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.putAsset(cameraAsset()).hasValue());
    ASSERT_TRUE(store.putAsset(microphoneAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(recordingTimeline()).hasValue());

    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue());
    auto service = std::move(serviceResult).value();
    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("split-mistake").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("screen-right").value(), at(400)))
                    .hasValue());
    ASSERT_TRUE(service.execute(std::make_unique<DeleteRangeCommand>(
                                    CommandId::create("delete-mistake").value(),
                                    TimeRange::create(at(400), DurationNs{100}).value(),
                                    true,
                                    std::vector<ClipId>{
                                        ClipId::create("camera-right").value(),
                                        ClipId::create("microphone-right").value()}))
                    .hasValue());
    ASSERT_TRUE(service.execute(std::make_unique<TrimClipCommand>(
                                    CommandId::create("trim-camera").value(),
                                    TrackId::create("camera-track").value(),
                                    ClipId::create("camera-right").value(),
                                    TrimEdge::Trailing, at(800)))
                    .hasValue());
    ASSERT_TRUE(service.undo().hasValue());
    ASSERT_TRUE(service.redo().hasValue());
    ASSERT_TRUE(service.markExplicitSave().hasValue());
    const auto finalSnapshot = service.snapshot();
    EXPECT_EQ(service.revision(), 5);
    EXPECT_TRUE(service.isClean());

    auto persisted = store.loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue());
    EXPECT_EQ(persisted.value().timeline, finalSnapshot);
    EXPECT_EQ(persisted.value().revision, 5);
    EXPECT_EQ(persisted.value().historyCount, 3U);
    EXPECT_EQ(persisted.value().historyCursor, 3U);
    ASSERT_TRUE(persisted.value().cleanCursor.has_value());
    EXPECT_EQ(*persisted.value().cleanCursor, 3U);
    EXPECT_EQ(persisted.value().explicitSavedRevision, 5);
    EXPECT_EQ(persisted.value().events.size(), 5U);

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    auto reopened = std::move(reopenedResult).value();
    EXPECT_EQ(reopened.snapshot(), finalSnapshot);
    EXPECT_EQ(reopened.revision(), 5);
    EXPECT_TRUE(reopened.isClean());
    ASSERT_TRUE(reopened.undo().hasValue());
    ASSERT_TRUE(reopened.undo().hasValue());
    ASSERT_TRUE(reopened.redo().hasValue());
    ASSERT_TRUE(reopened.redo().hasValue());
    EXPECT_EQ(reopened.snapshot(), finalSnapshot);
    EXPECT_EQ(reopened.revision(), 9);
    EXPECT_TRUE(reopened.isClean());
}

TEST(TimelineEditServiceTest, BoundedHistorySurvivesReopenWithoutRevivingEvictedEdits) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(initialTimeline()).hasValue());

    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 2, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue());
    auto service = std::move(serviceResult).value();
    ASSERT_TRUE(service.execute(std::make_unique<TrimClipCommand>(
                                    CommandId::create("trim-oldest").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    TrimEdge::Trailing, at(550)))
                    .hasValue());
    const auto afterEvictedEdit = service.snapshot();
    ASSERT_TRUE(service.execute(std::make_unique<TrimClipCommand>(
                                    CommandId::create("trim-middle").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    TrimEdge::Trailing, at(500)))
                    .hasValue());
    ASSERT_TRUE(service.execute(std::make_unique<TrimClipCommand>(
                                    CommandId::create("trim-newest").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    TrimEdge::Trailing, at(450)))
                    .hasValue());

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 2, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    auto reopened = std::move(reopenedResult).value();
    ASSERT_TRUE(reopened.undo().hasValue());
    ASSERT_TRUE(reopened.undo().hasValue());
    EXPECT_EQ(reopened.snapshot(), afterEvictedEdit);
    EXPECT_FALSE(reopened.undo().hasValue());

    auto undoneStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(undoneStoreResult.hasValue());
    auto undoneStore = std::move(undoneStoreResult).value();
    auto undoneResult = TimelineEditService::open(
        undoneStore, 2, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(undoneResult.hasValue()) << undoneResult.error().message();
    EXPECT_EQ(undoneResult.value().snapshot(), afterEvictedEdit);
    EXPECT_FALSE(undoneResult.value().undo().hasValue());
}

TEST(TimelineEditServiceTest, CanonicalCommandNumbersSurviveLocaleAndSignedZeroReopens) {
    TempProject project;
    auto storeResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(storeResult.hasValue());
    auto store = std::move(storeResult).value();
    ASSERT_TRUE(store.putAsset(videoAsset()).hasValue());
    ASSERT_TRUE(store.createTimeline(signedZeroTimeline()).hasValue());

    ScopedGlobalLocale locale;
    SequentialIds ids;
    auto serviceResult = TimelineEditService::open(
        store, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(serviceResult.hasValue());
    auto service = std::move(serviceResult).value();
    ASSERT_TRUE(service.execute(std::make_unique<SplitClipCommand>(
                                    CommandId::create("signed-zero-split").value(),
                                    TrackId::create("screen-track").value(),
                                    ClipId::create("screen-clip").value(),
                                    ClipId::create("screen-right").value(), at(250)))
                    .hasValue());

    auto reopenedStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(reopenedStoreResult.hasValue());
    auto reopenedStore = std::move(reopenedStoreResult).value();
    auto reopenedResult = TimelineEditService::open(
        reopenedStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(reopenedResult.hasValue()) << reopenedResult.error().message();
    auto reopened = std::move(reopenedResult).value();
    ASSERT_TRUE(reopened.undo().hasValue());

    auto undoneStoreResult = SqliteTimelineStore::open(project.path(), projectId());
    ASSERT_TRUE(undoneStoreResult.hasValue());
    auto undoneStore = std::move(undoneStoreResult).value();
    auto undoneResult = TimelineEditService::open(
        undoneStore, 100, [&ids] { return ids(); }, [] { return fixedUtc(); });
    ASSERT_TRUE(undoneResult.hasValue()) << undoneResult.error().message();
    ASSERT_TRUE(undoneResult.value().redo().hasValue());
}

}  // namespace
