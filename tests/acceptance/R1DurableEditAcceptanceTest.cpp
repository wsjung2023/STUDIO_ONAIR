#include "app/EditorController.h"

#include "core/Timebase.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "fakes/FakeEditEngine.h"
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteTimelineStore.h"
#include "project_store/internal/SqliteConnection.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using creator::app::EditorController;
using creator::app::TimelineTrackModel;
using creator::core::DurationNs;
using creator::core::FrameRate;
using creator::core::TimestampNs;
using creator::domain::AssetAvailability;
using creator::domain::AssetId;
using creator::domain::AudioAssetMetadata;
using creator::domain::Clip;
using creator::domain::ClipId;
using creator::domain::MediaAsset;
using creator::domain::MediaKind;
using creator::domain::TimeRange;
using creator::domain::Timeline;
using creator::domain::TimelineId;
using creator::domain::Track;
using creator::domain::TrackId;
using creator::domain::TrackKind;
using creator::domain::VideoAssetMetadata;
using creator::fakes::FakeEditEngine;
using creator::fakes::FakeEditOperation;
using creator::project_store::ProjectPackageStore;
using creator::project_store::SqliteTimelineStore;

constexpr std::int64_t kSecond = 1'000'000'000;

TimestampNs at(std::int64_t value) {
    return TimestampNs{DurationNs{value}};
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

MediaAsset videoAsset(std::string id, std::string path) {
    return MediaAsset::create(
               AssetId::create(std::move(id)).value(), MediaKind::Video,
               std::move(path), DurationNs{10 * kSecond},
               VideoAssetMetadata{.width = 1920,
                                  .height = 1080,
                                  .frameRate = FrameRate::create(60, 1).value()},
               std::nullopt, 100, "video-fingerprint",
               AssetAvailability::Available)
        .value();
}

MediaAsset audioAsset() {
    return MediaAsset::create(
               AssetId::create("microphone").value(), MediaKind::Audio,
               "audio/microphone.wav", DurationNs{10 * kSecond}, std::nullopt,
               AudioAssetMetadata{.sampleRate = 48'000, .channels = 2}, 100,
               "audio-fingerprint", AssetAvailability::Available)
        .value();
}

class DurablePackage final {
public:
    DurablePackage() {
        root_ = fs::temp_directory_path() /
                (fs::path{L"creator-studio-R1-재열기-"} /
                 std::to_wstring(++nextId_));
        package_ = root_ / fs::path{L"승인-편집.cstudio"};
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_);

        ProjectPackageStore packageStore;
        auto created = packageStore.create(package_, "R1 durable acceptance");
        EXPECT_TRUE(created.hasValue())
            << (created.hasValue() ? "" : created.error().message());
        if (!created.hasValue()) return;
        projectId_ = created.value().package.manifest.projectId;
        database_ = package_ / created.value().package.manifest.database;

        const auto screen = videoAsset("screen", "media/screen.mp4");
        const auto camera = videoAsset("camera", "media/camera.mp4");
        const auto microphone = audioAsset();
        auto timeline = Timeline::create(TimelineId::create("main").value(),
                                         "Main",
                                         FrameRate::create(60, 1).value())
                            .value();
        const auto videoTrack = TrackId::create("video-1").value();
        const auto audioTrack = TrackId::create("audio-1").value();
        EXPECT_TRUE(timeline.addTrack(
                                Track::create(videoTrack, TrackKind::Video,
                                              "Video 1", true, false)
                                    .value())
                        .hasValue());
        EXPECT_TRUE(timeline.addTrack(
                                Track::create(audioTrack, TrackKind::Audio,
                                              "Audio 1", true, false)
                                    .value())
                        .hasValue());
        const auto screenRange =
            TimeRange::create(at(0), DurationNs{4 * kSecond}).value();
        const auto cameraSource =
            TimeRange::create(at(0), DurationNs{3 * kSecond}).value();
        const auto cameraPlaced =
            TimeRange::create(at(5 * kSecond), DurationNs{3 * kSecond}).value();
        const auto audioRange =
            TimeRange::create(at(0), DurationNs{8 * kSecond}).value();
        EXPECT_TRUE(timeline.insertClip(
                                videoTrack,
                                Clip::createAsset(
                                    ClipId::create("clip-a").value(), screen,
                                    screenRange, screenRange, true, std::nullopt,
                                    std::nullopt)
                                    .value())
                        .hasValue());
        EXPECT_TRUE(timeline.insertClip(
                                videoTrack,
                                Clip::createAsset(
                                    ClipId::create("clip-b").value(), camera,
                                    cameraSource, cameraPlaced, true, std::nullopt,
                                    std::nullopt)
                                    .value())
                        .hasValue());
        EXPECT_TRUE(timeline.insertClip(
                                audioTrack,
                                Clip::createAsset(
                                    ClipId::create("clip-audio").value(),
                                    microphone, audioRange, audioRange, true,
                                    std::nullopt, std::nullopt)
                                    .value())
                        .hasValue());

        auto storeResult = SqliteTimelineStore::open(database_, *projectId_);
        EXPECT_TRUE(storeResult.hasValue())
            << (storeResult.hasValue() ? "" : storeResult.error().message());
        if (!storeResult.hasValue()) return;
        auto store = std::move(storeResult).value();
        EXPECT_TRUE(store.putAsset(screen).hasValue());
        EXPECT_TRUE(store.putAsset(camera).hasValue());
        EXPECT_TRUE(store.putAsset(microphone).hasValue());
        EXPECT_TRUE(store.createTimeline(timeline).hasValue());
    }

    ~DurablePackage() {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    [[nodiscard]] QUrl url() const {
        return QUrl::fromLocalFile(QString::fromStdWString(package_.wstring()));
    }
    [[nodiscard]] const fs::path& database() const noexcept { return database_; }
    [[nodiscard]] const creator::domain::ProjectId& projectId() const {
        return *projectId_;
    }
    void rejectEditCommits() const {
        auto connection =
            creator::project_store::internal::SqliteConnection::open(database_);
        ASSERT_TRUE(connection.hasValue()) << connection.error().message();
        ASSERT_TRUE(connection.value()
                        .execute(
                            "CREATE TRIGGER reject_acceptance_edit "
                            "BEFORE INSERT ON edit_commands "
                            "BEGIN SELECT RAISE(ABORT, 'acceptance commit "
                            "failure'); END")
                        .hasValue());
    }

private:
    inline static std::uint64_t nextId_{0};
    fs::path root_;
    fs::path package_;
    fs::path database_;
    std::optional<creator::domain::ProjectId> projectId_;
};

QVariantList timelineRows(EditorController& controller) {
    QVariantList rows;
    auto* model = controller.timelineTrackModel();
    for (int row = 0; row < model->rowCount(); ++row) {
        const auto index = model->index(row, 0);
        rows.push_back(QVariantMap{
            {QStringLiteral("trackId"),
             model->data(index, TimelineTrackModel::TrackIdRole)},
            {QStringLiteral("name"),
             model->data(index, TimelineTrackModel::NameRole)},
            {QStringLiteral("kind"),
             model->data(index, TimelineTrackModel::KindRole)},
            {QStringLiteral("clips"),
             model->data(index, TimelineTrackModel::ClipsRole)},
        });
    }
    return rows;
}

void seekAndWait(EditorController& controller, qlonglong position) {
    controller.seek(position);
    ASSERT_TRUE(waitUntil([&] { return !controller.busy(); }));
}

void waitRevision(EditorController& controller, qlonglong revision) {
    ASSERT_TRUE(waitUntil([&] {
        return !controller.sessionBusy() && !controller.busy() &&
               controller.timelineRevision() == revision;
    }));
}

TEST(R1DurableEditAcceptanceTest, ReopensExactCommittedWorkflowAndHistory) {
    DurablePackage package;
    QVariantList savedRows;
    std::vector<std::int64_t> updateRevisions;
    {
        auto engine = std::make_unique<FakeEditEngine>();
        FakeEditEngine* fake = engine.get();
        EditorController controller{std::move(engine)};
        controller.openProject(package.url());
        waitRevision(controller, 0);

        controller.selectClip(QStringLiteral("video-1"),
                              QStringLiteral("clip-a"));
        seekAndWait(controller, 2 * kSecond);
        controller.splitSelected();
        waitRevision(controller, 1);

        const auto clipsAfterSplit =
            timelineRows(controller)[0].toMap().value(QStringLiteral("clips")).toList();
        QString rightClipId;
        for (const auto& value : clipsAfterSplit) {
            const auto clip = value.toMap();
            if (clip.value(QStringLiteral("id")).toString() !=
                    QStringLiteral("clip-a") &&
                clip.value(QStringLiteral("timelineStartNs")).toLongLong() ==
                    2 * kSecond) {
                rightClipId = clip.value(QStringLiteral("id")).toString();
            }
        }
        ASSERT_FALSE(rightClipId.isEmpty());
        controller.selectClip(QStringLiteral("video-1"), rightClipId);
        seekAndWait(controller, 3'500'000'000);
        controller.trimSelectedEnd();
        waitRevision(controller, 2);

        seekAndWait(controller, 1 * kSecond);
        controller.markRangeIn();
        seekAndWait(controller, 1'500'000'000);
        controller.markRangeOut();
        controller.deleteMarkedRange(false);
        waitRevision(controller, 3);
        controller.undo();
        waitRevision(controller, 4);
        controller.redo();
        waitRevision(controller, 5);

        seekAndWait(controller, 4'500'000'000);
        controller.markRangeIn();
        seekAndWait(controller, 5'500'000'000);
        controller.markRangeOut();
        controller.deleteMarkedRange(true);
        waitRevision(controller, 6);
        controller.save();
        ASSERT_TRUE(waitUntil([&] {
            return !controller.sessionBusy() && controller.clean();
        }));

        savedRows = timelineRows(controller);
        EXPECT_TRUE(controller.canUndo());
        EXPECT_FALSE(controller.canRedo());
        for (const auto& call : fake->calls()) {
            if (call.operation == FakeEditOperation::Update &&
                call.revision.has_value()) {
                updateRevisions.push_back(*call.revision);
            }
        }
        EXPECT_EQ(updateRevisions,
                  (std::vector<std::int64_t>{1, 2, 3, 4, 5, 6}));
    }

    auto reopenedEngine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* reopenedFake = reopenedEngine.get();
    EditorController reopened{std::move(reopenedEngine)};
    reopened.openProject(package.url());
    waitRevision(reopened, 6);
    EXPECT_EQ(timelineRows(reopened), savedRows);
    EXPECT_TRUE(reopened.clean());
    EXPECT_TRUE(reopened.canUndo());
    EXPECT_FALSE(reopened.canRedo());
    ASSERT_GE(reopenedFake->calls().size(), 2U);
    EXPECT_EQ(reopenedFake->calls()[0].operation, FakeEditOperation::Load);
    EXPECT_EQ(reopenedFake->calls()[0].revision, 6);
    EXPECT_EQ(reopenedFake->calls()[1].operation,
              FakeEditOperation::RequestFrame);
}

TEST(R1DurableEditAcceptanceTest,
     TransactionFailureChangesNoUiEngineOrDatabaseState) {
    DurablePackage package;
    auto engine = std::make_unique<FakeEditEngine>();
    FakeEditEngine* fake = engine.get();
    EditorController controller{std::move(engine)};
    controller.openProject(package.url());
    waitRevision(controller, 0);
    controller.selectClip(QStringLiteral("video-1"), QStringLiteral("clip-a"));
    seekAndWait(controller, 2 * kSecond);
    const auto rowsBefore = timelineRows(controller);
    const auto callsBefore = fake->calls().size();
    package.rejectEditCommits();

    controller.splitSelected();
    ASSERT_TRUE(waitUntil([&] { return !controller.sessionBusy(); }));

    EXPECT_EQ(controller.timelineRevision(), 0);
    EXPECT_EQ(timelineRows(controller), rowsBefore);
    EXPECT_EQ(controller.selectedClipId(), QStringLiteral("clip-a"));
    EXPECT_EQ(controller.playheadNs(), 2 * kSecond);
    EXPECT_TRUE(controller.clean());
    EXPECT_FALSE(controller.canUndo());
    EXPECT_EQ(fake->calls().size(), callsBefore);

    auto stored = SqliteTimelineStore::open(package.database(), package.projectId());
    ASSERT_TRUE(stored.hasValue()) << stored.error().message();
    auto persisted = stored.value().loadPrimaryTimeline();
    ASSERT_TRUE(persisted.hasValue()) << persisted.error().message();
    EXPECT_EQ(persisted.value().revision, 0);
    EXPECT_EQ(persisted.value().historyCount, 0U);
    EXPECT_EQ(persisted.value().historyCursor, 0U);
    EXPECT_EQ(persisted.value().timeline.tracks()[0].clips().size(), 2U);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
