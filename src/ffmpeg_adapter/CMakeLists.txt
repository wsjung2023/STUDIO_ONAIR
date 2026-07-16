add_library(cs_ffmpeg_adapter STATIC
    FfmpegCapabilityProbe.h
    FfmpegCapabilityProbe.cpp
    FfmpegVideoSegmentEncoder.h
    FfmpegVideoSegmentEncoder.cpp
)
add_library(CreatorStudio::cs_ffmpeg_adapter ALIAS cs_ffmpeg_adapter)

target_include_directories(cs_ffmpeg_adapter
    PUBLIC ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(cs_ffmpeg_adapter SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_compile_features(cs_ffmpeg_adapter PUBLIC cxx_std_20)
target_link_libraries(cs_ffmpeg_adapter
    PUBLIC cs_recorder
    PRIVATE ${FFMPEG_LIBRARIES}
)
cs_apply_warnings(cs_ffmpeg_adapter)
cs_assert_qt_free(cs_ffmpeg_adapter)
