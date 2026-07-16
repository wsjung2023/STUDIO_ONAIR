include(FetchContent)

# GoogleTest is BSD-3-Clause and test-only: it is never linked into a shipped
# binary. Recorded as such in legal/OSS_BOM.csv (CLAUDE.md 7).
# The tag is pinned so the BOM entry keeps describing what we actually build.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# Without this, MSVC links gtest against the static CRT while our targets use
# the dynamic CRT, and the link fails with mismatched runtime library errors.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(cs_crash_recovery_fixture
    helpers/CrashRecoveryFixture.cpp
)
target_link_libraries(cs_crash_recovery_fixture PRIVATE cs_project_store)
set_target_properties(cs_crash_recovery_fixture PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
cs_apply_warnings(cs_crash_recovery_fixture)

add_executable(cs_tests
    core/ResultTest.cpp
    core/TimebaseTest.cpp
    core/UuidTest.cpp
    core/UtcTest.cpp
    domain/IdentifiersTest.cpp
    domain/ProjectManifestTest.cpp
    domain/RecordingSessionTest.cpp
    media/MediaTypesTest.cpp
    sync/ClockCoordinatorTest.cpp
    sync/VideoSyncPlannerTest.cpp
    capture/PortContractTest.cpp
    capture/CaptureTimestampMapperTest.cpp
    capture/ScreenCapturePortsTest.cpp
    capture/LatestVideoFrameMailboxTest.cpp
    capture/CaptureFanoutSinksTest.cpp
    capture/UnsupportedScreenCaptureBackendTest.cpp
    capture/NumericCaptureTargetIdTest.cpp
    capture/ScreenCaptureFrameAssemblerTest.cpp
    capture/ScreenCaptureStopCoordinatorTest.cpp
    capture/DeviceCapturePortsTest.cpp
    capture/AudioCapturePrimitivesTest.cpp
    capture/AudioPcmLayoutTest.cpp
    capture/DeviceCaptureAssemblerTest.cpp
    capture/DeviceCaptureStopCoordinatorTest.cpp
    recorder/RecordingTrackTest.cpp
    recorder/DiskSpaceMonitorTest.cpp
    recorder/DurableSegmentPublisherTest.cpp
    recorder/AsyncTrackRecorderTest.cpp
    project_store/ManifestSchemaValidatorTest.cpp
    project_store/JsonProjectStoreTest.cpp
    project_store/SqliteConnectionTest.cpp
    project_store/MigrationRunnerTest.cpp
    project_store/SqliteProjectDatabaseTest.cpp
    project_store/SegmentPersistenceTest.cpp
    project_store/RecoveryTest.cpp
    project_store/ProjectPackageStoreTest.cpp
    project_store/CrashRecoveryIntegrationTest.cpp
    fakes/FakeCaptureSourceTest.cpp
    fakes/ManualPushCaptureSourceTest.cpp
    fakes/FakeRecorderTest.cpp
)

target_link_libraries(cs_tests
    PRIVATE
        cs_core
        cs_domain
        cs_media
        cs_sync
        cs_capture
        cs_recorder
        cs_project_store
        cs_fakes
        nlohmann_json::nlohmann_json
        GTest::gtest_main
)

target_compile_definitions(cs_tests PRIVATE
    CS_CRASH_FIXTURE_PATH="$<TARGET_FILE:cs_crash_recovery_fixture>"
)
add_dependencies(cs_tests cs_crash_recovery_fixture)

cs_apply_warnings(cs_tests)

gtest_discover_tests(cs_tests)

# Separate binary: these need a QCoreApplication, so they cannot use
# gtest_main. Keeping them apart also keeps cs_tests Qt-free, which is a
# standing check that the layers below the application really do build without
# Qt on the include path.
add_executable(cs_app_tests
    app/StudioControllerTest.cpp
    app/LiveRecordingControllerTest.cpp
    app/ProjectControllerTest.cpp
    app/ScreenCaptureControllerTest.cpp
    app/PreviewGeometryTest.cpp
    app/DeviceCaptureControllerTest.cpp
    app/ProjectSegmentLifecycleSinkTest.cpp
    app/MultiTrackRecordingServiceTest.cpp
)

target_link_libraries(cs_app_tests
    PRIVATE
        cs_app
        cs_fakes
        # QSignalSpy lives in QtTest, not QtCore; cs_app itself only needs
        # Qt6::Core, so the test binary requests Test for itself rather than
        # widening what the shipped library links.
        Qt6::Test
        GTest::gtest
)

target_include_directories(cs_app_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

cs_apply_warnings(cs_app_tests)

gtest_discover_tests(cs_app_tests)

add_executable(cs_qml_tests
    app/QmlSmokeTest.cpp
)

target_link_libraries(cs_qml_tests
    PRIVATE
        cs_app
        Qt6::Quick
        Qt6::Test
        GTest::gtest
)

target_compile_definitions(cs_qml_tests PRIVATE
    CS_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml"
)

cs_apply_warnings(cs_qml_tests)

gtest_discover_tests(cs_qml_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

if(CS_ENABLE_FFMPEG)
    add_executable(cs_ffmpeg_tests
        ffmpeg_adapter/FfmpegCapabilityProbeTest.cpp
        ffmpeg_adapter/FfmpegVideoSegmentEncoderTest.cpp
        ffmpeg_adapter/FfmpegAudioSegmentEncoderTest.cpp
        ffmpeg_adapter/BgraFrameMappersTest.cpp
        ffmpeg_adapter/RecordingPipelineIntegrationTest.cpp
        ffmpeg_adapter/LongTimelineRecordingTest.cpp
        ffmpeg_adapter/FfmpegLiveRecordingEngineTest.cpp
    )
    target_link_libraries(cs_ffmpeg_tests PRIVATE
        cs_ffmpeg_adapter
        cs_app
        cs_project_store
        GTest::gtest_main
    )
    target_include_directories(cs_ffmpeg_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(cs_ffmpeg_tests PRIVATE ${FFMPEG_LIBRARIES})
    cs_apply_warnings(cs_ffmpeg_tests)

    if(WIN32)
        file(GLOB CS_FFMPEG_RUNTIME_DLLS CONFIGURE_DEPENDS "${CS_FFMPEG_ROOT}/bin/*.dll")
        add_custom_command(TARGET cs_ffmpeg_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CS_FFMPEG_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:cs_ffmpeg_tests>
            COMMAND_EXPAND_LISTS
        )
    elseif(APPLE)
        set_property(TARGET cs_ffmpeg_tests APPEND PROPERTY
            BUILD_RPATH "${CS_FFMPEG_ROOT}/lib"
        )
    endif()

    gtest_discover_tests(cs_ffmpeg_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${CS_FFMPEG_ROOT}/bin"
    )
endif()
