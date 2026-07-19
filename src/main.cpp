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
#elif defined(ANDROID)
#include "app/android/AndroidDeviceCaptureBackend.h"
#include "app/android/AndroidScreenCaptureBackend.h"
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
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <qqml.h>

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
#elif defined(ANDROID)
    creator::app::DeviceCaptureController deviceCaptureController{
        creator::app::android::makeAndroidDeviceCaptureBackend(), &app};
    auto androidScreenCaptureBackend = creator::app::android::makeAndroidScreenCaptureBackend();
    creator::app::ScreenCaptureController screenCaptureController{
        std::move(androidScreenCaptureBackend.permission),
        std::move(androidScreenCaptureBackend.discovery),
        std::move(androidScreenCaptureBackend.sourceFactory), &app};
#elif defined(_WIN32) && defined(CS_APP_ENABLE_FFMPEG)
    auto windowsCaptureBackend =
        creator::ffmpeg_adapter::windows::makeWindowsCaptureBackend();
    creator::app::DeviceCaptureController deviceCaptureController{
        std::move(windowsCaptureBackend.devices), &app};
    creator::app::ScreenCaptureController screenCaptureController{
        std::move(windowsCaptureBackend.screenPermission),
        std::move(windowsCaptureBackend.screenDiscovery),
        std::move(windowsCaptureBackend.screenSourceFactory), &app};
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
    return app.exec();
}
