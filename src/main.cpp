#include "app/ProjectController.h"
#include "app/EditorController.h"
#include "app/DeviceCaptureController.h"
#include "app/LiveRecordingController.h"
#include "app/LiveRecordingEngineFactory.h"
#include "app/ScreenCaptureController.h"
#include "app/ScreenPreviewItem.h"
#include "capture/UnsupportedScreenCaptureBackend.h"
#include "capture/UnsupportedDeviceCaptureBackend.h"
#if defined(__APPLE__)
#include "capture/macos/MacScreenCaptureBackend.h"
#include "capture/macos/MacDeviceCaptureBackend.h"
#endif
#include "project_store/ProjectPackageStore.h"
#include "edit_engine/UnavailableEditEngine.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <qqml.h>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("CreatorStudio"));
    QGuiApplication::setApplicationName(QStringLiteral("Creator Studio"));
    qmlRegisterType<creator::app::ScreenPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "ScreenPreviewItem");

    auto packageStore = std::make_unique<creator::project_store::ProjectPackageStore>();
    creator::app::ProjectController projectController{std::move(packageStore), &app};
#if defined(__APPLE__)
    creator::app::DeviceCaptureController deviceCaptureController{
        creator::capture::macos::makeMacDeviceCaptureBackend(), &app};
    auto screenCaptureBackend = creator::capture::macos::makeMacScreenCaptureBackend();
    creator::app::ScreenCaptureController screenCaptureController{
        std::move(screenCaptureBackend.permission), std::move(screenCaptureBackend.discovery),
        std::move(screenCaptureBackend.sourceFactory), &app};
#else
    creator::app::DeviceCaptureController deviceCaptureController{
        std::make_unique<creator::capture::UnsupportedDeviceCaptureBackend>(), &app};
    creator::app::ScreenCaptureController screenCaptureController{
        std::make_unique<creator::capture::UnsupportedScreenCapturePermission>(),
        std::make_unique<creator::capture::UnsupportedScreenCaptureDiscovery>(),
        std::make_unique<creator::capture::UnsupportedScreenCaptureSourceFactory>(), &app};
#endif
    auto recordingStore =
        std::make_shared<creator::project_store::ProjectPackageStore>();
    auto recordingEngine = creator::app::makeLiveRecordingEngine(
        &screenCaptureController, &deviceCaptureController, std::move(recordingStore));
    creator::app::LiveRecordingController studioController{
        std::move(recordingEngine), &projectController,
        [&projectController] { return projectController.recordingPackagePath(); },
        [] { return creator::core::ProjectClock::now(); }, &app};
    creator::app::EditorController editorController{
        std::make_unique<creator::edit_engine::UnavailableEditEngine>(), &app};

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("editorController"),
                                             &editorController);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);

    engine.loadFromModule("CreatorStudio", "Main");
    return app.exec();
}
