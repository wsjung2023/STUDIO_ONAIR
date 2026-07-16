cs_add_qtfree_library(cs_capture
    SOURCES
        CaptureTimestampMapper.h
        CaptureTimestampMapper.cpp
        ICaptureSource.h
        IPullCaptureSource.h
        IScreenCaptureDiscovery.h
        IScreenCapturePermission.h
        IScreenCaptureSourceFactory.h
        IScreenCaptureSource.h
        IVideoFrameSink.h
        LatestVideoFrameMailbox.h
        LatestVideoFrameMailbox.cpp
        NumericCaptureTargetId.h
        NumericCaptureTargetId.cpp
        ScreenCaptureTypes.h
        ScreenCaptureTypes.cpp
        ScreenCaptureFrameAssembler.h
        ScreenCaptureFrameAssembler.cpp
        ScreenCaptureStopCoordinator.h
        ScreenCaptureStopCoordinator.cpp
        UnsupportedScreenCaptureBackend.h
        UnsupportedScreenCaptureBackend.cpp
    DEPENDS
        cs_domain
        cs_media
)

if(APPLE)
    find_library(CS_SCK_FRAMEWORK ScreenCaptureKit REQUIRED)
    find_library(CS_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(CS_COREMEDIA_FRAMEWORK CoreMedia REQUIRED)
    find_library(CS_COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(CS_FOUNDATION_FRAMEWORK Foundation REQUIRED)

    add_library(cs_capture_macos STATIC
        macos/MacScreenCaptureBackend.h
        macos/MacScreenCaptureBackend.mm
    )
    add_library(CreatorStudio::cs_capture_macos ALIAS cs_capture_macos)
    target_include_directories(cs_capture_macos PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(cs_capture_macos PUBLIC cxx_std_20)
    target_compile_options(cs_capture_macos PRIVATE -fobjc-arc)
    target_link_libraries(cs_capture_macos
        PUBLIC cs_capture
        PRIVATE
            ${CS_SCK_FRAMEWORK}
            ${CS_COREGRAPHICS_FRAMEWORK}
            ${CS_COREMEDIA_FRAMEWORK}
            ${CS_COREVIDEO_FRAMEWORK}
            ${CS_FOUNDATION_FRAMEWORK}
    )
    cs_apply_warnings(cs_capture_macos)
    cs_assert_qt_free(cs_capture_macos)
endif()
