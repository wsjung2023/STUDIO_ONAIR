# Deliberately NOT cs_add_qtfree_library: this is the application layer, the
# first place Qt is allowed. Using plain add_library here is the visible marker
# of that boundary (ARCHITECTURE.md 14).
#
# AUTOMOC comes from qt_standard_project_setup() in the root CMakeLists, so
# Q_OBJECT works without extra wiring.
add_library(cs_app STATIC
    RecordingImportPlanner.h
    RecordingImportPlanner.cpp
    RecordingTimelineReconciler.h
    RecordingTimelineReconciler.cpp
    IRecordingPersistence.h
    ProjectSegmentLifecycleSink.h
    ProjectSegmentLifecycleSink.cpp
    MultiTrackRecordingService.h
    MultiTrackRecordingService.cpp
    ILiveRecordingEngine.h
    ILiveCaptureBindings.h
    ControllerLiveCaptureBindings.h
    ControllerLiveCaptureBindings.cpp
    LiveRecordingEngineFactory.h
    LiveRecordingEngineFactory.cpp
    TranscriptionProviderFactory.h
    TranscriptionProviderFactory.cpp
    LiveRecordingController.h
    LiveRecordingController.cpp
    StudioRecordingBinding.h
    StudioRecordingBinding.cpp
    StudioController.h
    StudioController.cpp
    ScreenCaptureController.h
    ScreenCaptureController.cpp
    PreviewGeometry.h
    PreviewGeometry.cpp
    DeviceCaptureController.h
    DeviceCaptureController.cpp
    ScreenPreviewItem.h
    ScreenPreviewItem.cpp
    VideoPreviewItem.h
    VideoPreviewItem.cpp
    CameraPreviewItem.h
    CameraPreviewItem.cpp
    RecentProjectRegistry.h
    RecentProjectRegistry.cpp
    ProjectWorker.h
    ProjectWorker.cpp
    ProjectController.h
    ProjectController.cpp
    ProjectEditorBinding.h
    ProjectEditorBinding.cpp
    TimelineEditService.h
    TimelineEditService.cpp
    EditorSessionTypes.h
    GeneratedOverlayCache.h
    GeneratedOverlayCache.cpp
    EditorSessionWorker.h
    EditorSessionWorker.cpp
    MediaBinModel.h
    MediaBinModel.cpp
    StudioSceneModel.h
    StudioSceneModel.cpp
    StudioSourceModel.h
    StudioSourceModel.cpp
    StudioWorkflowTypes.h
    StudioWorkflowWorker.h
    StudioWorkflowWorker.cpp
    StudioWorkflowController.h
    StudioWorkflowController.cpp
    ShortcutSettingsController.h
    ShortcutSettingsController.cpp
    TimelineTrackModel.h
    TimelineTrackModel.cpp
    EditorEngineWorker.h
    EditorEngineWorker.cpp
    EditorController.h
    EditorController.cpp
    EditorPreviewItem.h
    EditorPreviewItem.cpp
    ExportWorker.h
    ExportWorker.cpp
    IExportDestinationResolver.h
    IExportDestinationResolver.cpp
    ExportController.h
    ExportController.cpp
    AvatarFrameTrackingCoordinator.h
    AvatarFrameTrackingCoordinator.cpp
)

if(CS_ENABLE_FFMPEG OR ANDROID)
    target_sources(cs_app PRIVATE
        FfmpegLiveRecordingEngine.h
        FfmpegLiveRecordingEngine.cpp
    )
    target_compile_definitions(cs_app PRIVATE CS_APP_ENABLE_LIVE_RECORDING=1)
endif()

if(CS_ENABLE_FFMPEG)
    target_compile_definitions(cs_app PRIVATE CS_APP_ENABLE_FFMPEG=1)
endif()

if(CS_ENABLE_MLT)
    target_sources(cs_app PRIVATE
        ProjectExportEngine.h
        ProjectExportEngine.cpp
    )
    target_link_libraries(cs_app PRIVATE cs_mlt_adapter)
endif()

if(APPLE)
    find_library(CS_APP_COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(CS_APP_IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(CS_APP_METAL_FRAMEWORK Metal REQUIRED)
    target_sources(cs_app PRIVATE VideoPreviewItem_macos.mm)
    set_source_files_properties(VideoPreviewItem_macos.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
    )
    target_link_libraries(cs_app PRIVATE
        ${CS_APP_COREVIDEO_FRAMEWORK}
        ${CS_APP_IOSURFACE_FRAMEWORK}
        ${CS_APP_METAL_FRAMEWORK}
    )
else()
    target_sources(cs_app PRIVATE VideoPreviewItem_stub.cpp)
endif()

if(ANDROID)
    target_sources(cs_app PRIVATE
        android/AndroidDeviceCaptureBackend.h
        android/AndroidDeviceCaptureBackend.cpp
        android/AndroidScreenCaptureBackend.h
        android/AndroidScreenCaptureBackend.cpp
        android/AndroidExportDestinationResolver.h
        android/AndroidExportDestinationResolver.cpp
        android/AndroidDeviceProfile.h
        android/AndroidDeviceProfile.cpp
        android/AndroidMediaCodecSession.h
        android/AndroidMediaCodecSession.cpp
        android/AndroidMediaCodecSegmentEncoder.h
        android/AndroidMediaCodecSegmentEncoder.cpp
    )
endif()
add_library(CreatorStudio::cs_app ALIAS cs_app)

target_include_directories(cs_app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(cs_app PUBLIC cxx_std_20)

target_link_libraries(cs_app
    PUBLIC
        Qt6::Core
        Qt6::Quick
        cs_capture
        cs_avatar
        cs_recorder
        cs_domain
        cs_edit_engine
        cs_transcription
        cs_platform_release
    PRIVATE
        cs_project_store
)

if(CS_ENABLE_FFMPEG)
    target_link_libraries(cs_app PRIVATE cs_ffmpeg_adapter)
endif()

if(CS_ENABLE_FFMPEG OR ANDROID)
    target_link_libraries(cs_app PRIVATE cs_sync)
endif()

if(CS_ENABLE_WHISPER)
    target_link_libraries(cs_app PRIVATE cs_whisper_adapter)
    target_compile_definitions(cs_app PRIVATE CS_APP_ENABLE_WHISPER=1)
endif()

cs_apply_warnings(cs_app)
