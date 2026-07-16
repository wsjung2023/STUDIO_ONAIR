# Deliberately NOT cs_add_qtfree_library: this is the application layer, the
# first place Qt is allowed. Using plain add_library here is the visible marker
# of that boundary (ARCHITECTURE.md 14).
#
# AUTOMOC comes from qt_standard_project_setup() in the root CMakeLists, so
# Q_OBJECT works without extra wiring.
add_library(cs_app STATIC
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
    LiveRecordingController.h
    LiveRecordingController.cpp
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
    RecentProjectRegistry.h
    RecentProjectRegistry.cpp
    ProjectWorker.h
    ProjectWorker.cpp
    ProjectController.h
    ProjectController.cpp
)

if(CS_ENABLE_FFMPEG)
    target_sources(cs_app PRIVATE
        FfmpegLiveRecordingEngine.h
        FfmpegLiveRecordingEngine.cpp
    )
    target_compile_definitions(cs_app PRIVATE CS_APP_ENABLE_FFMPEG=1)
endif()

if(APPLE)
    find_library(CS_APP_COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(CS_APP_IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(CS_APP_METAL_FRAMEWORK Metal REQUIRED)
    target_sources(cs_app PRIVATE ScreenPreviewItem_macos.mm)
    set_source_files_properties(ScreenPreviewItem_macos.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
    )
    target_link_libraries(cs_app PRIVATE
        ${CS_APP_COREVIDEO_FRAMEWORK}
        ${CS_APP_IOSURFACE_FRAMEWORK}
        ${CS_APP_METAL_FRAMEWORK}
    )
else()
    target_sources(cs_app PRIVATE ScreenPreviewItem_stub.cpp)
endif()
add_library(CreatorStudio::cs_app ALIAS cs_app)

target_include_directories(cs_app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(cs_app PUBLIC cxx_std_20)

target_link_libraries(cs_app
    PUBLIC
        Qt6::Core
        Qt6::Quick
        cs_capture
        cs_recorder
    PRIVATE
        cs_project_store
)

if(CS_ENABLE_FFMPEG)
    target_link_libraries(cs_app PRIVATE cs_ffmpeg_adapter)
endif()

cs_apply_warnings(cs_app)
