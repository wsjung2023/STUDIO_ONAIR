#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

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
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    QQmlComponent component{
        &engine, QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/Main.qml"))};

    std::unique_ptr<QObject> object{component.create()};

    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
    EXPECT_EQ(object->property("currentPage").toString(), QStringLiteral("Recovery"));
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "QmlSmokeTest.moc"
