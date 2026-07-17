add_library(cs_ffmpeg_adapter STATIC
    FfmpegCapabilityProbe.h
    FfmpegCapabilityProbe.cpp
    FfmpegMediaProbe.h
    FfmpegMediaProbe.cpp
    FfmpegVideoSegmentEncoder.h
    FfmpegVideoSegmentEncoder.cpp
    FfmpegAudioSegmentEncoder.h
    FfmpegAudioSegmentEncoder.cpp
    BgraFrameMappers.h
    BgraFrameMappers.cpp
)

if(APPLE)
    target_sources(cs_ffmpeg_adapter PRIVATE BgraFrameMappers_macos.mm)
    find_library(CS_FFMPEG_COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
    target_link_libraries(cs_ffmpeg_adapter PRIVATE ${CS_FFMPEG_COREVIDEO_FRAMEWORK})
endif()
add_library(CreatorStudio::cs_ffmpeg_adapter ALIAS cs_ffmpeg_adapter)

target_include_directories(cs_ffmpeg_adapter
    PUBLIC ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(cs_ffmpeg_adapter SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_compile_features(cs_ffmpeg_adapter PUBLIC cxx_std_20)
target_link_libraries(cs_ffmpeg_adapter
    PUBLIC cs_recorder cs_media
    PRIVATE cs_sync ${FFMPEG_LIBRARIES}
)
cs_apply_warnings(cs_ffmpeg_adapter)
cs_assert_qt_free(cs_ffmpeg_adapter)
