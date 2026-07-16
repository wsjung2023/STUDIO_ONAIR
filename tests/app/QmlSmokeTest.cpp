#include "app/ScreenPreviewItem.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <gtest/gtest.h>

#include <memory>

namespace {

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
    Q_PROPERTY(int segmentCount READ segmentCount CONSTANT)
    Q_PROPERTY(QString takeDuration READ takeDuration CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool recording() const noexcept { return false; }
    [[nodiscard]] int segmentCount() const noexcept { return 0; }
    [[nodiscard]] QString takeDuration() const { return QStringLiteral("00:00:00"); }
    [[nodiscard]] QString statusMessage() const { return {}; }
    Q_INVOKABLE void startRecording() {}
    Q_INVOKABLE void stopRecording() {}
};

class FakeScreenCaptureController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(bool previewing READ previewing CONSTANT)
    Q_PROPERTY(bool permissionRequired READ permissionRequired CONSTANT)
    Q_PROPERTY(QVariantList targets READ targets CONSTANT)
    Q_PROPERTY(QString selectedTargetId READ selectedTargetId CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)
    Q_PROPERTY(quint32 actualWidth READ actualWidth CONSTANT)
    Q_PROPERTY(quint32 actualHeight READ actualHeight CONSTANT)
    Q_PROPERTY(qulonglong receivedFrames READ receivedFrames CONSTANT)
    Q_PROPERTY(qulonglong droppedFrames READ droppedFrames CONSTANT)
    Q_PROPERTY(qulonglong replacedPreviewFrames READ replacedPreviewFrames CONSTANT)
    Q_PROPERTY(double currentFps READ currentFps CONSTANT)

public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] bool previewing() const noexcept { return false; }
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
    [[nodiscard]] qulonglong replacedPreviewFrames() const noexcept { return 2; }
    [[nodiscard]] double currentFps() const noexcept { return 59.94; }

    Q_INVOKABLE void initialize() {}
    Q_INVOKABLE void requestPermission() {}
    Q_INVOKABLE void refreshTargets() {}
    Q_INVOKABLE void selectTarget(const QString&) {}
    Q_INVOKABLE void startPreview() {}
    Q_INVOKABLE void stopPreview() {}
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
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/Main.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    EXPECT_EQ(object->property("currentPage").toString(), QStringLiteral("Recovery"));
}

TEST(QmlSmokeTest, StudioPageShowsCaptureTargetsAndTerminalError) {
    QQmlEngine engine;
    FakeStudioController studioController;
    FakeScreenCaptureController screenCaptureController;
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/StudioPage.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    auto* selector = object->findChild<QObject*>(QStringLiteral("captureTargetSelector"));
    auto* status = object->findChild<QObject*>(QStringLiteral("captureStatusLabel"));
    auto* preview = object->findChild<QObject*>(QStringLiteral("nativeScreenPreview"));
    ASSERT_NE(selector, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(preview, nullptr);
    EXPECT_EQ(selector->property("count").toInt(), 1);
    EXPECT_EQ(status->property("text").toString(),
              QStringLiteral("captured window closed"));
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
