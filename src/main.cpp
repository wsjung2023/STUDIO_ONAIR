#include "app/ProjectController.h"
#include "app/ProjectEditorBinding.h"
#include "app/EditorController.h"
#include "app/ExportController.h"
#include "app/EditorPreviewItem.h"
#include "app/DeviceCaptureController.h"
#include "app/LiveRecordingController.h"
#include "app/LiveRecordingEngineFactory.h"
#include "app/ScreenCaptureController.h"
#include "app/ScreenPreviewItem.h"
#include "app/ShortcutSettingsController.h"
#include "app/CameraPreviewItem.h"
#include "app/StudioRecordingBinding.h"
#include "app/StudioWorkflowController.h"
#include "app/RecordingTimelineReconciler.h"
#include "core/AppError.h"
#include "core/Uuid.h"
#include "core/Utc.h"
#include "capture/UnsupportedScreenCaptureBackend.h"
#include "capture/UnsupportedDeviceCaptureBackend.h"
#if defined(__APPLE__)
#include "capture/macos/MacScreenCaptureBackend.h"
#include "capture/macos/MacDeviceCaptureBackend.h"
#endif
#if defined(_WIN32)
#include "capture/windows/WindowsScreenCaptureBackend.h"
#endif
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"
#if defined(CS_APP_ENABLE_FFMPEG)
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#if defined(_WIN32)
#include "ffmpeg_adapter/windows/WindowsCaptureBackend.h"
#endif
#endif
#include "edit_engine/UnavailableEditEngine.h"
#if defined(CS_APP_ENABLE_RNNOISE)
#include "audio_dsp/AudioCleanupChain.h"
#include "audio_dsp/AudioFormat.h"
#include "rnnoise_adapter/RnnoiseDenoiseProcessor.h"
#include <QDebug>
#endif
#if defined(CS_APP_ENABLE_MLT)
#include "app/ProjectExportEngine.h"
#include "mlt_adapter/MltEditEngine.h"
#endif

#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <qqml.h>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <filesystem>
#include <memory>

namespace {

class UnavailableRecordingTimelineReconciler final
    : public creator::app::IRecordingTimelineReconciler {
public:
    [[nodiscard]] creator::core::Result<creator::app::RecordingReconcileResult>
    reconcile(const std::filesystem::path&,
              const creator::domain::SessionId&) override {
        return creator::core::AppError{
            creator::core::ErrorCode::InvalidState,
            "FFmpeg media inspection is unavailable in this build"};
    }
};

#if defined(CS_APP_ENABLE_MLT)
std::filesystem::path stagedMltRuntimeRoot() {
#if defined(_WIN32)
    return std::filesystem::path{
               QGuiApplication::applicationDirPath().toStdWString()} /
           L"mlt-runtime";
#else
    return std::filesystem::path{
               QGuiApplication::applicationDirPath().toStdString()} /
           "mlt-runtime";
#endif
}

#endif

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("CreatorStudio"));
    QGuiApplication::setApplicationName(QStringLiteral("Creator Studio"));

    // Bundle Pretendard (OFL) and make it the application default so the entire
    // Korean UI renders in one consistent, premium typeface instead of the host
    // system font. Failure to load any weight is logged but non-fatal.
    const QStringList fontResources{
        QStringLiteral(":/fonts/Pretendard-Regular.otf"),
        QStringLiteral(":/fonts/Pretendard-Medium.otf"),
        QStringLiteral(":/fonts/Pretendard-SemiBold.otf"),
        QStringLiteral(":/fonts/Pretendard-Bold.otf")};
    for (const QString& resource : fontResources) {
        if (QFontDatabase::addApplicationFont(resource) < 0) {
            qWarning("Failed to load bundled font %s", qUtf8Printable(resource));
        }
    }
    QFont appFont(QStringLiteral("Pretendard"));
    appFont.setPixelSize(14);
    appFont.setStyleStrategy(QFont::PreferAntialias);
    QGuiApplication::setFont(appFont);

    // Load the Korean UI translation. The whole product ships in Korean, so the
    // translator is installed regardless of the host locale.
    auto* translator = new QTranslator(&app);
    if (translator->load(QStringLiteral(":/i18n/creator_studio_ko.qm"))) {
        QCoreApplication::installTranslator(translator);
    } else {
        qWarning("Failed to load Korean translation resource");
    }

    // The Material style gives Qt Quick Controls a modern, dark, elevation-aware
    // look out of the box; Theme (qml/Theme.qml) layers the product palette,
    // typography and spacing on top. Set before any QML loads.
    QQuickStyle::setStyle(QStringLiteral("Material"));

    qmlRegisterType<creator::app::ScreenPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "ScreenPreviewItem");
    qmlRegisterType<creator::app::CameraPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "CameraPreviewItem");
    qmlRegisterType<creator::app::EditorPreviewItem>("CreatorStudio.Native", 1, 0,
                                                      "EditorPreviewItem");

    auto packageStore = std::make_unique<creator::project_store::ProjectPackageStore>();
    creator::app::ProjectController projectController{std::move(packageStore), &app};
#if defined(__APPLE__)
    creator::app::DeviceCaptureController deviceCaptureController{
        creator::capture::macos::makeMacDeviceCaptureBackend(), &app};
    auto screenCaptureBackend = creator::capture::macos::makeMacScreenCaptureBackend();
    creator::app::ScreenCaptureController screenCaptureController{
        std::move(screenCaptureBackend.permission), std::move(screenCaptureBackend.discovery),
        std::move(screenCaptureBackend.sourceFactory), &app};
#elif defined(_WIN32) && defined(CS_APP_ENABLE_FFMPEG)
    // Audited-FFmpeg Windows build: the gdigrab screen source feeds the FFmpeg
    // recording engine, whose CpuBgraFrameMapper depends on that path's frame
    // handle type, so screen capture stays on gdigrab here.
    auto windowsCaptureBackend =
        creator::ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    creator::app::DeviceCaptureController deviceCaptureController{
        std::move(windowsCaptureBackend.devices), &app};
    creator::app::ScreenCaptureController screenCaptureController{
        std::move(windowsCaptureBackend.screenPermission),
        std::move(windowsCaptureBackend.screenDiscovery),
        std::move(windowsCaptureBackend.screenSourceFactory), &app};
#elif defined(_WIN32)
    // Default Windows build: real desktop capture via Windows.Graphics.Capture +
    // Direct3D11. Replaces the "native screen capture is unavailable" fallback.
    creator::app::DeviceCaptureController deviceCaptureController{
        std::make_unique<creator::capture::UnsupportedDeviceCaptureBackend>(), &app};
    auto screenCaptureBackend =
        creator::capture::windows::makeWindowsScreenCaptureBackend();
    creator::app::ScreenCaptureController screenCaptureController{
        std::move(screenCaptureBackend.permission),
        std::move(screenCaptureBackend.discovery),
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
    auto studioPackageStore =
        std::make_shared<creator::project_store::ProjectPackageStore>();
#if defined(CS_APP_ENABLE_FFMPEG)
    creator::ffmpeg_adapter::FfmpegMediaProbe studioMediaProbe;
    std::unique_ptr<creator::app::IRecordingTimelineReconciler>
        recordingReconciler =
            std::make_unique<creator::app::RecordingTimelineReconciler>(
                studioMediaProbe,
                [] { return creator::core::generateUuidV4(); },
                [] { return creator::core::Utc::now(); });
#else
    std::unique_ptr<creator::app::IRecordingTimelineReconciler>
        recordingReconciler =
            std::make_unique<UnavailableRecordingTimelineReconciler>();
#endif
    creator::app::StudioWorkflowController studioWorkflowController{
        [studioPackageStore](const std::filesystem::path& packageRoot)
            -> creator::core::Result<std::unique_ptr<
                creator::project_store::IStudioStore>> {
            auto opened = studioPackageStore->open(packageRoot);
            if (!opened.hasValue()) return opened.error();
            const auto lease = opened.value().databaseIdentityLease;
            if (!lease) {
                return creator::core::AppError{
                    creator::core::ErrorCode::IoFailure,
                    "validated Studio database identity is missing"};
            }
            auto store = creator::project_store::SqliteStudioStore::open(
                opened.value().databasePath,
                opened.value().package.manifest.projectId,
                [lease] { return lease->verifyCurrentIdentity(); });
            if (!store.hasValue()) return store.error();
            return std::unique_ptr<creator::project_store::IStudioStore>{
                new creator::project_store::SqliteStudioStore{
                    std::move(store).value()}};
        },
        std::move(recordingReconciler),
        [] { return creator::core::generateUuidV4(); }, &app};
    creator::app::ShortcutSettingsController shortcutSettingsController{&app};
#if defined(CS_APP_ENABLE_MLT)
    const auto mltRuntimeRoot = stagedMltRuntimeRoot();
    std::shared_ptr<creator::audio_dsp::IAudioProcessor> audioProcessingChain;
#if defined(CS_APP_ENABLE_RNNOISE)
    auto denoise = creator::rnnoise_adapter::createRnnoiseDenoiseProcessor(
        std::filesystem::path{CS_APP_RNNOISE_ROOT});
    if (denoise.hasValue()) {
        // Cleanup chain: denoise -> compressor -> true-peak limiter, at the
        // export/preview consumer's 48 kHz stereo format (MltEditEngine
        // normalizes audio to 48 kHz; the export consumer is 2ch). Loudness
        // standardization (음량 표준화) is applied separately, offline.
        const auto cleanupFormat =
            creator::audio_dsp::AudioFormat::create(48'000, 2).value();
        if (auto chain = creator::audio_dsp::makeAudioCleanupChain(
                cleanupFormat, std::move(denoise).value());
            chain.hasValue()) {
            audioProcessingChain = std::move(chain).value();
        } else {
            qWarning().noquote() << "Audio cleanup chain unavailable:"
                                 << QString::fromStdString(chain.error().message());
        }
    } else {
        qWarning().noquote() << "RNNoise runtime unavailable:" << QString::fromStdString(
            denoise.error().message());
    }
#endif
    std::unique_ptr<creator::edit_engine::IEditEngine> editEngine =
        std::make_unique<creator::mlt_adapter::MltEditEngine>(
            creator::mlt_adapter::MltEditEngineConfig{
                .runtimeRoot = mltRuntimeRoot,
                .audioProcessingChain = std::move(audioProcessingChain)});
    std::unique_ptr<creator::edit_engine::IEditEngine> exportEngine =
        std::make_unique<creator::app::ProjectExportEngine>(mltRuntimeRoot);
#else
    std::unique_ptr<creator::edit_engine::IEditEngine> editEngine =
        std::make_unique<creator::edit_engine::UnavailableEditEngine>();
    std::unique_ptr<creator::edit_engine::IEditEngine> exportEngine =
        std::make_unique<creator::edit_engine::UnavailableEditEngine>();
#endif
    creator::app::EditorController editorController{std::move(editEngine), &app};
    creator::app::ExportController exportController{std::move(exportEngine), &app};
    creator::app::StudioRecordingBinding studioRecordingBinding{
        studioController, studioWorkflowController,
        [&projectController] { return projectController.projectUrl(); }, &app};
    static_cast<void>(
        creator::app::bindProjectEditor(projectController, editorController,
                                        studioRecordingBinding));
    const auto updateExportSource = [&projectController, &editorController,
                                     &exportController] {
        auto snapshot = editorController.exportSnapshot();
        if (!snapshot.has_value()) {
            exportController.clearSource();
            return;
        }
        auto projectId = creator::domain::ProjectId::create(
            projectController.projectId().toStdString());
        if (!projectId.hasValue()) {
            creator::project_store::ProjectPackageStore packages;
            auto opened = packages.open(snapshot->mediaRoot);
            if (!opened.hasValue()) {
                exportController.clearSource();
                return;
            }
            projectId = opened.value().package.manifest.projectId;
        }
        exportController.setSource(std::move(projectId).value(),
                                   std::move(*snapshot));
    };
    QObject::connect(&editorController, &creator::app::EditorController::timelineChanged,
                     &app, updateExportSource);
    QObject::connect(&projectController, &creator::app::ProjectController::projectChanged,
                     &app, updateExportSource);

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("studioController"),
                                             &studioController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("studioWorkflowController"),
        &studioWorkflowController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("shortcutSettingsController"),
        &shortcutSettingsController);
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"),
                                             &projectController);
    engine.rootContext()->setContextProperty(QStringLiteral("screenCaptureController"),
                                             &screenCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("deviceCaptureController"),
                                             &deviceCaptureController);
    engine.rootContext()->setContextProperty(QStringLiteral("editorController"),
                                             &editorController);
    engine.rootContext()->setContextProperty(QStringLiteral("exportController"),
                                             &exportController);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);

    engine.loadFromModule("CreatorStudio", "Main");

    // Optional non-interactive drive for verification. When CS_AUTODRIVE_DIR is
    // set the app creates a project, starts the primary-display screen preview
    // and system-audio capture, screenshots the live preview, records ~5s and
    // stops -- exercising the exact application services the QML buttons call.
    // Absent the variable, the app behaves normally.
    if (qEnvironmentVariableIsSet("CS_AUTODRIVE_DIR") &&
        !qEnvironmentVariable("CS_AUTODRIVE_DIR").isEmpty()) {
        const QString autoDriveDir = qEnvironmentVariable("CS_AUTODRIVE_DIR");
        QDir().mkpath(autoDriveDir);

        struct AutoDriveState {
            int phase = 0;
            int phaseTicks = 0;
            bool systemAudioRequested = false;
            bool workflowOpenRequested = false;
            bool recordRequested = false;
            int recordAttempts = 0;
            QElapsedTimer recordTimer;
            int totalTicks = 0;
        };
        auto state = std::make_shared<AutoDriveState>();

        auto* driver = new QTimer(&app);
        driver->setInterval(150);
        const auto advance = [state](int next) {
            state->phase = next;
            state->phaseTicks = 0;
        };
        QObject::connect(driver, &QTimer::timeout, &app,
            [&engine, &projectController, &screenCaptureController,
             &deviceCaptureController, &studioController, &studioWorkflowController,
             autoDriveDir, driver, state, advance]() mutable {
            ++state->phaseTicks;
            if (++state->totalTicks > 800) {  // ~2 min hard cap
                qInfo("[autodrive] timeout, quitting");
                QCoreApplication::exit(2);
                return;
            }
            switch (state->phase) {
            case 0: {  // screenshot the Home page, then create project
                if (state->phaseTicks < 10) return;  // let Home render
                auto* homeWindow = engine.rootObjects().isEmpty()
                    ? nullptr
                    : qobject_cast<QQuickWindow*>(
                          engine.rootObjects().constFirst());
                if (homeWindow != nullptr) {
                    const QImage homeShot = homeWindow->grabWindow();
                    const QString homePath =
                        autoDriveDir + QStringLiteral("/home.png");
                    if (!homeShot.isNull() && homeShot.save(homePath)) {
                        qInfo("[autodrive] saved home screenshot: %s (%dx%d)",
                              qUtf8Printable(homePath), homeShot.width(),
                              homeShot.height());
                    } else {
                        qWarning("[autodrive] failed to save home screenshot");
                    }
                }
                const QString packagePath =
                    autoDriveDir + QStringLiteral("/autodrive.cstudio");
                qInfo("[autodrive] creating project at %s",
                      qUtf8Printable(packagePath));
                projectController.createProject(
                    QUrl::fromLocalFile(packagePath),
                    QStringLiteral("AutoDrive"));
                advance(1);
                break;
            }
            case 1: {  // wait for project open, then start capture sources
                if (!projectController.hasOpenProject()) return;
                if (!state->workflowOpenRequested) {
                    // The app's own projectOpened -> Studio-open binding is
                    // responsible for opening the Studio workflow. Observe its
                    // result here; do NOT re-drive openProject, so this path
                    // proves a plain Record works off the real binding alone.
                    qInfo("[autodrive] app-binding workflow busy=%d status=\"%s\"",
                          static_cast<int>(studioWorkflowController.busy()),
                          qUtf8Printable(studioWorkflowController.statusMessage()));
                    const QUrl url = projectController.projectUrl();
                    qInfo("[autodrive] project url=\"%s\" (local=%d)",
                          qUtf8Printable(url.toString()),
                          static_cast<int>(url.isLocalFile()));
                    state->workflowOpenRequested = true;
                }
                if (screenCaptureController.state() ==
                        creator::app::ScreenCaptureState::Ready &&
                    !screenCaptureController.previewing() &&
                    !screenCaptureController.selectedTargetId().isEmpty()) {
                    qInfo("[autodrive] starting screen preview for target %s",
                          qUtf8Printable(screenCaptureController.selectedTargetId()));
                    screenCaptureController.startPreview();
                }
                if (!state->systemAudioRequested) {
                    qInfo("[autodrive] starting system audio");
                    deviceCaptureController.setSystemAudioEnabled(true);
                    state->systemAudioRequested = true;
                }
                const bool audioReady =
                    deviceCaptureController.systemAudioCapturing();
                if (screenCaptureController.receivedFrames() > 0 &&
                    (audioReady || state->phaseTicks > 40)) {
                    qInfo("[autodrive] screen frames=%llu, systemAudio capturing=%d"
                          " status=\"%s\"",
                          static_cast<unsigned long long>(
                              screenCaptureController.receivedFrames()),
                          static_cast<int>(audioReady),
                          qUtf8Printable(deviceCaptureController.systemAudioStatus()));
                    advance(2);
                }
                break;
            }
            case 2: {  // let a few frames render, then screenshot the preview
                if (state->phaseTicks < 12) return;  // ~1.8s of live frames
                auto* window = engine.rootObjects().isEmpty()
                    ? nullptr
                    : qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
                if (window != nullptr) {
                    const QImage shot = window->grabWindow();
                    const QString shotPath =
                        autoDriveDir + QStringLiteral("/preview.png");
                    if (!shot.isNull() && shot.save(shotPath)) {
                        qInfo("[autodrive] saved preview screenshot: %s (%dx%d)",
                              qUtf8Printable(shotPath), shot.width(),
                              shot.height());
                    } else {
                        qWarning("[autodrive] failed to save preview screenshot");
                    }
                }
                advance(3);
                break;
            }
            case 3: {  // start recording (the Studio DB opens async after the
                       // project does, so retry until the take actually starts)
                if (!studioController.recordingAvailable()) {
                    qWarning("[autodrive] recording unavailable: %s",
                             qUtf8Printable(studioController.statusMessage()));
                    if (state->phaseTicks > 20) advance(6);
                    return;
                }
                if (studioController.isBusy()) return;
                if (studioWorkflowController.busy()) return;  // wait for DB open
                ++state->recordAttempts;
                qInfo("[autodrive] starting recording (attempt %d); workflow="
                      "\"%s\"",
                      state->recordAttempts,
                      qUtf8Printable(studioWorkflowController.statusMessage()));
                studioController.startRecording();
                state->recordRequested = true;
                advance(4);
                break;
            }
            case 4: {  // wait until recording is live, else retry from phase 3
                if (studioController.isRecording()) {
                    qInfo("[autodrive] recording is live");
                    state->recordTimer.start();
                    advance(5);
                    return;
                }
                if (!studioController.isBusy() && state->phaseTicks > 6) {
                    if (state->recordAttempts < 20) {
                        advance(3);  // start failed and settled; retry
                    } else {
                        qWarning("[autodrive] recording did not start: %s",
                                 qUtf8Printable(studioController.statusMessage()));
                        advance(6);
                    }
                }
                break;
            }
            case 5: {  // record ~5s then stop
                if (state->recordTimer.elapsed() >= 5000) {
                    qInfo("[autodrive] stopping recording after %lld ms",
                          static_cast<long long>(state->recordTimer.elapsed()));
                    studioController.stopRecording();
                    advance(6);
                }
                break;
            }
            case 6: {  // wait for finalize, then quit
                if (!studioController.isRecording() &&
                    !studioController.isBusy()) {
                    if (state->phaseTicks < 8) return;  // grace for finalize
                    qInfo("[autodrive] done: %s",
                          qUtf8Printable(studioController.statusMessage()));
                    driver->stop();
                    QCoreApplication::quit();
                }
                break;
            }
            default:
                break;
            }
        });
        driver->start();
    }

    return app.exec();
}
