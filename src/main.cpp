#include "app/ProjectController.h"
#include "app/ScreenCaptureController.h"
#include "app/StudioController.h"
#include "capture/UnsupportedScreenCaptureBackend.h"
#if defined(__APPLE__)
#include "capture/macos/MacScreenCaptureBackend.h"
#endif
#include "domain/Identifiers.h"
#include "fakes/FakeCaptureSource.h"
#include "fakes/FakeRecorder.h"
#include "project_store/ProjectPackageStore.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("CreatorStudio"));
    QGuiApplication::setApplicationName(QStringLiteral("Creator Studio"));

    auto packageStore = std::make_unique<creator::project_store::ProjectPackageStore>();
    creator::app::ProjectController projectController{std::move(packageStore), &app};
#if defined(__APPLE__)
    auto screenCaptureBackend = creator::capture::macos::makeMacScreenCaptureBackend();
    creator::app::ScreenCaptureController screenCaptureController{
        std::move(screenCaptureBackend.permission), std::move(screenCaptureBackend.discovery),
        std::move(screenCaptureBackend.sourceFactory), &app};
#else
    creator::app::ScreenCaptureController screenCaptureController{
        std::make_unique<creator::capture::UnsupportedScreenCapturePermission>(),
        std::make_unique<creator::capture::UnsupportedScreenCaptureDiscovery>(),
        std::make_unique<creator::capture::UnsupportedScreenCaptureSourceFactory>(), &app};
#endif
    creator::app::StudioController studioController{
        std::make_unique<creator::fakes::FakeCaptureSource>(
            creator::domain::SourceId::create("screen-1").value(), "Test Pattern"),
        std::make_unique<creator::fakes::FakeRecorder>(), &projectController, &app};

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);

    engine.loadFromModule("CreatorStudio", "Main");
    return app.exec();
}
