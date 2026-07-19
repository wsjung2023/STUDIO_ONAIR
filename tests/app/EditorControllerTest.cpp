#include "app/EditorController.h"
#include "app/ProjectController.h"
#include "app/ProjectEditorBinding.h"

#include "core/AppError.h"
#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/SplitClipCommand.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "edit_engine/EditEngineTypes.h"
#include "edit_engine/IEditEngine.h"
#include "fakes/FakeEditEngine.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteTimelineStore.h"
#include "project_store/internal/SqliteConnection.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

using creator::app::EditorController;
using creator::app::ProjectController;
using creator::app::bindProjectEditor;
using creator::app::MediaBinModel;
using creator::app::TimelineTrackModel;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioEnvelope;
using creator::domain::AudioAssetMetadata;
using creator::domain::CaptionCue;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::CommandId;
using creator::domain::CueId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::RgbaColor;
using creator::domain::SplitClipCommand;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::TimelineRevision;
using creator::domain::TitlePayload;
using creator::domain::TextAlignment;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;
using creator::domain::VisualTransform;
using creator::edit_engine::IEditEngine;
using creator::edit_engine::IRenderJob;
using creator::edit_engine::GeneratedOverlayDescriptor;
using creator::edit_engine::PreviewFrame;
using creator::edit_engine::RenderRequest;
using creator::edit_engine::TimelineChangeSet;
using creator::edit_engine::TimelineSnapshot;
using creator::fakes::FakeEditEngine;
using creator::fakes::FakeEditOperation;
using creator::project_store::ProjectPackageStore;
using creator::project_store::SqliteTimelineStore;

namespace fs = std::filesystem;

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

MediaAsset asset(std::string id = "screen") {
    const std::string path = "media/" + id + ".mp4";
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), MediaKind::Video,
               path, DurationNs{5'000'000'000},
               VideoAssetMetadata{1920, 1080, FrameRate::create(60, 1).value()},
               std::nullopt, 42'000, "fingerprint",
               AssetAvailability::Available)
        .value();
}

MediaAsset audioAsset(std::string id) {
    const std::string path = "media/" + id + ".wav";
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), MediaKind::Audio, path,
               DurationNs{5'000'000'000}, std::nullopt,
               AudioAssetMetadata{48'000, 2}, 21'000, "audio-fingerprint",
               AssetAvailability::Available)
        .value();
}

TimelineSnapshot snapshot(std::int64_t revision, std::string name,
                          std::string trackName = "Screen") {
    auto timeline = Timeline::create(TimelineId::create("main").value(),
                                     std::move(name),
                                     FrameRate::create(60, 1).value())
                        .value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("v1").value(),
                                          TrackKind::Video,
                                          std::move(trackName), true, false)
                                .value())
                    .hasValue());
    return TimelineSnapshot{std::move(timeline),
                            TimelineRevision::create(revision).value()};
}

TimelineSnapshot snapshotWithVideo(std::int64_t revision,
                                   DurationNs duration) {
    auto video = asset();
    auto value = snapshot(revision, "Timed timeline");
    const auto range = TimeRange::create(TimestampNs{}, duration).value();
    EXPECT_TRUE(value.timeline.insertClip(
                                  TrackId::create("v1").value(),
                                  Clip::createAsset(
                                      ClipId::create("timed-clip").value(),
                                      video, range, range, true, std::nullopt,
                                      std::nullopt)
                                      .value())
                    .hasValue());
    value.assets = {std::move(video)};
    return value;
}

TimelineSnapshot snapshotWithInspectorValues(std::int64_t revision,
                                             bool lockGenerated = false,
                                             bool lockVideo = false) {
    auto video = asset();
    auto audio = audioAsset("microphone");
    auto timeline = Timeline::create(TimelineId::create("main").value(),
                                     "Inspector timeline",
                                     FrameRate::create(60, 1).value())
                        .value();
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("v1").value(),
                                          TrackKind::Video, "Video", true,
                                          false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("a1").value(),
                                          TrackKind::Audio, "Audio", true,
                                          false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("title-1").value(),
                                          TrackKind::Title, "Titles", true,
                                          false)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.addTrack(
                            Track::create(TrackId::create("caption-1").value(),
                                          TrackKind::Caption, "Captions", true,
                                          false)
                                .value())
                    .hasValue());
    const auto range =
        TimeRange::create(TimestampNs{}, DurationNs{1'000'000'000}).value();
    const auto transform = VisualTransform::create(
                               0.1, 0.2, 0.3, 0.4, 1.0, 1.0, 5.0, 0.01,
                               0.02, 0.03, 0.04, 0.8, 7)
                               .value();
    const auto envelope =
        AudioEnvelope::create(-4.0, DurationNs{100'000'000},
                              DurationNs{200'000'000}, range.duration())
            .value();
    EXPECT_TRUE(timeline.insertClip(
                            TrackId::create("v1").value(),
                            Clip::createAsset(ClipId::create("video").value(),
                                              video, range, range, true,
                                              transform, std::nullopt)
                                .value())
                    .hasValue());
    EXPECT_TRUE(timeline.insertClip(
                            TrackId::create("a1").value(),
                            Clip::createAsset(ClipId::create("audio").value(),
                                              audio, range, range, true,
                                              std::nullopt, envelope)
                                .value())
                    .hasValue());
    const auto title = TitlePayload::create(
                           "Inspector title", "Noto Sans", 0.25, 0.1,
                           RgbaColor::parse("#ffffffff").value(),
                           RgbaColor::parse("#00000080").value(),
                           TextAlignment::Center)
                           .value();
    EXPECT_TRUE(timeline.insertClip(
                            TrackId::create("title-1").value(),
                            Clip::createTitle(ClipId::create("title").value(),
                                              range, true, title, std::nullopt)
                                .value())
                    .hasValue());
    const auto cue = CaptionCue::create(
                         CueId::create("cue-1").value(), DurationNs{10},
                         DurationNs{100}, "Caption text")
                         .value();
    EXPECT_TRUE(timeline.insertClip(
                            TrackId::create("caption-1").value(),
                            Clip::createCaption(
                                ClipId::create("caption").value(), range, true,
                                {cue}, std::nullopt)
                                .value())
                    .hasValue());
    if (lockGenerated) {
        EXPECT_TRUE(timeline.setTrackLocked(
                                TrackId::create("title-1").value(), true)
                        .hasValue());
        EXPECT_TRUE(timeline.setTrackLocked(
                                TrackId::create("caption-1").value(), true)
                        .hasValue());
    }
    if (lockVideo) {
        EXPECT_TRUE(timeline.setTrackLocked(
                                TrackId::create("v1").value(), true)
                        .hasValue());
    }
    auto snapshot = TimelineSnapshot{
        std::move(timeline), TimelineRevision::create(revision).value(),
        {std::move(video), std::move(audio)}};
    snapshot.generatedOverlays.push_back(
        GeneratedOverlayDescriptor::create(
            ClipId::create("title").value(), std::nullopt,
            fs::path{"cache/generated/title.png"}, range,
            "Noto Sans CJK KR")
            .value());
    return snapshot;
}

class DurableControllerPackage final {
public:
    explicit DurableControllerPackage(bool inspectorValues = false,
                                      bool lockGenerated = false,
                                      bool lockVideo = false) {
        root_ = fs::temp_directory_path() /
                ("creator-studio-controller-durable-" +
                 std::to_string(testProcessId()) + "-" +
                 std::to_string(++nextId_));
        package_ = root_ / fs::path{L"편집-세션.cstudio"};
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_);
        ProjectPackageStore packageStore;
        auto created = packageStore.create(package_, "Controller durable");
        EXPECT_TRUE(created.hasValue())
            << (created.hasValue() ? "" : created.error().message());
        if (!created.hasValue()) return;
        const auto& manifest = created.value().package.manifest;
        database_ = package_ / manifest.database;
        auto storeResult = SqliteTimelineStore::open(
            database_, manifest.projectId);
        EXPECT_TRUE(storeResult.hasValue())
            << (storeResult.hasValue() ? "" : storeResult.error().message());
        if (!storeResult.hasValue()) return;
        auto store = std::move(storeResult).value();
        if (inspectorValues) {
            auto seeded = snapshotWithInspectorValues(
                0, lockGenerated, lockVideo);
            for (const auto& media : seeded.assets) {
                EXPECT_TRUE(store.putAsset(media).hasValue());
            }
            EXPECT_TRUE(store.createTimeline(seeded.timeline).hasValue());
        } else {
            EXPECT_TRUE(store.putAsset(asset()).hasValue());
            EXPECT_TRUE(store.createTimeline(
                                 snapshotWithVideo(0, DurationNs{1'000}).timeline)
                            .hasValue());
        }
    }

    ~DurableControllerPackage() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    [[nodiscard]] QUrl url() const {
        return QUrl::fromLocalFile(QString::fromStdWString(package_.wstring()));
    }
    [[nodiscard]] const fs::path& root() const noexcept { return root_; }

    void blockGeneratedCacheWrites() const {
        const auto generated = package_ / "cache" / "generated";
        std::error_code error;
        fs::remove_all(generated, error);
        ASSERT_FALSE(error) << error.message();
        QFile blocker{QString::fromStdWString(generated.wstring())};
        ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
        ASSERT_EQ(blocker.write("blocked"), 7);
    }

    void rejectEditCommits() const {
        auto connection =
            creator::project_store::internal::SqliteConnection::open(database_);
        ASSERT_TRUE(connection.hasValue()) << connection.error().message();
        ASSERT_TRUE(connection.value()
                        .execute(
                            "CREATE TRIGGER reject_controller_edit "
                            "BEFORE INSERT ON edit_commands "
                            "BEGIN SELECT RAISE(ABORT, 'injected controller "
                            "commit failure'); END")
                        .hasValue());
    }

private:
    inline static std::uint64_t nextId_{0};
    fs::path root_;
    fs::path package_;
    fs::path database_;
};

TEST(EditorControllerDurableTest, PublishesSelectionAndNormalizesMarkedRange) {
    auto engine = std::make_unique<FakeEditEngine>();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshotWithVideo(1, DurationNs{1'000}));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));

    controller.selectClip(QStringLiteral("missing-track"),
                          QStringLiteral("missing-clip"));
    EXPECT_TRUE(controller.selectedTrackId().isEmpty());
    EXPECT_TRUE(controller.selectedClipId().isEmpty());

    controller.selectClip(QStringLiteral("v1"),
                          QStringLiteral("timed-clip"));
    EXPECT_EQ(controller.selectedTrackId(), QStringLiteral("v1"));
    EXPECT_EQ(controller.selectedClipId(), QStringLiteral("timed-clip"));
    EXPECT_EQ(controller.selectedClipStartNs(), 0);
    EXPECT_EQ(controller.selectedClipEndNs(), 1'000);

    controller.seek(800);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeOut();
    controller.seek(200);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeIn();

    EXPECT_TRUE(controller.hasMarkedRange());
    EXPECT_EQ(controller.rangeInNs(), 200);
    EXPECT_EQ(controller.rangeOutNs(), 800);
    EXPECT_TRUE(controller.clean());
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    EXPECT_FALSE(controller.sessionBusy());
}

TEST(EditorControllerDurableTest,
     PublishesTypedInspectorValuesAndCompatibilityFromCommittedSelection) {
    EditorController controller{std::make_unique<FakeEditEngine>()};
    controller.openSession({}, snapshotWithInspectorValues(1));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));

    controller.selectClip(QStringLiteral("v1"), QStringLiteral("video"));
    EXPECT_EQ(controller.selectedClipKind(), QStringLiteral("asset"));
    EXPECT_TRUE(controller.selectedVisualCompatible());
    EXPECT_FALSE(controller.selectedAudioCompatible());
    EXPECT_DOUBLE_EQ(
        controller.selectedVisualTransform()
            .value(QStringLiteral("rotationDegrees"))
            .toDouble(),
        5.0);
    EXPECT_EQ(controller.selectedPipPreset(), QStringLiteral("custom"));

    controller.selectClip(QStringLiteral("a1"), QStringLiteral("audio"));
    EXPECT_FALSE(controller.selectedVisualCompatible());
    EXPECT_TRUE(controller.selectedAudioCompatible());
    EXPECT_DOUBLE_EQ(controller.selectedAudioEnvelope()
                         .value(QStringLiteral("gainDb"))
                         .toDouble(),
                     -4.0);

    controller.selectClip(QStringLiteral("title-1"),
                          QStringLiteral("title"));
    EXPECT_EQ(controller.selectedClipKind(), QStringLiteral("title"));
    EXPECT_EQ(controller.selectedTitlePayload()
                  .value(QStringLiteral("text"))
                  .toString(),
              QStringLiteral("Inspector title"));
    EXPECT_EQ(controller.selectedResolvedFontFamily(),
              QStringLiteral("Noto Sans CJK KR"));

    controller.selectClip(QStringLiteral("caption-1"),
                          QStringLiteral("caption"));
    EXPECT_EQ(controller.selectedClipKind(), QStringLiteral("caption"));
    ASSERT_EQ(controller.selectedCaptionCues().size(), 1);
    EXPECT_EQ(controller.selectedCaptionCues()
                  .front()
                  .toMap()
                  .value(QStringLiteral("cueId"))
                  .toString(),
              QStringLiteral("cue-1"));

    controller.openProject(QUrl::fromLocalFile(
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/missing-inspector-project.cstudio")));
    EXPECT_TRUE(controller.selectedClipKind().isEmpty());
    EXPECT_FALSE(controller.selectedVisualCompatible());
    EXPECT_TRUE(controller.selectedVisualTransform().isEmpty());
    EXPECT_TRUE(controller.selectedCaptionCues().isEmpty());
    EXPECT_TRUE(controller.selectedResolvedFontFamily().isEmpty());
}

TEST(EditorControllerDurableTest,
     MapsTypedInspectorSubmissionsToOneDurableRevisionEach) {
    DurableControllerPackage package{true};
    EditorController controller{std::make_unique<FakeEditEngine>()};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 0;
    }));
    const auto waitRevision = [&](qlonglong revision) {
        return waitUntil([&] {
            return !controller.sessionBusy() && !controller.busy() &&
                   controller.timelineRevision() == revision;
        });
    };

    controller.selectClip(QStringLiteral("v1"), QStringLiteral("video"));
    controller.applySelectedVisualTransform(
        0.2, 0.1, 0.4, 0.3, 1.0, 1.0, 12.0, 0.01, 0.02, 0.03, 0.04,
        0.75, 9);
    ASSERT_TRUE(waitRevision(1));
    EXPECT_DOUBLE_EQ(controller.selectedVisualTransform()
                         .value(QStringLiteral("rotationDegrees"))
                         .toDouble(),
                     12.0);
    controller.applySelectedPipPreset(QStringLiteral("bottomRight"));
    ASSERT_TRUE(waitRevision(2));
    EXPECT_EQ(controller.selectedPipPreset(), QStringLiteral("bottomRight"));
    controller.resetSelectedVisualTransform();
    ASSERT_TRUE(waitRevision(3));
    EXPECT_TRUE(controller.selectedVisualTransform().isEmpty());
    EXPECT_EQ(controller.selectedPipPreset(), QStringLiteral("fullFrame"));

    controller.selectClip(QStringLiteral("a1"), QStringLiteral("audio"));
    controller.applySelectedAudioEnvelope(-6.0, 50'000'000, 75'000'000);
    ASSERT_TRUE(waitRevision(4));
    EXPECT_DOUBLE_EQ(controller.selectedAudioEnvelope()
                         .value(QStringLiteral("gainDb"))
                         .toDouble(),
                     -6.0);
    controller.resetSelectedAudioEnvelope();
    ASSERT_TRUE(waitRevision(5));
    EXPECT_TRUE(controller.selectedAudioEnvelope().isEmpty());

    controller.seek(1'000'000'000);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.addTitle(QStringLiteral("New title"),
                        QStringLiteral("Noto Sans"), 0.2, 0.1,
                        QStringLiteral("#ffffffff"),
                        QStringLiteral("#00000080"),
                        QStringLiteral("center"));
    ASSERT_TRUE(waitRevision(6));
    const QVariantList titles =
        controller.timelineTrackModel()
            ->data(controller.timelineTrackModel()->index(2, 0),
                   TimelineTrackModel::ClipsRole)
            .toList();
    ASSERT_EQ(titles.size(), 2);
    const QString addedTitleId =
        titles.back().toMap().value(QStringLiteral("id")).toString();
    controller.selectClip(QStringLiteral("title-1"), addedTitleId);
    controller.editSelectedTitle(
        QStringLiteral("Edited title"), QStringLiteral("Noto Sans"), 0.3,
        0.2, QStringLiteral("#ff0000ff"),
        QStringLiteral("#00000000"), QStringLiteral("left"));
    ASSERT_TRUE(waitRevision(7));
    EXPECT_EQ(controller.selectedTitlePayload()
                  .value(QStringLiteral("text"))
                  .toString(),
              QStringLiteral("Edited title"));
    controller.removeSelectedTitle();
    ASSERT_TRUE(waitRevision(8));
    EXPECT_TRUE(controller.selectedClipId().isEmpty());

    controller.selectClip(QStringLiteral("caption-1"),
                          QStringLiteral("caption"));
    controller.addCaptionCue(200, 100, QStringLiteral("Second caption"));
    ASSERT_TRUE(waitRevision(9));
    ASSERT_EQ(controller.selectedCaptionCues().size(), 2);
    const QString addedCueId = controller.selectedCaptionCues()
                                   .back()
                                   .toMap()
                                   .value(QStringLiteral("cueId"))
                                   .toString();
    controller.editCaptionCue(addedCueId, 220, 100,
                              QStringLiteral("Edited caption"));
    ASSERT_TRUE(waitRevision(10));
    EXPECT_EQ(controller.selectedCaptionCues()
                  .back()
                  .toMap()
                  .value(QStringLiteral("text"))
                  .toString(),
              QStringLiteral("Edited caption"));
    controller.removeCaptionCue(addedCueId);
    ASSERT_TRUE(waitRevision(11));
    ASSERT_EQ(controller.selectedCaptionCues().size(), 1);
}

TEST(EditorControllerDurableTest,
     RejectsInvalidInspectorFieldsBeforeIssuingDurableWork) {
    DurableControllerPackage package{true};
    EditorController controller{std::make_unique<FakeEditEngine>()};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy();
    }));
    controller.selectClip(QStringLiteral("v1"), QStringLiteral("video"));
    controller.applySelectedVisualTransform(
        0.0, 0.0, 1.0, 1.0, 1.0, 1.0,
        std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0, 0.0, 1.0,
        0);
    EXPECT_FALSE(controller.sessionBusy());
    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("visual transform"), Qt::CaseInsensitive));

    controller.addTitle(QStringLiteral("Bad color"),
                        QStringLiteral("Noto Sans"), 0.2, 0.1,
                        QStringLiteral("red"), QStringLiteral("#00000000"),
                        QStringLiteral("center"));
    EXPECT_FALSE(controller.sessionBusy());
    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("foreground"), Qt::CaseInsensitive));

    controller.selectClip(QStringLiteral("caption-1"),
                          QStringLiteral("caption"));
    controller.addCaptionCue(50, 100, QStringLiteral("Overlaps"));
    EXPECT_FALSE(controller.sessionBusy());
    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("overlap"), Qt::CaseInsensitive));
}

TEST(EditorControllerDurableTest,
     PreservesCommittedTitleAndMarksPreviewStaleAfterCacheFailure) {
    DurableControllerPackage package;
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 0;
    }));
    const auto callsBefore = fake->calls().size();
    package.blockGeneratedCacheWrites();

    controller.addTitle(QStringLiteral("Committed despite cache failure"),
                        QStringLiteral("Noto Sans"), 0.2, 0.1,
                        QStringLiteral("#ffffffff"),
                        QStringLiteral("#00000080"),
                        QStringLiteral("center"));

    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 1;
    }));
    EXPECT_TRUE(controller.canUndo());
    EXPECT_FALSE(controller.clean());
    EXPECT_TRUE(controller.previewStale());
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("generated overlay"), Qt::CaseInsensitive));
    EXPECT_EQ(controller.timelineTrackModel()->rowCount(), 2);
    ASSERT_GT(fake->calls().size(), callsBefore);
    EXPECT_EQ(fake->calls()[callsBefore].operation, FakeEditOperation::Load);
}

TEST(EditorControllerDurableTest,
     RejectsGeneratedEditsOnStableLockedTracksBeforeDurableWork) {
    DurableControllerPackage package{true, true};
    EditorController controller{std::make_unique<FakeEditEngine>()};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy();
    }));
    controller.selectClip(QStringLiteral("v1"), QStringLiteral("video"));

    controller.addTitle(QStringLiteral("Locked title"),
                        QStringLiteral("Noto Sans"), 0.2, 0.1,
                        QStringLiteral("#ffffffff"),
                        QStringLiteral("#00000080"),
                        QStringLiteral("center"));
    EXPECT_FALSE(controller.sessionBusy());
    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("locked"), Qt::CaseInsensitive));

    controller.addCaptionCue(200, 100, QStringLiteral("Locked caption"));
    EXPECT_FALSE(controller.sessionBusy());
    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("locked"), Qt::CaseInsensitive));
}

TEST(EditorControllerDurableTest,
     ReusesCaptionAtPlayheadDespiteUnrelatedLockedSelection) {
    DurableControllerPackage package{true, false, true};
    EditorController controller{std::make_unique<FakeEditEngine>()};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy();
    }));
    controller.selectClip(QStringLiteral("v1"), QStringLiteral("video"));

    controller.addCaptionCue(200, 100, QStringLiteral("Reused caption"));

    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 1;
    }));
    const QVariantList captionClips =
        controller.timelineTrackModel()
            ->data(controller.timelineTrackModel()->index(3, 0),
                   TimelineTrackModel::ClipsRole)
            .toList();
    ASSERT_EQ(captionClips.size(), 1);
    EXPECT_EQ(captionClips.front()
                  .toMap()
                  .value(QStringLiteral("captionCues"))
                  .toList()
                  .size(),
              2);
}

TEST(EditorControllerDurableTest,
     StartingProjectOpenClearsTransientSelectionRangeAndPreview) {
    DurableControllerPackage package;
    auto engine = std::make_unique<FakeEditEngine>();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshotWithVideo(1, DurationNs{1'000}));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.hasPreviewFrame();
    }));
    controller.selectClip(QStringLiteral("v1"),
                          QStringLiteral("timed-clip"));
    controller.seek(200);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeIn();
    controller.seek(800);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeOut();
    ASSERT_TRUE(controller.hasMarkedRange());

    controller.openProject(package.url());

    EXPECT_TRUE(controller.selectedTrackId().isEmpty());
    EXPECT_TRUE(controller.selectedClipId().isEmpty());
    EXPECT_FALSE(controller.hasMarkedRange());
    EXPECT_FALSE(controller.hasPreviewFrame());
    EXPECT_TRUE(controller.previewStale());
}

TEST(EditorControllerDurableTest,
     StartingNewRangePastExistingOutDropsTheStaleOutMarker) {
    auto engine = std::make_unique<FakeEditEngine>();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshotWithVideo(1, DurationNs{2'000}));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));

    controller.seek(200);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeIn();
    controller.seek(800);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeOut();
    ASSERT_TRUE(controller.hasMarkedRange());

    controller.seek(1'200);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeIn();

    EXPECT_EQ(controller.rangeInNs(), 1'200);
    EXPECT_EQ(controller.rangeOutNs(), -1);
    EXPECT_FALSE(controller.hasMarkedRange());

    controller.seek(1'600);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.markRangeOut();
    EXPECT_EQ(controller.rangeInNs(), 1'200);
    EXPECT_EQ(controller.rangeOutNs(), 1'600);
    EXPECT_TRUE(controller.hasMarkedRange());
}

TEST(EditorControllerDurableTest,
     SerializesDurableSplitAndPausesPlaybackBeforePreviewUpdate) {
    DurableControllerPackage package;
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};

    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 0;
    }));
    controller.selectClip(QStringLiteral("v1"),
                          QStringLiteral("timed-clip"));
    controller.seek(400);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    controller.play();
    ASSERT_TRUE(waitUntil([&] { return controller.playing(); }));
    const auto callsBeforeEdit = fake->calls().size();

    controller.splitSelected();
    EXPECT_TRUE(controller.sessionBusy());
    EXPECT_FALSE(controller.playing());
    controller.splitSelected();

    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 1;
    }));
    EXPECT_TRUE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    EXPECT_FALSE(controller.clean());
    const auto clips = controller.timelineTrackModel()
                           ->data(controller.timelineTrackModel()->index(0, 0),
                                  TimelineTrackModel::ClipsRole)
                           .toList();
    EXPECT_EQ(clips.size(), 2);

    ASSERT_GT(fake->calls().size(), callsBeforeEdit + 1U);
    EXPECT_EQ(fake->calls()[callsBeforeEdit].operation,
              FakeEditOperation::Pause);
    EXPECT_EQ(fake->calls()[callsBeforeEdit + 1].operation,
              FakeEditOperation::Update);

    controller.undo();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 2;
    }));
    EXPECT_FALSE(controller.canUndo());
    EXPECT_TRUE(controller.canRedo());
}

TEST(EditorControllerDurableTest,
     FailedCommitChangesNeitherControllerModelsNorPreviewGraph) {
    DurableControllerPackage package;
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 0;
    }));
    controller.selectClip(QStringLiteral("v1"),
                          QStringLiteral("timed-clip"));
    controller.seek(400);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    const auto callsBefore = fake->calls().size();
    const auto clipsBefore = controller.timelineTrackModel()
                                 ->data(
                                     controller.timelineTrackModel()->index(0, 0),
                                     TimelineTrackModel::ClipsRole)
                                 .toList();
    package.rejectEditCommits();

    controller.splitSelected();
    ASSERT_TRUE(waitUntil([&] { return !controller.sessionBusy(); }));

    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::ClipsRole)
                  .toList(),
              clipsBefore);
    EXPECT_EQ(controller.selectedClipId(), QStringLiteral("timed-clip"));
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    EXPECT_TRUE(controller.clean());
    EXPECT_EQ(fake->calls().size(), callsBefore);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("injected controller commit failure")));
}

TEST(EditorControllerDurableTest,
     MapsTrimRangeUndoRedoSaveAndClearsDeletedSelection) {
    DurableControllerPackage package;
    auto engine = std::make_unique<FakeEditEngine>();
    EditorController controller{std::move(engine)};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 0;
    }));
    controller.selectClip(QStringLiteral("v1"),
                          QStringLiteral("timed-clip"));
    QSignalSpy selectionSpy{&controller, &EditorController::selectionChanged};

    const auto seekAndWait = [&](qlonglong position) {
        controller.seek(position);
        EXPECT_TRUE(waitUntil([&] { return !controller.busy(); }));
    };
    const auto waitRevision = [&](qlonglong revision) {
        return waitUntil([&] {
            return !controller.sessionBusy() && !controller.busy() &&
                   controller.timelineRevision() == revision;
        });
    };

    seekAndWait(100);
    controller.trimSelectedStart();
    ASSERT_TRUE(waitRevision(1));
    EXPECT_EQ(controller.selectedClipStartNs(), 100);
    EXPECT_EQ(selectionSpy.count(), 1);
    seekAndWait(900);
    controller.trimSelectedEnd();
    ASSERT_TRUE(waitRevision(2));
    EXPECT_EQ(controller.selectedClipEndNs(), 900);
    EXPECT_EQ(selectionSpy.count(), 2);

    seekAndWait(300);
    controller.markRangeIn();
    seekAndWait(500);
    controller.markRangeOut();
    controller.deleteMarkedRange(false);
    ASSERT_TRUE(waitRevision(3));
    EXPECT_EQ(controller.selectedClipId(), QStringLiteral("timed-clip"));

    seekAndWait(100);
    controller.markRangeIn();
    seekAndWait(300);
    controller.markRangeOut();
    controller.deleteMarkedRange(true);
    ASSERT_TRUE(waitRevision(4));
    EXPECT_TRUE(controller.selectedTrackId().isEmpty());
    EXPECT_TRUE(controller.selectedClipId().isEmpty());

    controller.undo();
    ASSERT_TRUE(waitRevision(5));
    EXPECT_TRUE(controller.canUndo());
    EXPECT_TRUE(controller.canRedo());
    controller.redo();
    ASSERT_TRUE(waitRevision(6));
    EXPECT_TRUE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    controller.save();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && controller.clean();
    }));
    EXPECT_EQ(controller.timelineRevision(), 6);

    seekAndWait(300);
    controller.markRangeIn();
    controller.markRangeOut();
    ASSERT_FALSE(controller.hasMarkedRange());
    controller.deleteMarkedRange(false);
    EXPECT_FALSE(controller.sessionBusy());
    EXPECT_EQ(controller.timelineRevision(), 6);
    EXPECT_TRUE(controller.statusMessage().contains(
        QStringLiteral("non-empty range")));
}

TEST(EditorControllerDurableTest, BoundarySplitLeavesDurableAndPreviewStateUntouched) {
    DurableControllerPackage package;
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openProject(package.url());
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == 0;
    }));
    controller.selectClip(QStringLiteral("v1"),
                          QStringLiteral("timed-clip"));
    const auto callsBefore = fake->calls().size();

    controller.splitSelected();
    ASSERT_TRUE(waitUntil([&] { return !controller.sessionBusy(); }));

    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::ClipsRole)
                  .toList()
                  .size(),
              1);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_TRUE(controller.clean());
    EXPECT_EQ(fake->calls().size(), callsBefore);
    EXPECT_FALSE(controller.statusMessage().isEmpty());
}

TEST(ProjectEditorIntegrationTest,
     ValidatedProjectOpenPublishesDefaultTimelineWithoutBlockingUi) {
    DurableControllerPackage package;
    ProjectController projects{std::make_unique<ProjectPackageStore>(),
                               package.root() / "recent-projects.json", false};
    auto engine = std::make_unique<FakeEditEngine>();
    EditorController editor{std::move(engine)};
    const auto binding = bindProjectEditor(projects, editor);
    ASSERT_TRUE(binding);

    projects.openProject(package.url());

    ASSERT_TRUE(waitUntil([&] {
        return projects.hasOpenProject() && !projects.busy() &&
               !editor.sessionBusy() && !editor.busy() &&
               editor.timelineRevision() == 0;
    }));
    EXPECT_EQ(projects.projectUrl(), package.url());
    EXPECT_EQ(editor.timelineTrackModel()->rowCount(), 1);
}

TEST(EditorControllerTest, PublishesModelsBeforeAsynchronousEngineLoadCompletes) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    QSignalSpy mediaReset{controller.mediaBinModel(),
                          &QAbstractItemModel::modelReset};
    QSignalSpy tracksReset{controller.timelineTrackModel(),
                           &QAbstractItemModel::modelReset};

    controller.openSession({asset()}, snapshot(1, "강의", "화면"));

    EXPECT_EQ(controller.mediaBinModel()->rowCount(), 1);
    EXPECT_EQ(controller.timelineTrackModel()->rowCount(), 1);
    EXPECT_EQ(mediaReset.count(), 1);
    EXPECT_EQ(tracksReset.count(), 1);
    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::NameRole)
                  .toString(),
              QString::fromUtf8("화면"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    ASSERT_EQ(fake->calls().size(), 2U);
    EXPECT_EQ(fake->calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::RequestFrame);
    EXPECT_FALSE(controller.previewStale());
    EXPECT_EQ(controller.thread(), QThread::currentThread());
}

TEST(EditorControllerTest, SerializesPlaybackAndIgnoresStaleSessionCallback) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    fake->failNext(FakeEditOperation::Load,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "old session failure"});
    EditorController controller{std::move(engine)};
    controller.openSession({asset("old")}, snapshot(1, "Old"));
    controller.openSession({asset("new")}, snapshot(2, "New"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 3U;
    }));

    EXPECT_TRUE(controller.statusMessage().isEmpty());
    EXPECT_FALSE(controller.previewStale());
    ASSERT_EQ(fake->calls().size(), 3U);
    EXPECT_EQ(fake->calls()[0].revision, 1);
    EXPECT_EQ(fake->calls()[1].revision, 2);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::RequestFrame);

    controller.play();
    controller.seek(250);
    controller.pause();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 7U;
    }));
    ASSERT_EQ(fake->calls().size(), 7U);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::Play);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::Seek);
    EXPECT_EQ(fake->calls()[5].operation, FakeEditOperation::Pause);
    EXPECT_EQ(fake->calls()[6].operation, FakeEditOperation::RequestFrame);
    EXPECT_FALSE(controller.playing());
    EXPECT_EQ(controller.playheadNs(), 250);
}

TEST(EditorControllerTest, PublishesCommittedTimelineBeforeEngineUpdate) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(3, "Before", "Original"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(3).value(),
                      snapshot(4, "After", "Edited"),
                      {TrackId::create("v1").value()}, false)
                      .value();

    controller.commitTimeline(change);

    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::NameRole)
                  .toString(),
              QStringLiteral("Edited"));
    EXPECT_EQ(controller.timelineRevision(), 4);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 4U;
    }));
    ASSERT_EQ(fake->calls().size(), 4U);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::RequestFrame);
}

TEST(EditorControllerTest, FailedUpdateKeepsDurableStateAndReloadsPreview) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(8, "Before", "Original"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    fake->failNext(FakeEditOperation::Update,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "preview graph update failed"});
    QSignalSpy staleSpy{&controller, &EditorController::previewStaleChanged};
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(8).value(),
                      snapshot(9, "Durable", "Committed"),
                      {TrackId::create("v1").value()}, false)
                      .value();

    controller.commitTimeline(change);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() >= 5U;
    }));

    EXPECT_EQ(controller.timelineRevision(), 9);
    EXPECT_EQ(controller.timelineTrackModel()
                  ->data(controller.timelineTrackModel()->index(0, 0),
                         TimelineTrackModel::NameRole)
                  .toString(),
              QStringLiteral("Committed"));
    ASSERT_EQ(fake->calls().size(), 5U);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[3].revision, 9);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::RequestFrame);
    EXPECT_GE(staleSpy.count(), 2);
    EXPECT_FALSE(controller.previewStale());
    EXPECT_EQ(controller.statusMessage(),
              QStringLiteral("preview graph update failed"));
}

TEST(EditorControllerTest, RecoveryReloadRunsBeforePlaybackQueuedAfterFailedUpdate) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(10, "Before"));
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    fake->failNext(FakeEditOperation::Update,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "force ordered recovery"});
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(10).value(),
                      snapshot(11, "After"),
                      {TrackId::create("v1").value()}, false)
                      .value();

    controller.commitTimeline(change);
    controller.play();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.playing() &&
               fake->calls().size() >= 5U;
    }));

    ASSERT_GE(fake->calls().size(), 5U);
    EXPECT_EQ(fake->calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::RequestFrame);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::Play);
    EXPECT_TRUE(controller.playing());
    controller.pause();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing();
    }));
}

TEST(EditorControllerAcceptanceTest,
     OpensMultitrackTakeSeeksCommitsSplitAndRecoversPreviewGraph) {
    const MediaAsset screen = asset("screen");
    const MediaAsset camera = asset("camera");
    const MediaAsset microphone = audioAsset("microphone");
    auto timeline = Timeline::create(TimelineId::create("take-1").value(),
                                     "Recorded take",
                                     FrameRate::create(60, 1).value())
                        .value();
    const TrackId screenTrack = TrackId::create("screen-track").value();
    const TrackId cameraTrack = TrackId::create("camera-track").value();
    const TrackId microphoneTrack = TrackId::create("microphone-track").value();
    ASSERT_TRUE(timeline.addTrack(Track::create(screenTrack, TrackKind::Video,
                                                "Screen", true, false)
                                      .value())
                    .hasValue());
    ASSERT_TRUE(timeline.addTrack(Track::create(cameraTrack, TrackKind::Video,
                                                "Camera", true, false)
                                      .value())
                    .hasValue());
    ASSERT_TRUE(timeline.addTrack(Track::create(microphoneTrack, TrackKind::Audio,
                                                "Microphone", true, false)
                                      .value())
                    .hasValue());
    const auto range = TimeRange::create(TimestampNs{}, DurationNs{4'000'000'000})
                           .value();
    ASSERT_TRUE(timeline.insertClip(
                            screenTrack,
                            Clip::createAsset(ClipId::create("screen-clip").value(),
                                              screen, range, range, true,
                                              std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            cameraTrack,
                            Clip::createAsset(ClipId::create("camera-clip").value(),
                                              camera, range, range, true,
                                              std::nullopt, std::nullopt)
                                .value())
                    .hasValue());
    ASSERT_TRUE(timeline.insertClip(
                            microphoneTrack,
                            Clip::createAsset(
                                ClipId::create("microphone-clip").value(), microphone,
                                range, range, true, std::nullopt, std::nullopt)
                                .value())
                    .hasValue());

    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession(
        {screen, camera, microphone},
        TimelineSnapshot{timeline, TimelineRevision::create(1).value()});
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 2U;
    }));
    EXPECT_EQ(controller.mediaBinModel()->rowCount(), 3);
    EXPECT_EQ(controller.timelineTrackModel()->rowCount(), 3);

    controller.seek(2'000'000'000);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 4U;
    }));
    EXPECT_EQ(controller.playheadNs(), 2'000'000'000);

    SplitClipCommand split{
        CommandId::create("split-screen").value(), screenTrack,
        ClipId::create("screen-clip").value(),
        ClipId::create("screen-clip-right").value(),
        TimestampNs{DurationNs{2'000'000'000}}};
    ASSERT_TRUE(split.execute(timeline).hasValue());
    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(1).value(),
                      TimelineSnapshot{timeline,
                                       TimelineRevision::create(2).value()},
                      {screenTrack}, false)
                      .value();
    fake->failNext(FakeEditOperation::Update,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "rebuild acceptance failure"});

    controller.commitTimeline(change);

    const QVariantList committedClips =
        controller.timelineTrackModel()
            ->data(controller.timelineTrackModel()->index(0, 0),
                   TimelineTrackModel::ClipsRole)
            .toList();
    EXPECT_EQ(committedClips.size(), 2);
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() >= 7U;
    }));
    EXPECT_EQ(controller.timelineRevision(), 2);
    EXPECT_FALSE(controller.previewStale());
    ASSERT_EQ(fake->calls().size(), 7U);
    EXPECT_EQ(fake->calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[1].operation, FakeEditOperation::RequestFrame);
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Seek);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::RequestFrame);
    EXPECT_EQ(fake->calls()[4].operation, FakeEditOperation::Update);
    EXPECT_EQ(fake->calls()[5].operation, FakeEditOperation::Load);
    EXPECT_EQ(fake->calls()[6].operation, FakeEditOperation::RequestFrame);
}

struct DrainState final {
    std::atomic<int> calls{0};
    std::thread::id uiThread;
    std::atomic<bool> loadRanOffUiThread{false};
};

struct PreviewThreadState final {
    std::thread::id uiThread;
    std::atomic<bool> frameRanOffUiThread{false};
    std::atomic<std::int64_t> requestedPosition{-1};
    std::atomic<int> frameCalls{0};
    std::atomic<int> activeFrameCalls{0};
    std::atomic<int> maximumActiveFrameCalls{0};
    std::atomic<int> blockCall{0};
    std::atomic<bool> blockedCallEntered{false};
    std::atomic<std::int64_t> returnedPositionOffset{0};
    std::mutex blockMutex;
    std::condition_variable blockCondition;
    bool releaseBlockedCall{false};
};

class PreviewThreadEngine final : public IEditEngine {
public:
    explicit PreviewThreadEngine(std::shared_ptr<PreviewThreadState> state)
        : state_(std::move(state)) {}

    creator::core::Result<void> load(const TimelineSnapshot& value) override {
        revision_ = value.revision;
        return creator::core::ok();
    }
    creator::core::Result<void> update(const TimelineChangeSet& change) override {
        revision_ = change.target().revision;
        return creator::core::ok();
    }
    creator::core::Result<void> play() override { return creator::core::ok(); }
    creator::core::Result<void> pause() override { return creator::core::ok(); }
    creator::core::Result<void> seek(TimestampNs) override {
        return creator::core::ok();
    }
    creator::core::Result<PreviewFrame> requestFrame(
        TimestampNs position) override {
        const int call = ++state_->frameCalls;
        const int active = ++state_->activeFrameCalls;
        int previousMaximum = state_->maximumActiveFrameCalls.load();
        while (active > previousMaximum &&
               !state_->maximumActiveFrameCalls.compare_exchange_weak(
                   previousMaximum, active)) {
        }
        state_->frameRanOffUiThread =
            std::this_thread::get_id() != state_->uiThread;
        state_->requestedPosition = position.time_since_epoch().count();
        if (call == state_->blockCall.load()) {
            std::unique_lock lock{state_->blockMutex};
            state_->blockedCallEntered = true;
            state_->blockCondition.notify_all();
            state_->blockCondition.wait(
                lock, [&] { return state_->releaseBlockedCall; });
        }
        auto pixels = std::make_shared<std::vector<std::uint8_t>>(
            std::initializer_list<std::uint8_t>{0x10, 0x20, 0x30, 0xff,
                                                0x40, 0x50, 0x60, 0xff});
        (*pixels)[2] = static_cast<std::uint8_t>(revision_.value());
        --state_->activeFrameCalls;
        const auto returnedPosition =
            position + DurationNs{state_->returnedPositionOffset.load()};
        creator::media::VideoFrame frame{
            .timestamp = returnedPosition,
            .width = 2,
            .height = 1,
            .visibleRect = {.x = 0, .y = 0, .width = 2, .height = 1},
            .contentWidth = 2,
            .contentHeight = 1,
            .contentScale = 1.0,
            .pointPixelScale = 1.0,
            .pixelFormat = creator::media::PixelFormat::Bgra8,
            .colorSpace = creator::media::ColorSpace::Rec709Sdr,
            .platformHandle =
                std::shared_ptr<void>{pixels, pixels->data()}};
        return PreviewFrame::create(returnedPosition, revision_,
                                    std::move(frame));
    }
    creator::core::Result<std::unique_ptr<IRenderJob>> render(
        const RenderRequest&) override {
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "not used"};
    }

private:
    std::shared_ptr<PreviewThreadState> state_;
    TimelineRevision revision_{TimelineRevision::create(0).value()};
};

class DrainCountingEngine final : public IEditEngine {
public:
    explicit DrainCountingEngine(std::shared_ptr<DrainState> state)
        : state_(std::move(state)) {}

    creator::core::Result<void> load(const TimelineSnapshot&) override {
        ++state_->calls;
        state_->loadRanOffUiThread =
            std::this_thread::get_id() != state_->uiThread;
        return creator::core::ok();
    }
    creator::core::Result<void> update(const TimelineChangeSet&) override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<void> play() override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<void> pause() override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<void> seek(TimestampNs) override {
        ++state_->calls;
        return creator::core::ok();
    }
    creator::core::Result<PreviewFrame> requestFrame(TimestampNs) override {
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "not used"};
    }
    creator::core::Result<std::unique_ptr<IRenderJob>> render(
        const RenderRequest&) override {
        return creator::core::AppError{creator::core::ErrorCode::InvalidState,
                                       "not used"};
    }

private:
    std::shared_ptr<DrainState> state_;
};

TEST(EditorControllerTest, RunsEngineLoadOffTheUiThread) {
    auto state = std::make_shared<DrainState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{std::make_unique<DrainCountingEngine>(state)};
    controller.openSession({asset()}, snapshot(1, "Threaded"));
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
    EXPECT_TRUE(state->loadRanOffUiThread.load());
}

TEST(EditorControllerTest, RequestsDetachedPreviewFrameOffTheUiThread) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};

    controller.openSession({asset()}, snapshot(7, "Preview worker"));

    ASSERT_TRUE(waitUntil([&] { return !controller.previewImage().isNull(); }));
    EXPECT_TRUE(state->frameRanOffUiThread.load());
    EXPECT_EQ(state->requestedPosition.load(), 0);
    ASSERT_EQ(controller.previewImage().size(), QSize(2, 1));
    EXPECT_EQ(controller.previewImage().pixelColor(0, 0),
              QColor(0x07, 0x20, 0x10));
}

TEST(EditorControllerTest, SeekRequestsTheExactPreviewFrame) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(12, "Exact seek"));
    ASSERT_TRUE(waitUntil([&] { return fake->calls().size() == 2U; }));

    constexpr qlonglong kPosition = 1'234'567'890;
    controller.seek(kPosition);

    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && fake->calls().size() == 4U;
    }));
    EXPECT_EQ(fake->calls()[2].operation, FakeEditOperation::Seek);
    EXPECT_EQ(fake->calls()[3].operation, FakeEditOperation::RequestFrame);
    ASSERT_TRUE(fake->calls()[3].position.has_value());
    EXPECT_EQ(fake->calls()[3].position->time_since_epoch().count(), kPosition);
    EXPECT_EQ(controller.playheadNs(), kPosition);
}

TEST(EditorControllerTest, PlaybackAdvancesAtTimelineRateAndPauseStopsIt) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    auto timed = snapshotWithVideo(13, DurationNs{5'000'000'000});
    controller.openSession(timed.assets, std::move(timed));
    ASSERT_TRUE(waitUntil([&] { return fake->calls().size() == 2U; }));

    controller.play();
    ASSERT_TRUE(waitUntil([&] {
        return controller.playing() && controller.playheadNs() > 0 &&
               fake->calls().size() >= 4U;
    }, 1000));
    const qlonglong advancedPosition = controller.playheadNs();
    const auto rate = FrameRate::create(60, 1).value();
    const auto frameIndex = creator::core::timestampToFrame(
        TimestampNs{DurationNs{advancedPosition}}, rate);
    EXPECT_EQ(creator::core::frameToTimestamp(frameIndex, rate)
                  .time_since_epoch()
                  .count(),
              advancedPosition);

    controller.pause();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing();
    }));
    const qlonglong pausedPosition = controller.playheadNs();
    QElapsedTimer pauseObservation;
    pauseObservation.start();
    while (pauseObservation.elapsed() < 80) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    EXPECT_GE(pausedPosition, advancedPosition);
    EXPECT_EQ(controller.playheadNs(), pausedPosition);
}

TEST(EditorControllerTest, PlaybackStopsAtLastFrameWithoutMarkingPreviewStale) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    auto timed = snapshotWithVideo(16, DurationNs{100'000'000});
    controller.openSession(timed.assets, std::move(timed));
    ASSERT_TRUE(waitUntil([&] { return fake->calls().size() == 2U; }));

    controller.play();

    const auto rate = FrameRate::create(60, 1).value();
    const auto lastPosition = creator::core::frameToTimestamp(5, rate)
                                  .time_since_epoch()
                                  .count();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing() &&
               controller.playheadNs() == lastPosition;
    }, 2000));
    EXPECT_FALSE(controller.previewStale());
    EXPECT_TRUE(controller.statusMessage().isEmpty());
    ASSERT_GE(fake->calls().size(), 6U);
    EXPECT_EQ(fake->calls().back().operation, FakeEditOperation::Pause);
}

TEST(EditorControllerTest, RejectsFrameReturnedForDifferentPosition) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    state->returnedPositionOffset = 1;
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};

    controller.openSession({asset()}, snapshot(17, "Wrong position"));

    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.previewStale();
    }));
    EXPECT_FALSE(controller.hasPreviewFrame());
    EXPECT_EQ(controller.statusMessage(),
              QStringLiteral("Edit engine returned the wrong preview position"));
}

TEST(EditorControllerTest, PlaybackKeepsOnlyOneFrameRequestInFlight) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};
    controller.openSession(
        {asset()}, snapshotWithVideo(14, DurationNs{5'000'000'000}));
    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 1 && controller.hasPreviewFrame();
    }));
    state->blockCall = 2;

    controller.play();
    const bool blocked = waitUntil(
        [&] { return state->blockedCallEntered.load(); }, 1000);
    EXPECT_TRUE(blocked);
    QElapsedTimer observation;
    observation.start();
    while (observation.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    EXPECT_EQ(state->frameCalls.load(), 2);
    EXPECT_EQ(state->maximumActiveFrameCalls.load(), 1);

    controller.pause();
    {
        std::lock_guard lock{state->blockMutex};
        state->releaseBlockedCall = true;
    }
    state->blockCondition.notify_all();
    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && !controller.playing();
    }));
}

TEST(EditorControllerTest, DropsLateFrameFromReplacedSession) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    state->blockCall = 1;
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};
    QSignalSpy frameSpy{&controller, &EditorController::previewImageChanged};
    controller.openSession({asset("old")}, snapshot(1, "Old preview"));
    const bool blocked = waitUntil(
        [&] { return state->blockedCallEntered.load(); }, 1000);
    EXPECT_TRUE(blocked);

    controller.openSession({asset("new")}, snapshot(2, "New preview"));
    {
        std::lock_guard lock{state->blockMutex};
        state->releaseBlockedCall = true;
    }
    state->blockCondition.notify_all();

    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 2 && controller.hasPreviewFrame();
    }));
    EXPECT_EQ(frameSpy.count(), 1);
    EXPECT_EQ(controller.previewImage().pixelColor(0, 0),
              QColor(0x02, 0x20, 0x10));
    EXPECT_EQ(controller.timelineRevision(), 2);
}

TEST(EditorControllerTest, DropsLateFrameFromReplacedTimelineRevision) {
    auto state = std::make_shared<PreviewThreadState>();
    state->uiThread = std::this_thread::get_id();
    EditorController controller{
        std::make_unique<PreviewThreadEngine>(state)};
    controller.openSession({asset()}, snapshot(20, "Before revision"));
    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 1 && controller.hasPreviewFrame();
    }));
    QSignalSpy frameSpy{&controller, &EditorController::previewImageChanged};
    state->blockCall = 2;
    controller.seek(100);
    const bool blocked = waitUntil(
        [&] { return state->blockedCallEntered.load(); }, 1000);
    EXPECT_TRUE(blocked);

    auto change = TimelineChangeSet::create(
                      TimelineRevision::create(20).value(),
                      snapshot(21, "After revision"),
                      {TrackId::create("v1").value()}, false)
                      .value();
    controller.commitTimeline(std::move(change));
    {
        std::lock_guard lock{state->blockMutex};
        state->releaseBlockedCall = true;
    }
    state->blockCondition.notify_all();

    ASSERT_TRUE(waitUntil([&] {
        return state->frameCalls.load() == 3 && !controller.previewStale();
    }));
    EXPECT_EQ(frameSpy.count(), 1);
    EXPECT_EQ(controller.previewImage().pixelColor(0, 0),
              QColor(0x15, 0x20, 0x10));
    EXPECT_TRUE(controller.statusMessage().isEmpty());
    EXPECT_EQ(controller.timelineRevision(), 21);
}

TEST(EditorControllerTest, FrameFailureMarksOnlyPreviewStale) {
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openSession({asset()}, snapshot(15, "Frame failure"));
    ASSERT_TRUE(waitUntil([&] { return fake->calls().size() == 2U; }));
    fake->failNext(FakeEditOperation::RequestFrame,
                   creator::core::AppError{creator::core::ErrorCode::IoFailure,
                                           "preview decode failed"});

    controller.seek(500'000'000);

    ASSERT_TRUE(waitUntil([&] {
        return !controller.busy() && controller.previewStale();
    }));
    EXPECT_EQ(controller.timelineRevision(), 15);
    EXPECT_EQ(controller.playheadNs(), 500'000'000);
    EXPECT_EQ(controller.statusMessage(), QStringLiteral("preview decode failed"));
}

TEST(EditorControllerTest, DestructionDrainsEachQueuedEngineCommandExactlyOnce) {
    auto state = std::make_shared<DrainState>();
    state->uiThread = std::this_thread::get_id();
    {
        EditorController controller{
            std::make_unique<DrainCountingEngine>(state)};
        controller.openSession({asset()}, snapshot(1, "Drain"));
        controller.play();
        controller.seek(100);
        controller.pause();
    }
    EXPECT_EQ(state->calls.load(), 4);
}

}  // namespace
