#include "app/ScreenPreviewItem.h"
#include "app/MediaBinModel.h"
#include "app/TimelineTrackModel.h"

#include "core/Timebase.h"
#include "domain/Identifiers.h"
#include "domain/MediaAsset.h"
#include "domain/Timeline.h"
#include "domain/TimelineRevision.h"
#include "domain/TimelineTypes.h"

#include <QGuiApplication>
#include <QCoreApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <gtest/gtest.h>

#include <memory>

namespace {

QQuickItem* findVisualItem(QQuickItem* root, const QString& objectName) {
    if (root->objectName() == objectName) return root;
    for (QQuickItem* child : root->childItems()) {
        if (auto* found = findVisualItem(child, objectName); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

class FakeProjectController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool hasOpenProject READ hasOpenProject CONSTANT)
    Q_PROPERTY(QString projectName READ projectName CONSTANT)
    Q_PROPERTY(QUrl projectUrl READ projectUrl CONSTANT)
    Q_PROPERTY(QVariantList recentProjects READ recentProjects CONSTANT)
    Q_PROPERTY(QVariantList recoveries READ recoveries CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)

public:
    using QObject::QObject;

    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool hasOpenProject() const noexcept { return false; }
    [[nodiscard]] QString projectName() const { return {}; }
    [[nodiscard]] QUrl projectUrl() const { return {}; }
    [[nodiscard]] QVariantList recentProjects() const { return {}; }
    [[nodiscard]] QVariantList recoveries() const {
        return {QVariantMap{
            {QStringLiteral("sessionId"), QStringLiteral("session-1")},
            {QStringLiteral("projectName"), QString::fromUtf8("강의")},
            {QStringLiteral("projectUrl"), QUrl::fromLocalFile("C:/fixture.cstudio")},
            {QStringLiteral("createdAt"), QStringLiteral("2026-07-16T12:00:00Z")},
            {QStringLiteral("readySegments"), 1},
            {QStringLiteral("writingSegments"), 1},
        }};
    }
    [[nodiscard]] QString statusMessage() const { return {}; }

    Q_INVOKABLE void recoverSession(const QString&) {}
    Q_INVOKABLE void leaveRecoveryForLater() {}
    Q_INVOKABLE void createProject(const QUrl&, const QString&) {}
    Q_INVOKABLE void openProject(const QUrl&) {}

signals:
    void projectOpened();
    void recoveryRequired();
    void recoveryDeferred();
};

class FakeStudioController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool recording READ recording CONSTANT)
    Q_PROPERTY(bool recordingAvailable READ recordingAvailable CONSTANT)
    Q_PROPERTY(int segmentCount READ segmentCount CONSTANT)
    Q_PROPERTY(int trackCount READ trackCount CONSTANT)
    Q_PROPERTY(qulonglong queuedItems READ queuedItems CONSTANT)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames CONSTANT)
    Q_PROPERTY(qulonglong syncDroppedFrames READ syncDroppedFrames CONSTANT)
    Q_PROPERTY(qulonglong duplicatedFrames READ duplicatedFrames CONSTANT)
    Q_PROPERTY(double maximumDriftMilliseconds READ maximumDriftMilliseconds CONSTANT)
    Q_PROPERTY(double audioCorrectionPpm READ audioCorrectionPpm CONSTANT)
    Q_PROPERTY(qulonglong diskAvailableBytes READ diskAvailableBytes CONSTANT)
    Q_PROPERTY(QString encoderName READ encoderName CONSTANT)
    Q_PROPERTY(QString takeDuration READ takeDuration CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool recording() const noexcept { return false; }
    [[nodiscard]] bool recordingAvailable() const noexcept { return true; }
    [[nodiscard]] int segmentCount() const noexcept { return 0; }
    [[nodiscard]] int trackCount() const noexcept { return 2; }
    [[nodiscard]] qulonglong queuedItems() const noexcept { return 3; }
    [[nodiscard]] qulonglong droppedFrames() const noexcept { return 1; }
    [[nodiscard]] qulonglong syncDroppedFrames() const noexcept { return 2; }
    [[nodiscard]] qulonglong duplicatedFrames() const noexcept { return 4; }
    [[nodiscard]] double maximumDriftMilliseconds() const noexcept { return 8.5; }
    [[nodiscard]] double audioCorrectionPpm() const noexcept { return 125.0; }
    [[nodiscard]] qulonglong diskAvailableBytes() const noexcept {
        return 8ULL * 1024ULL * 1024ULL * 1024ULL;
    }
    [[nodiscard]] QString encoderName() const { return QStringLiteral("mpeg4, aac"); }
    [[nodiscard]] QString takeDuration() const { return QStringLiteral("00:00:00"); }
    [[nodiscard]] QString statusMessage() const { return {}; }
    Q_INVOKABLE void startRecording() {}
    Q_INVOKABLE void stopRecording() {}
};

class FakeScreenCaptureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool previewing READ previewing CONSTANT)
    Q_PROPERTY(bool canStopPreview READ canStopPreview CONSTANT)
    Q_PROPERTY(bool permissionRequired READ permissionRequired CONSTANT)
    Q_PROPERTY(QVariantList targets READ targets CONSTANT)
    Q_PROPERTY(QString selectedTargetId READ selectedTargetId CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)
    Q_PROPERTY(quint32 actualWidth READ actualWidth CONSTANT)
    Q_PROPERTY(quint32 actualHeight READ actualHeight CONSTANT)
    Q_PROPERTY(qulonglong receivedFrames READ receivedFrames CONSTANT)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames CONSTANT)
    Q_PROPERTY(qulonglong ignoredFrames READ ignoredFrames CONSTANT)
    Q_PROPERTY(qulonglong invalidFrames READ invalidFrames CONSTANT)
    Q_PROPERTY(qulonglong replacedPreviewFrames READ replacedPreviewFrames CONSTANT)
    Q_PROPERTY(double currentFps READ currentFps CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool previewing() const noexcept { return false; }
    [[nodiscard]] bool canStopPreview() const noexcept { return false; }
    [[nodiscard]] bool permissionRequired() const noexcept { return false; }
    [[nodiscard]] QVariantList targets() const {
        return {QVariantMap{{QStringLiteral("id"), QStringLiteral("display:1")},
                            {QStringLiteral("kind"), QStringLiteral("display")},
                            {QStringLiteral("name"), QStringLiteral("Built-in Display")},
                            {QStringLiteral("application"), QString{}},
                            {QStringLiteral("width"), 1920},
                            {QStringLiteral("height"), 1080}}};
    }
    [[nodiscard]] QString selectedTargetId() const { return QStringLiteral("display:1"); }
    [[nodiscard]] QString statusMessage() const {
        return QStringLiteral("captured window closed");
    }
    [[nodiscard]] quint32 actualWidth() const noexcept { return 1920; }
    [[nodiscard]] quint32 actualHeight() const noexcept { return 1080; }
    [[nodiscard]] qulonglong receivedFrames() const noexcept { return 60; }
    [[nodiscard]] qulonglong droppedFrames() const noexcept { return 1; }
    [[nodiscard]] qulonglong ignoredFrames() const noexcept { return 2; }
    [[nodiscard]] qulonglong invalidFrames() const noexcept { return 3; }
    [[nodiscard]] qulonglong replacedPreviewFrames() const noexcept { return 2; }
    [[nodiscard]] double currentFps() const noexcept { return 59.94; }

    Q_INVOKABLE void initialize() {}
    Q_INVOKABLE void requestPermission() {}
    Q_INVOKABLE void refreshTargets() {}
    Q_INVOKABLE void selectTarget(const QString&) {}
    Q_INVOKABLE void startPreview() {}
    Q_INVOKABLE void stopPreview() {}
};

class FakeDeviceCaptureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList cameras READ cameras CONSTANT)
    Q_PROPERTY(QVariantList microphones READ microphones CONSTANT)
    Q_PROPERTY(QString selectedCameraId READ selectedCameraId CONSTANT)
    Q_PROPERTY(QString selectedMicrophoneId READ selectedMicrophoneId CONSTANT)
    Q_PROPERTY(bool cameraPermissionRequired READ cameraPermissionRequired CONSTANT)
    Q_PROPERTY(bool microphonePermissionRequired READ microphonePermissionRequired CONSTANT)
    Q_PROPERTY(bool cameraCapturing READ cameraCapturing CONSTANT)
    Q_PROPERTY(bool microphoneCapturing READ microphoneCapturing CONSTANT)
    Q_PROPERTY(bool systemAudioCapturing READ systemAudioCapturing CONSTANT)
    Q_PROPERTY(bool cameraCanStop READ cameraCanStop CONSTANT)
    Q_PROPERTY(bool microphoneCanStop READ microphoneCanStop CONSTANT)
    Q_PROPERTY(bool systemAudioCanStop READ systemAudioCanStop CONSTANT)
    Q_PROPERTY(bool cameraBusy READ cameraBusy CONSTANT)
    Q_PROPERTY(bool microphoneBusy READ microphoneBusy CONSTANT)
    Q_PROPERTY(bool systemAudioBusy READ systemAudioBusy CONSTANT)
    Q_PROPERTY(QString cameraStatus READ cameraStatus CONSTANT)
    Q_PROPERTY(QString microphoneStatus READ microphoneStatus CONSTANT)
    Q_PROPERTY(QString systemAudioStatus READ systemAudioStatus CONSTANT)
    Q_PROPERTY(quint32 cameraWidth READ cameraWidth CONSTANT)
    Q_PROPERTY(quint32 cameraHeight READ cameraHeight CONSTANT)
    Q_PROPERTY(double cameraFps READ cameraFps CONSTANT)
    Q_PROPERTY(double microphonePeakDbfs READ microphonePeakDbfs CONSTANT)
    Q_PROPERTY(double microphoneRmsDbfs READ microphoneRmsDbfs CONSTANT)
    Q_PROPERTY(double systemAudioPeakDbfs READ systemAudioPeakDbfs CONSTANT)
    Q_PROPERTY(double systemAudioRmsDbfs READ systemAudioRmsDbfs CONSTANT)
    Q_PROPERTY(qulonglong microphoneBlocks READ microphoneBlocks CONSTANT)
    Q_PROPERTY(qulonglong systemAudioBlocks READ systemAudioBlocks CONSTANT)
    Q_PROPERTY(qulonglong microphoneOverruns READ microphoneOverruns CONSTANT)
    Q_PROPERTY(qulonglong systemAudioOverruns READ systemAudioOverruns CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] QVariantList cameras() const {
        return {QVariantMap{{QStringLiteral("id"), QStringLiteral("camera:1")},
                            {QStringLiteral("name"), QStringLiteral("Studio Camera")},
                            {QStringLiteral("default"), true}}};
    }
    [[nodiscard]] QVariantList microphones() const {
        return {QVariantMap{{QStringLiteral("id"), QStringLiteral("mic:1")},
                            {QStringLiteral("name"), QStringLiteral("Studio Microphone")},
                            {QStringLiteral("default"), true}}};
    }
    [[nodiscard]] QString selectedCameraId() const { return QStringLiteral("camera:1"); }
    [[nodiscard]] QString selectedMicrophoneId() const { return QStringLiteral("mic:1"); }
    [[nodiscard]] bool cameraPermissionRequired() const noexcept { return false; }
    [[nodiscard]] bool microphonePermissionRequired() const noexcept { return false; }
    [[nodiscard]] bool cameraCapturing() const noexcept { return true; }
    [[nodiscard]] bool microphoneCapturing() const noexcept { return true; }
    [[nodiscard]] bool systemAudioCapturing() const noexcept { return true; }
    [[nodiscard]] bool cameraCanStop() const noexcept { return true; }
    [[nodiscard]] bool microphoneCanStop() const noexcept { return true; }
    [[nodiscard]] bool systemAudioCanStop() const noexcept { return true; }
    [[nodiscard]] bool cameraBusy() const noexcept { return false; }
    [[nodiscard]] bool microphoneBusy() const noexcept { return false; }
    [[nodiscard]] bool systemAudioBusy() const noexcept { return false; }
    [[nodiscard]] QString cameraStatus() const { return QStringLiteral("Camera capturing"); }
    [[nodiscard]] QString microphoneStatus() const {
        return QStringLiteral("Microphone capturing");
    }
    [[nodiscard]] QString systemAudioStatus() const {
        return QStringLiteral("System audio capturing");
    }
    [[nodiscard]] quint32 cameraWidth() const noexcept { return 1920; }
    [[nodiscard]] quint32 cameraHeight() const noexcept { return 1080; }
    [[nodiscard]] double cameraFps() const noexcept { return 30.0; }
    [[nodiscard]] double microphonePeakDbfs() const noexcept { return -6.0; }
    [[nodiscard]] double microphoneRmsDbfs() const noexcept { return -12.0; }
    [[nodiscard]] double systemAudioPeakDbfs() const noexcept { return -9.0; }
    [[nodiscard]] double systemAudioRmsDbfs() const noexcept { return -18.0; }
    [[nodiscard]] qulonglong microphoneBlocks() const noexcept { return 42; }
    [[nodiscard]] qulonglong systemAudioBlocks() const noexcept { return 43; }
    [[nodiscard]] qulonglong microphoneOverruns() const noexcept { return 0; }
    [[nodiscard]] qulonglong systemAudioOverruns() const noexcept { return 1; }

    Q_INVOKABLE void initialize() {}
    Q_INVOKABLE void refreshDevices() {}
    Q_INVOKABLE void requestCameraPermission() {}
    Q_INVOKABLE void requestMicrophonePermission() {}
    Q_INVOKABLE void selectCamera(const QString&) {}
    Q_INVOKABLE void selectMicrophone(const QString&) {}
    Q_INVOKABLE void setCameraEnabled(bool) {}
    Q_INVOKABLE void setMicrophoneEnabled(bool) {}
    Q_INVOKABLE void setSystemAudioEnabled(bool) {}
};

class FakeEditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel* mediaBinModel READ mediaBinModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* timelineTrackModel READ timelineTrackModel CONSTANT)
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool previewStale READ previewStale CONSTANT)
    Q_PROPERTY(bool playing READ playing CONSTANT)
    Q_PROPERTY(qlonglong playheadNs READ playheadNs CONSTANT)
    Q_PROPERTY(qlonglong timelineRevision READ timelineRevision CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)

public:
    explicit FakeEditorController(QObject* parent = nullptr) : QObject(parent) {
        using namespace creator;
        const auto media = domain::MediaAsset::create(
                               domain::AssetId::create("screen").value(),
                               domain::MediaKind::Video,
                               "media/화면 녹화.mp4", core::DurationNs{5'000'000'000},
                               domain::VideoAssetMetadata{
                                   1920, 1080, core::FrameRate::create(60, 1).value()},
                               std::nullopt, 42'000, "fingerprint",
                               domain::AssetAvailability::Offline)
                               .value();
        mediaBin_.setAssets({media});
        auto timeline =
            domain::Timeline::create(domain::TimelineId::create("main").value(),
                                     "강의 편집",
                                     core::FrameRate::create(60, 1).value())
                .value();
        static_cast<void>(timeline.addTrack(
            domain::Track::create(domain::TrackId::create("v1").value(),
                                  domain::TrackKind::Video, "화면", true, false)
                .value()));
        const auto source = domain::TimeRange::create(
                                core::TimestampNs{core::DurationNs::zero()},
                                core::DurationNs{2'000'000'000})
                                .value();
        const auto placed = domain::TimeRange::create(
                                core::TimestampNs{core::DurationNs{1'000'000'000}},
                                core::DurationNs{2'000'000'000})
                                .value();
        static_cast<void>(timeline.insertClip(
            domain::TrackId::create("v1").value(),
            domain::Clip::createAsset(domain::ClipId::create("clip-1").value(),
                                      media, source, placed, true, std::nullopt,
                                      std::nullopt)
                .value()));
        tracks_.setTimeline(std::move(timeline));
    }

    [[nodiscard]] QAbstractItemModel* mediaBinModel() noexcept {
        return &mediaBin_;
    }
    [[nodiscard]] QAbstractItemModel* timelineTrackModel() noexcept {
        return &tracks_;
    }
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool previewStale() const noexcept { return true; }
    [[nodiscard]] bool playing() const noexcept { return false; }
    [[nodiscard]] qlonglong playheadNs() const noexcept { return 1'000'000'000; }
    [[nodiscard]] qlonglong timelineRevision() const noexcept { return 4; }
    [[nodiscard]] QString statusMessage() const {
        return QStringLiteral("Preview engine unavailable");
    }
    Q_INVOKABLE void play() {}
    Q_INVOKABLE void pause() {}
    Q_INVOKABLE void seek(qlonglong) {}

private:
    creator::app::MediaBinModel mediaBin_;
    creator::app::TimelineTrackModel tracks_;
};

TEST(QmlSmokeTest, RecoveryPageLoadsWithProjectControllerContract) {
    QQmlEngine engine;
    FakeProjectController controller;
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"), &controller);
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/RecoveryPage.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
}

TEST(QmlSmokeTest, MainOpensRecoveryWhenStartupScanAlreadyFinished) {
    QQmlEngine engine;
    FakeProjectController projectController;
    FakeStudioController studioController;
    FakeScreenCaptureController screenCaptureController;
    FakeDeviceCaptureController deviceCaptureController;
    FakeEditorController editorController;
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("editorController"),
                                             &editorController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/Main.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    EXPECT_EQ(object->property("currentPage").toString(), QStringLiteral("Recovery"));
}

TEST(QmlSmokeTest, EditorPageRendersTypedModelsAndPreviewState) {
    QQmlEngine engine;
    FakeEditorController controller;
    QQmlComponent component{
        &engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/EditorPage.qml"))};
    QVariantMap initialProperties{
        {QStringLiteral("controller"),
         QVariant::fromValue(static_cast<QObject*>(&controller))},
        {QStringLiteral("width"), 1200},
        {QStringLiteral("height"), 720}};

    QQuickWindow window;
    window.setGeometry(0, 0, 1200, 720);
    std::unique_ptr<QObject> object{
        component.createWithInitialProperties(initialProperties)};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* rootItem = qobject_cast<QQuickItem*>(object.get());
    ASSERT_NE(rootItem, nullptr);
    rootItem->setParentItem(window.contentItem());
    window.show();
    for (int attempt = 0; attempt < 20; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    auto* media = findVisualItem(rootItem, QStringLiteral("mediaAsset-screen"));
    auto* mediaList =
        object->findChild<QObject*>(QStringLiteral("editorMediaList"));
    auto* offline = findVisualItem(rootItem, QStringLiteral("mediaOffline-screen"));
    auto* track = findVisualItem(rootItem, QStringLiteral("timelineTrack-v1"));
    auto* clip = findVisualItem(rootItem, QStringLiteral("timelineClip-clip-1"));
    auto* preview = object->findChild<QObject*>(QStringLiteral("editorPreviewState"));
    auto* status = object->findChild<QObject*>(QStringLiteral("editorStatus"));
    ASSERT_NE(mediaList, nullptr);
    ASSERT_EQ(mediaList->property("count").toInt(), 1);
    ASSERT_NE(media, nullptr);
    ASSERT_NE(offline, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(clip, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(status, nullptr);
    EXPECT_TRUE(media->property("text").toString().contains(
        QString::fromUtf8("화면 녹화.mp4")));
    EXPECT_TRUE(offline->property("visible").toBool());
    EXPECT_EQ(track->property("text").toString(), QString::fromUtf8("화면"));
    EXPECT_GT(clip->property("width").toDouble(), 0.0);
    EXPECT_TRUE(preview->property("text").toString().contains(
        QStringLiteral("stale"), Qt::CaseInsensitive));
    EXPECT_EQ(status->property("text").toString(),
              QStringLiteral("Preview engine unavailable"));
}

TEST(QmlSmokeTest, StudioPageShowsCaptureTargetsAndTerminalError) {
    QQmlEngine engine;
    FakeStudioController studioController;
    FakeScreenCaptureController screenCaptureController;
    FakeDeviceCaptureController deviceCaptureController;
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/StudioPage.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* selector = object->findChild<QObject*>(QStringLiteral("captureTargetSelector"));
    auto* status = object->findChild<QObject*>(QStringLiteral("captureStatusLabel"));
    auto* preview = object->findChild<QObject*>(QStringLiteral("nativeScreenPreview"));
    auto* cameraSelector = object->findChild<QObject*>(QStringLiteral("cameraDeviceSelector"));
    auto* microphoneSelector =
        object->findChild<QObject*>(QStringLiteral("microphoneDeviceSelector"));
    auto* microphoneMeter = object->findChild<QObject*>(QStringLiteral("microphoneLevelMeter"));
    auto* disk = object->findChild<QObject*>(QStringLiteral("recordingDiskLabel"));
    auto* encoder = object->findChild<QObject*>(QStringLiteral("recordingEncoderLabel"));
    auto* queue = object->findChild<QObject*>(QStringLiteral("recordingQueueLabel"));
    auto* sync = object->findChild<QObject*>(QStringLiteral("recordingSyncLabel"));
    ASSERT_NE(selector, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(preview, nullptr);
    ASSERT_NE(cameraSelector, nullptr);
    ASSERT_NE(microphoneSelector, nullptr);
    ASSERT_NE(microphoneMeter, nullptr);
    ASSERT_NE(disk, nullptr);
    ASSERT_NE(encoder, nullptr);
    ASSERT_NE(queue, nullptr);
    ASSERT_NE(sync, nullptr);
    EXPECT_TRUE(disk->property("text").toString().contains(QStringLiteral("8.0 GiB")));
    EXPECT_TRUE(encoder->property("text").toString().contains(QStringLiteral("mpeg4, aac")));
    EXPECT_TRUE(queue->property("text").toString().contains(QStringLiteral("Queue: 3")));
    EXPECT_TRUE(sync->property("text").toString().contains(QStringLiteral("duplicate 4")));
    EXPECT_TRUE(sync->property("text").toString().contains(QStringLiteral("8.5 ms")));
    EXPECT_EQ(selector->property("count").toInt(), 1);
    EXPECT_EQ(status->property("text").toString(),
              QStringLiteral("captured window closed"));
    EXPECT_EQ(cameraSelector->property("count").toInt(), 1);
    EXPECT_EQ(microphoneSelector->property("count").toInt(), 1);
    EXPECT_GT(microphoneMeter->property("value").toDouble(), 0.0);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    qmlRegisterType<creator::app::ScreenPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "ScreenPreviewItem");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "QmlSmokeTest.moc"
