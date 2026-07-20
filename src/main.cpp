#include "app/ProjectController.h"
#include "app/ProjectEditorBinding.h"
#include "app/EditorController.h"
#include "app/ExportController.h"
#include "app/EditorPreviewItem.h"
#include "app/DeviceCaptureController.h"
#include "app/LiveRecordingController.h"
#include "app/CursorRecordingBinding.h"
#include "app/LiveRecordingEngineFactory.h"
#include "app/ScreenCaptureController.h"
#include "app/ScreenPreviewItem.h"
#include "app/ShortcutSettingsController.h"
#include "app/CameraPreviewItem.h"
#include "app/CommercialControlsController.h"
#include "app/StudioRecordingBinding.h"
#include "app/StudioWorkflowController.h"
#include "app/RecordingTimelineReconciler.h"
#include "app/CreatorIntelligenceController.h"
#include "app/TranscriptionProviderFactory.h"
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
#include "app/android/AndroidDeviceProfile.h"
#include "app/android/AndroidExportDestinationResolver.h"
#include "app/android/AndroidMediaCodecEditEngine.h"
#include "app/android/AndroidScreenCaptureBackend.h"
#endif
#include "project_store/ProjectPackageStore.h"
#include "project_store/SqliteStudioStore.h"
#include "platform_release/EntitlementPolicy.h"
#if defined(CS_APP_ENABLE_FFMPEG)
#include "ffmpeg_adapter/FfmpegMediaProbe.h"
#if defined(_WIN32)
#include "ffmpeg_adapter/windows/WindowsCaptureBackend.h"
#endif
#endif
#if defined(_WIN32)
#include "cursor/windows/WindowsRawInputCursorSource.h"
#endif
#include "edit_engine/UnavailableEditEngine.h"
#if defined(CS_APP_ENABLE_MLT)
#include "audio_dsp/AudioCleanupChain.h"
#include "audio_dsp/AudioFormat.h"
#endif
#if defined(CS_APP_ENABLE_RNNOISE)
#include "rnnoise_adapter/RnnoiseDenoiseProcessor.h"
#endif
#if defined(CS_APP_ENABLE_MLT)
#include "app/ProjectExportEngine.h"
#include "app/MltCreatorIntelligenceAudioLoader.h"
#include "mlt_adapter/MltEditEngine.h"
#endif

#include <QGuiApplication>
#include <QDebug>
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
    creator::app::CursorRecordingBinding::SourceFactory cursorSourceFactory;
#if defined(_WIN32)
    cursorSourceFactory = [](const creator::app::LiveRecordingStart&)
        -> creator::core::Result<std::unique_ptr<
            creator::cursor::ICursorSource>> {
        auto created = creator::cursor::windows::WindowsRawInputCursorSource::create();
        if (!created.hasValue()) return created.error();
        return std::unique_ptr<creator::cursor::ICursorSource>{
            std::move(created).value()};
    };
#endif
    creator::app::CursorRecordingBinding cursorRecordingController{
        studioController, std::move(cursorSourceFactory), &app};
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
    const creator::platform_release::EntitlementPolicy entitlementPolicy{
        {.productId = "creator-studio-pro",
         .offlineGraceSeconds = 7 * 24 * 60 * 60,
         .communityBuild = true}};
    creator::app::CommercialControlsController commercialControlsController{
        entitlementPolicy.evaluate({}, {}), &app};
    creator::app::CreatorIntelligenceController::AudioLoader
        intelligenceAudioLoader;
#if defined(CS_APP_ENABLE_MLT)
    const auto mltRuntimeRoot = stagedMltRuntimeRoot();
    creator::mlt_adapter::MltEditEngineConfig::AudioProcessorFactory
        audioProcessingFactory;
    // Every graph gets a fresh cleanup chain. Export creates one for pass 1
    // measurement and another for pass 2 encoding.
#if defined(CS_APP_ENABLE_RNNOISE)
    const auto rnnoiseRuntimeRoot =
        std::filesystem::path{CS_APP_RNNOISE_ROOT};
    audioProcessingFactory = [rnnoiseRuntimeRoot]()
        -> creator::core::Result<std::unique_ptr<
            creator::audio_dsp::IAudioProcessor>> {
        auto denoise = creator::rnnoise_adapter::createRnnoiseDenoiseProcessor(
            rnnoiseRuntimeRoot);
        if (!denoise.hasValue()) return denoise.error();
        const auto cleanupFormat =
            creator::audio_dsp::AudioFormat::create(48'000, 2).value();
        auto chain = creator::audio_dsp::makeAudioCleanupChain(
            cleanupFormat, std::move(denoise).value());
        if (!chain.hasValue()) return chain.error();
        return std::unique_ptr<creator::audio_dsp::IAudioProcessor>{
            std::move(chain).value()};
    };
#else
    audioProcessingFactory = []()
        -> creator::core::Result<std::unique_ptr<
            creator::audio_dsp::IAudioProcessor>> {
        const auto cleanupFormat =
            creator::audio_dsp::AudioFormat::create(48'000, 2).value();
        auto chain = creator::audio_dsp::makeAudioCleanupChain(cleanupFormat);
        if (!chain.hasValue()) return chain.error();
        return std::unique_ptr<creator::audio_dsp::IAudioProcessor>{
            std::move(chain).value()};
    };
#endif
    intelligenceAudioLoader =
        creator::app::makeMltCreatorIntelligenceAudioLoader(
            mltRuntimeRoot, audioProcessingFactory);
    std::unique_ptr<creator::edit_engine::IEditEngine> editEngine =
        std::make_unique<creator::mlt_adapter::MltEditEngine>(
            creator::mlt_adapter::MltEditEngineConfig{
                .runtimeRoot = mltRuntimeRoot,
                .audioProcessingFactory = audioProcessingFactory});
    std::unique_ptr<creator::edit_engine::IEditEngine> exportEngine =
        std::make_unique<creator::app::ProjectExportEngine>(
            mltRuntimeRoot, audioProcessingFactory);
#elif defined(ANDROID)
    std::unique_ptr<creator::edit_engine::IEditEngine> editEngine =
        std::make_unique<creator::edit_engine::UnavailableEditEngine>();
    std::unique_ptr<creator::edit_engine::IEditEngine> exportEngine =
        std::make_unique<creator::app::android::AndroidMediaCodecEditEngine>();
#else
    std::unique_ptr<creator::edit_engine::IEditEngine> editEngine =
        std::make_unique<creator::edit_engine::UnavailableEditEngine>();
    std::unique_ptr<creator::edit_engine::IEditEngine> exportEngine =
        std::make_unique<creator::edit_engine::UnavailableEditEngine>();
#endif
#if !defined(CS_APP_ENABLE_MLT)
    intelligenceAudioLoader =
        [](const creator::edit_engine::TimelineSnapshot&, std::stop_token)
        -> creator::core::Result<std::vector<float>> {
        return creator::core::AppError{
            creator::core::ErrorCode::InvalidState,
            "local AI audio loading requires the MLT desktop build"};
    };
#endif
    creator::app::EditorController editorController{std::move(editEngine), &app};
    creator::app::TranscriptionProviderOptions transcriptionOptions;
#if defined(CS_APP_ENABLE_WHISPER)
    transcriptionOptions.whisperRuntimeRoot =
        std::filesystem::path{CS_APP_WHISPER_ROOT};
#endif
    creator::app::CreatorIntelligenceController intelligenceController{
        creator::app::makeTranscriptionProvider(transcriptionOptions),
        std::move(intelligenceAudioLoader),
        [&editorController] { return editorController.exportSnapshot(); },
        [&editorController](const creator::transcription::Transcript& transcript,
                            std::int64_t expectedRevision) {
            return editorController.approveTranscriptProposal(
                transcript, expectedRevision);
        },
        [&editorController](const creator::domain::TimeRange& range,
                            std::int64_t expectedRevision) {
            return editorController.approveCutProposal(
                range, expectedRevision, true);
        },
        &app};
#if defined(ANDROID)
    creator::app::ExportController exportController{
        std::move(exportEngine),
        std::make_unique<creator::app::android::AndroidExportDestinationResolver>(),
        &app};
    const auto performancePolicy =
        creator::app::android::currentAndroidPerformancePolicy();
    if (performancePolicy.hasValue()) {
        const auto& budget = performancePolicy.value().budget();
        exportController.setResourceConstraints(
            budget.maximumExportHeight, budget.foregroundExportRequired,
            budget.exportAllowed);
    } else {
        qWarning().noquote()
            << QString::fromStdString(performancePolicy.error().message());
        exportController.setResourceConstraints(1'080, true, true);
    }
    exportController.setApplicationActive(
        app.applicationState() == Qt::ApplicationActive);
    QObject::connect(
        &app, &QGuiApplication::applicationStateChanged, &exportController,
        [&exportController](Qt::ApplicationState state) {
            exportController.setApplicationActive(
                state == Qt::ApplicationActive);
        });
#else
    creator::app::ExportController exportController{std::move(exportEngine), &app};
#endif
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
        QStringLiteral("cursorRecordingController"),
        &cursorRecordingController);
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
    engine.rootContext()->setContextProperty(
        QStringLiteral("intelligenceController"), &intelligenceController);
    engine.rootContext()->setContextProperty(QStringLiteral("exportController"),
                                             &exportController);
    engine.rootContext()->setContextProperty(
        QStringLiteral("commercialControlsController"),
        &commercialControlsController);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);

    engine.loadFromModule("CreatorStudio", "Main");
    return app.exec();
}
