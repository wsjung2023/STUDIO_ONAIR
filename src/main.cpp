#include "app/ProjectController.h"
#include "app/ProjectEditorBinding.h"
#include "app/EditorController.h"
#include "app/ExportController.h"
#include "app/EditorPreviewItem.h"
#include "app/AvatarPreviewItem.h"
#include "app/AvatarSceneController.h"
#include "app/DeviceCaptureController.h"
#include "avatar/AvatarParameterMapper.h"
#include "avatar/CharacterAvatarRenderer.h"
#include "avatar/IAvatarRenderer.h"
#include "avatar/PlaceholderAvatarRenderer.h"
#include "avatar/SyntheticFaceTrackingProvider.h"
#include "avatar/openseeface/OpenSeeFaceTrackingProvider.h"
#include "avatar/inochi2d/Inochi2dAvatarRenderer.h"
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
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
// WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+) hides the window from screen
// capture while leaving it fully visible to the user. Defined defensively in
// case the toolchain's headers predate it.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
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
#include <QVector>
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
#if defined(_WIN32)
    // Editor-preview audio (QAudioSink) needs a QtMultimedia backend. Qt's
    // default ffmpeg backend links avcodec-61, which clashes with the
    // avcodec-62 this app ships on PATH and then fails to load ("No
    // QtMultimedia backends found" -> silent editor). Force the native
    // Windows backend, which has no FFmpeg dependency, unless the user
    // explicitly chose one.
    if (qEnvironmentVariableIsEmpty("QT_MEDIA_BACKEND")) {
        qputenv("QT_MEDIA_BACKEND", "windows");
    }
#endif
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
    qmlRegisterType<creator::app::AvatarPreviewItem>("CreatorStudio.Native", 1, 0,
                                                     "AvatarPreviewItem");

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
    // Live VTuber avatar source: synthetic (fake) tracking drives a rendered
    // avatar that composites into the Studio preview and records to its own
    // media/avatar track, exactly like the camera/screen sources. No real
    // face-tracking model (MediaPipe/OpenSeeFace) runs here, and no Inochi2D
    // runtime/model asset is bundled, so this uses a clearly-labelled synthetic
    // tracker and a placeholder puppet renderer. If a real Inochi2D runtime DLL
    // and .inp model are staged next to the executable the adapter is used
    // instead; both facts are surfaced in the UI (avatarSceneController.trackingLabel).
    // 16:9 to match the Studio stage's aspect so the live preview is not
    // pillar-boxed/stretched and what the user drags matches the baked frame.
    constexpr std::uint32_t kAvatarWidth = 640;
    constexpr std::uint32_t kAvatarHeight = 360;
    auto avatarBindings = creator::avatar::characterAvatarBindings();
    auto avatarMapper =
        creator::avatar::AvatarParameterMapper::create(std::move(avatarBindings));
    std::unique_ptr<creator::avatar::IAvatarRenderer> avatarRenderer;
    // Non-owning alias to the built-in character renderer (when active), so the
    // Studio avatar picker and front/corner placement controls can retune it
    // live. Null if a real Inochi2D model is loaded instead.
    creator::avatar::CharacterAvatarRenderer* avatarCharacterControl = nullptr;
    bool avatarRealModel = false;
    {
        const std::filesystem::path appDir{
            QGuiApplication::applicationDirPath().toStdWString()};
        const auto modelPath = appDir / L"resources" / L"avatar" / L"model.inp";
        const auto runtimePath = appDir / L"inochi2d-c.dll";
        std::error_code modelError;
        std::error_code runtimeError;
        if (std::filesystem::is_regular_file(modelPath, modelError) &&
            std::filesystem::is_regular_file(runtimePath, runtimeError)) {
            if (auto real = creator::avatar::inochi2d::Inochi2dAvatarRenderer::open(
                    runtimePath, modelPath, kAvatarWidth, kAvatarHeight);
                real.hasValue()) {
                avatarRenderer = std::move(real).value();
                avatarRealModel = true;
            } else {
                qWarning().noquote()
                    << "Inochi2D avatar unavailable, using placeholder:"
                    << QString::fromStdString(real.error().message());
            }
        }
    }
    if (!avatarRenderer) {
        // Default to a bottom-right corner overlay so the live avatar behaves
        // like a picture-in-picture over the screen; 정면(front) is opt-in via
        // the placement toggle.
        auto characterRenderer =
            std::make_unique<creator::avatar::CharacterAvatarRenderer>(
                kAvatarWidth, kAvatarHeight,
                creator::avatar::AvatarCharacterId::Human,
                creator::avatar::AvatarPlacement{
                    creator::avatar::AvatarPlacementMode::Corner,
                    creator::avatar::AvatarCorner::RightBottom},
                // supersample=1: the avatar re-renders in software every 33 ms;
                // 2x supersampling quadrupled that cost (a 1280x720 rasterise
                // per frame) and was a large share of the app's CPU. 1x keeps
                // playback light; the vector art still anti-aliases acceptably.
                1U);
        avatarCharacterControl = characterRenderer.get();
        avatarRenderer = std::move(characterRenderer);
    }
    // Real face tracking: when CS_OPENSEEFACE_UDP is set, an external
    // OpenSeeFace process (facetracker.py) owns the webcam and streams tracking
    // over UDP; this app only receives it. OpenSeeFaceTrackingProvider adapts
    // that UDP feed to ITrackingProvider so the exact same avatar chain is
    // driven by a REAL face instead of the synthetic generator. OpenSeeFace
    // owns the camera in this mode, so the app must not also open it — leave the
    // device camera closed and drive the avatar continuously (cameraLive=true).
    std::unique_ptr<creator::avatar::ITrackingProvider> avatarProvider;
    bool avatarRealTracking = false;
    std::function<bool()> avatarCameraLive;
    if (qEnvironmentVariableIsSet("CS_OPENSEEFACE_UDP")) {
        auto port = static_cast<std::uint16_t>(
            qEnvironmentVariableIntValue("CS_OPENSEEFACE_UDP"));
        if (port == 0) port = creator::avatar::openseeface::kDefaultUdpPort;
        if (auto real =
                creator::avatar::openseeface::OpenSeeFaceTrackingProvider::create(port);
            real.hasValue()) {
            avatarProvider = std::move(real).value();
            avatarRealTracking = true;
            avatarCameraLive = [] { return true; };
            qInfo().noquote() << "Avatar: real OpenSeeFace UDP tracking on port"
                              << port << "(app camera left closed)";
        } else {
            qWarning().noquote()
                << "OpenSeeFace UDP tracking unavailable, using synthetic:"
                << QString::fromStdString(real.error().message());
        }
    }
    if (!avatarProvider) {
        avatarProvider = std::make_unique<creator::avatar::SyntheticFaceTrackingProvider>();
        avatarCameraLive = [&deviceCaptureController] {
            return deviceCaptureController.cameraCapturing();
        };
    }
    creator::app::AvatarSceneController avatarSceneController{
        std::move(avatarProvider),
        std::move(avatarMapper).value(), std::move(avatarRenderer), kAvatarWidth,
        kAvatarHeight, std::move(avatarCameraLive),
        avatarRealModel, avatarRealTracking, avatarCharacterControl, &app};

    auto recordingStore =
        std::make_shared<creator::project_store::ProjectPackageStore>();
    auto recordingEngine = creator::app::makeLiveRecordingEngine(
        &screenCaptureController, &deviceCaptureController,
        std::move(recordingStore), &avatarSceneController);
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
                // Editor preview renders on the CPU. 960x540 (16:9) keeps the
                // software composite roughly twice as cheap as 720p so debug
                // playback is smoother, while staying sharp enough to edit by.
                // Export is unaffected (it renders at full 1920x1080).
                .previewWidth = 960,
                .previewHeight = 540,
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
    engine.rootContext()->setContextProperty(QStringLiteral("avatarSceneController"),
                                             &avatarSceneController);
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

#if defined(_WIN32)
    // Mirror/Droste fix: exclude the Creator Studio window itself from screen
    // capture. When the avatar is in "코너"(corner) overlay mode its transparent
    // background shows the desktop, and capturing the same monitor the app lives
    // on would otherwise feed the preview back into itself indefinitely. With
    // WDA_EXCLUDEFROMCAPTURE the window stays visible to the user but is invisible
    // to WGC/gdigrab, so there is zero feedback even in full-screen capture.
    // Applied on window creation and re-applied whenever a native window appears
    // (winId() is only valid once the platform window exists). A failure is
    // surfaced via qWarning rather than hidden (CLAUDE.md 9).
    //
    // ON by default so the creator can record the very monitor the Studio sits
    // on (their working monitor) without the recording feeding back into itself.
    // The window stays fully visible to the user; only screen-capture sees it as
    // excluded. Set CS_INCLUDE_SELF_IN_CAPTURE to opt out.
    if (!qEnvironmentVariableIsSet("CS_INCLUDE_SELF_IN_CAPTURE")) {
        // DYNAMIC exclusion: hidden from capture only while the screen preview
        // or a recording is actually running (that is when the mirror feedback
        // exists). Excluding the window permanently also made the creator's own
        // screenshots/캡처 of the app impossible, which read as "the app
        // disappears when I try to capture it".
        auto applyCaptureAffinity = [&engine, &screenCaptureController,
                                     &studioController]() {
            const bool capturing = screenCaptureController.previewing() ||
                                   screenCaptureController.canStopPreview() ||
                                   studioController.isRecording();
            for (auto* object : engine.rootObjects()) {
                auto* window = qobject_cast<QQuickWindow*>(object);
                if (window == nullptr) continue;
                const auto handle = reinterpret_cast<HWND>(window->winId());
                if (handle == nullptr) continue;
                if (!SetWindowDisplayAffinity(
                        handle,
                        capturing ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE)) {
                    qWarning(
                        "[capture] SetWindowDisplayAffinity failed (error %lu); "
                        "the app window may appear in screen recordings",
                        static_cast<unsigned long>(GetLastError()));
                }
            }
        };
        QObject::connect(&screenCaptureController,
                         &creator::app::ScreenCaptureController::captureStateChanged,
                         &app, applyCaptureAffinity);
        QObject::connect(
            &studioController,
            &creator::app::LiveRecordingController::recordingChanged, &app,
            applyCaptureAffinity);
        applyCaptureAffinity();
        // Re-apply once the event loop has created native windows, covering the
        // case where winId() was not yet valid at load time.
        QTimer::singleShot(0, &app, applyCaptureAffinity);
    }
#endif

    // Optional non-interactive UX screenshot drive. When CS_UXSHOT_DIR is set the
    // app opens a project (so Studio/Editor/Export are reachable), then cycles the
    // window through desktop (1440x900) and phone (390x844) geometries, visiting
    // each page and saving a PNG per (size,page). It touches no capture hardware,
    // so it is a fast, deterministic way to prove the responsive layout renders.
    if (qEnvironmentVariableIsSet("CS_UXSHOT_DIR") &&
        !qEnvironmentVariable("CS_UXSHOT_DIR").isEmpty()) {
        const QString shotDir = qEnvironmentVariable("CS_UXSHOT_DIR");
        QDir().mkpath(shotDir);

        struct Shot { QString name; int width; int height; QString page; };
        auto shots = std::make_shared<QVector<Shot>>(QVector<Shot>{
            {QStringLiteral("desktop-home"), 1440, 900, QStringLiteral("Home")},
            {QStringLiteral("desktop-studio"), 1440, 900, QStringLiteral("Studio")},
            {QStringLiteral("desktop-editor"), 1440, 900, QStringLiteral("Editor")},
            {QStringLiteral("desktop-export"), 1440, 900, QStringLiteral("Export")},
            {QStringLiteral("mobile-home"), 390, 844, QStringLiteral("Home")},
            {QStringLiteral("mobile-studio"), 390, 844, QStringLiteral("Studio")},
            {QStringLiteral("mobile-editor"), 390, 844, QStringLiteral("Editor")},
            {QStringLiteral("mobile-export"), 390, 844, QStringLiteral("Export")},
        });

        struct UxState { int phase = 0; int ticks = 0; int index = 0; int total = 0; };
        auto ux = std::make_shared<UxState>();

        auto* driver = new QTimer(&app);
        driver->setInterval(90);
        QObject::connect(driver, &QTimer::timeout, &app,
            [&engine, &projectController, shotDir, shots, ux, driver]() mutable {
            ++ux->ticks;
            if (++ux->total > 1200) {
                qWarning("[uxshot] timeout");
                driver->stop();
                QCoreApplication::exit(2);
                return;
            }
            auto* window = engine.rootObjects().isEmpty()
                ? nullptr
                : qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
            if (window == nullptr)
                return;
            switch (ux->phase) {
            case 0: {  // create a project so all pages are reachable
                if (ux->ticks < 6) return;
                const QString packagePath =
                    shotDir + QStringLiteral("/uxshot.cstudio");
                projectController.createProject(
                    QUrl::fromLocalFile(packagePath), QStringLiteral("UX"));
                ux->phase = 1;
                ux->ticks = 0;
                break;
            }
            case 1: {  // wait for the project to open, then begin the shot loop
                if (!projectController.hasOpenProject()) return;
                if (ux->ticks < 4) return;
                ux->phase = 2;
                ux->ticks = 0;
                break;
            }
            case 2: {  // apply geometry + page for the current shot
                if (ux->index >= shots->size()) {
                    qInfo("[uxshot] done (%d shots)", ux->index);
                    driver->stop();
                    QCoreApplication::quit();
                    return;
                }
                const Shot& s = shots->at(ux->index);
                window->setWidth(s.width);
                window->setHeight(s.height);
                window->setProperty("currentPage", s.page);
                ux->phase = 3;
                ux->ticks = 0;
                break;
            }
            case 3: {  // let the resize + page switch settle, then grab
                if (ux->ticks < 10) return;
                const Shot& s = shots->at(ux->index);
                const QImage shot = window->grabWindow();
                const QString path =
                    shotDir + QStringLiteral("/") + s.name + QStringLiteral(".png");
                if (!shot.isNull() && shot.save(path)) {
                    qInfo("[uxshot] saved %s (%dx%d)", qUtf8Printable(path),
                          shot.width(), shot.height());
                } else {
                    qWarning("[uxshot] failed to save %s", qUtf8Printable(path));
                }
                ++ux->index;
                ux->phase = 2;
                ux->ticks = 0;
                break;
            }
            default:
                break;
            }
        });
        driver->start();
    }

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
            bool cameraRequested = false;
            bool avatarRequested = false;
            bool avatarShotSaved = false;
            bool presentationShotSaved = false;
            bool cameraSceneSwitched = false;
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
             &deviceCaptureController, &avatarSceneController, &studioController,
             &studioWorkflowController,
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
                    qInfo("[autodrive] starting system audio + microphone");
                    deviceCaptureController.setSystemAudioEnabled(true);
                    if (!deviceCaptureController.selectedMicrophoneId().isEmpty()) {
                        deviceCaptureController.setMicrophoneEnabled(true);
                    }
                    state->systemAudioRequested = true;
                }
                if (!state->cameraRequested &&
                    deviceCaptureController.cameraState() ==
                        creator::app::DeviceCaptureState::Ready &&
                    !deviceCaptureController.selectedCameraId().isEmpty()) {
                    qInfo("[autodrive] starting camera %s (\"%s\")",
                          qUtf8Printable(deviceCaptureController.selectedCameraId()),
                          qUtf8Printable(deviceCaptureController.cameraStatus()));
                    deviceCaptureController.setCameraEnabled(true);
                    state->cameraRequested = true;
                }
                if (!state->avatarRequested) {
                    qInfo("[autodrive] enabling avatar source (%s)",
                          qUtf8Printable(avatarSceneController.trackingLabel()));
                    avatarSceneController.setAvatarEnabled(true);
                    state->avatarRequested = true;
                }
                const bool audioReady =
                    deviceCaptureController.systemAudioCapturing() ||
                    deviceCaptureController.microphoneCapturing();
                const bool cameraReady =
                    deviceCaptureController.cameraCapturing() &&
                    deviceCaptureController.cameraWidth() > 0;
                if (screenCaptureController.receivedFrames() > 0 &&
                    (audioReady || state->phaseTicks > 40) &&
                    (cameraReady || state->phaseTicks > 60)) {
                    qInfo("[autodrive] screen frames=%llu, systemAudio capturing=%d,"
                          " camera capturing=%d %ux%u fps=%.1f status=\"%s\"",
                          static_cast<unsigned long long>(
                              screenCaptureController.receivedFrames()),
                          static_cast<int>(audioReady),
                          static_cast<int>(deviceCaptureController.cameraCapturing()),
                          deviceCaptureController.cameraWidth(),
                          deviceCaptureController.cameraHeight(),
                          deviceCaptureController.cameraFps(),
                          qUtf8Printable(deviceCaptureController.cameraStatus()));
                    advance(2);
                }
                break;
            }
            case 2: {  // screenshot the live preview, then a full-frame camera shot
                const auto grabTo = [&](const QString& fileName) {
                    auto* window = engine.rootObjects().isEmpty()
                        ? nullptr
                        : qobject_cast<QQuickWindow*>(
                              engine.rootObjects().constFirst());
                    if (window == nullptr) {
                        qWarning("[autodrive] no window to screenshot %s",
                                 qUtf8Printable(fileName));
                        return;
                    }
                    const QImage shot = window->grabWindow();
                    const QString shotPath =
                        autoDriveDir + QStringLiteral("/") + fileName;
                    if (!shot.isNull() && shot.save(shotPath)) {
                        qInfo("[autodrive] saved screenshot: %s (%dx%d)",
                              qUtf8Printable(shotPath), shot.width(), shot.height());
                    } else {
                        qWarning("[autodrive] failed to save screenshot %s",
                                 qUtf8Printable(fileName));
                    }
                };
                // First: the default 'presentation' scene shows the screen with the
                // live camera as a picture-in-picture overlay.
                if (!state->presentationShotSaved && state->phaseTicks >= 12) {
                    grabTo(QStringLiteral("preview.png"));
                    // A dedicated avatar shot proves the rendered, moving avatar
                    // is visible in the Studio preview (it is a PiP overlay in
                    // every scene while the avatar source is capturing).
                    if (avatarSceneController.avatarCapturing()) {
                        qInfo("[autodrive] avatar capturing (%s), status=\"%s\" "
                              "producedFrames=%llu",
                              qUtf8Printable(avatarSceneController.trackingLabel()),
                              qUtf8Printable(avatarSceneController.avatarStatus()),
                              static_cast<unsigned long long>(
                                  avatarSceneController.producedFrames()));
                        grabTo(QStringLiteral("avatar_preview.png"));
                        // Direct render-to-PNG of the avatar frames (bypassing the
                        // Qt Quick preview surface) so the rendered, moving avatar
                        // face is provable even in a headless/automated run. Two
                        // frames a moment apart show the tracking-driven motion.
                        const QImage faceA =
                            avatarSceneController.renderDiagnosticImage(0.0);
                        const QImage faceB =
                            avatarSceneController.renderDiagnosticImage(0.5);
                        if (!faceA.isNull() &&
                            faceA.save(autoDriveDir +
                                       QStringLiteral("/avatar_face_a.png"))) {
                            qInfo("[autodrive] saved avatar_face_a.png (%dx%d)",
                                  faceA.width(), faceA.height());
                        }
                        if (!faceB.isNull() &&
                            faceB.save(autoDriveDir +
                                       QStringLiteral("/avatar_face_b.png"))) {
                            qInfo("[autodrive] saved avatar_face_b.png (%dx%d)",
                                  faceB.width(), faceB.height());
                        }
                        state->avatarShotSaved = true;
                    }
                    state->presentationShotSaved = true;
                    // Switch to the full-frame 'camera' scene so the webcam fills the
                    // composition for an unmistakable camera-preview screenshot.
                    if (deviceCaptureController.cameraCapturing() &&
                        !state->cameraSceneSwitched) {
                        qInfo("[autodrive] switching to full-frame camera scene "
                              "(camera %ux%u fps=%.1f)",
                              deviceCaptureController.cameraWidth(),
                              deviceCaptureController.cameraHeight(),
                              deviceCaptureController.cameraFps());
                        studioWorkflowController.switchScene(
                            QStringLiteral("camera"));
                        state->cameraSceneSwitched = true;
                    }
                    return;
                }
                // Then: grab the full-frame camera preview once the scene switch and a
                // few more live camera frames have rendered.
                if (state->presentationShotSaved && state->phaseTicks >= 24) {
                    grabTo(QStringLiteral("camera_preview.png"));
                    // Restore the presentation scene so the recorded take composites
                    // screen + camera as usual (recording keeps a separate track per
                    // active source regardless of the active scene).
                    if (state->cameraSceneSwitched) {
                        studioWorkflowController.switchScene(
                            QStringLiteral("presentation"));
                    }
                    advance(3);
                }
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
