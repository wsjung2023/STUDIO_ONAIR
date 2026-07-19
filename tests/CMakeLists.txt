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
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

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
    ${PROJECT_SOURCE_DIR}/src/app/TimelineEditService.cpp
    core/ResultTest.cpp
    core/TimebaseTest.cpp
    core/UuidTest.cpp
    core/UtcTest.cpp
    core/Sha256Test.cpp
    domain/IdentifiersTest.cpp
    domain/TimelineTypesTest.cpp
    domain/StudioSceneTest.cpp
    domain/MediaAssetTest.cpp
    domain/TimelineTest.cpp
    domain/EditCommandTest.cpp
    domain/ImportRecordingCommandTest.cpp
    domain/GeneratedEditCommandTest.cpp
    domain/DeleteRangeCommandTest.cpp
    edit_engine/EditEngineTypesTest.cpp
    mlt_adapter/MltGraphPlanTest.cpp
    mlt_adapter/FrameEffectsTest.cpp
    domain/ProjectManifestTest.cpp
    domain/RecordingSessionTest.cpp
    media/MediaTypesTest.cpp
    sync/ClockCoordinatorTest.cpp
    sync/VideoSyncPlannerTest.cpp
    sync/AudioRateCompensatorTest.cpp
    sync/SyncPipelineAcceptanceTest.cpp
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
    project_store/SqliteTimelineStoreTest.cpp
    project_store/SqliteStudioStoreTest.cpp
    project_store/SqliteRenderJobStoreTest.cpp
    project_store/RenderJobRecoveryTest.cpp
    project_store/PersistentRenderJobLifecycleTest.cpp
    project_store/SegmentPersistenceTest.cpp
    project_store/RecoveryTest.cpp
    project_store/ProjectPackageStoreTest.cpp
    project_store/CrashRecoveryIntegrationTest.cpp
    app/TimelineEditServiceTest.cpp
    fakes/FakeCaptureSourceTest.cpp
    fakes/ManualPushCaptureSourceTest.cpp
    fakes/FakeRecorderTest.cpp
    fakes/FakeEditEngineTest.cpp
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
        cs_edit_engine
        cs_mlt_graph_plan
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

# cs_cursor (R2-01 cursor telemetry core) is Qt-free, so its tests live in their
# own binary that links no Qt: a standing check that the cursor module really
# does build below the application boundary. The schema path is injected so the
# pipeline test validates serialized events against the single shipped
# schemas/event.schema.json, mirroring ManifestSchemaValidator.
add_executable(cs_cursor_tests
    cursor/CursorValueTest.cpp
    cursor/CursorNormalizerTest.cpp
    cursor/FakeCursorSourceTest.cpp
    cursor/CursorPipelineTest.cpp
    cursor/CursorNdjsonSinkTest.cpp
)

target_link_libraries(cs_cursor_tests
    PRIVATE
        cs_core
        cs_domain
        cs_cursor
        cs_fakes
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)

target_include_directories(cs_cursor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cursor)

target_compile_definitions(cs_cursor_tests PRIVATE
    CS_EVENT_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/event.schema.json"
)

set_target_properties(cs_cursor_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

cs_apply_warnings(cs_cursor_tests)

gtest_discover_tests(cs_cursor_tests)

# cs_autozoom (R2-03 auto-zoom candidate generation core) is Qt-free like
# cs_cursor, so its tests get their own Qt-free binary. The zoom-candidate schema
# path is injected so the serializer test validates against the single shipped
# schemas/zoom_candidate.schema.json, mirroring the cursor pipeline test.
add_executable(cs_autozoom_tests
    autozoom/ZoomValueTest.cpp
    autozoom/AutoZoomAnalyzerTest.cpp
    autozoom/ZoomCandidateSerializerTest.cpp
)

target_link_libraries(cs_autozoom_tests
    PRIVATE
        cs_core
        cs_domain
        cs_cursor
        cs_autozoom
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)

target_include_directories(cs_autozoom_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/autozoom)

target_compile_definitions(cs_autozoom_tests PRIVATE
    CS_ZOOM_CANDIDATE_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/zoom_candidate.schema.json"
)

set_target_properties(cs_autozoom_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

cs_apply_warnings(cs_autozoom_tests)

gtest_discover_tests(cs_autozoom_tests)

# Separate binary: these need a QCoreApplication, so they cannot use
# gtest_main. Keeping them apart also keeps cs_tests Qt-free, which is a
# standing check that the layers below the application really do build without
# Qt on the include path.
add_executable(cs_app_tests
    app/RecordingImportPlannerTest.cpp
    app/RecordingTimelineReconcilerTest.cpp
    app/EditorModelsTest.cpp
    app/ShortcutSettingsControllerTest.cpp
    app/StudioWorkflowControllerTest.cpp
    app/EditorControllerTest.cpp
    app/ExportControllerTest.cpp
    app/EditorSessionWorkerTest.cpp
    app/GeneratedOverlayCacheTest.cpp
    app/StudioControllerTest.cpp
    app/LiveRecordingControllerTest.cpp
    app/StudioRecordingBindingTest.cpp
    app/ProjectControllerTest.cpp
    app/ScreenCaptureControllerTest.cpp
    app/PreviewGeometryTest.cpp
    app/VideoPreviewItemTest.cpp
    app/DeviceCaptureControllerTest.cpp
    app/ProjectSegmentLifecycleSinkTest.cpp
    app/MultiTrackRecordingServiceTest.cpp
    app/QmlSmokeTest.cpp
)

target_link_libraries(cs_app_tests
    PRIVATE
        cs_app
        cs_fakes
        # QSignalSpy lives in QtTest, not QtCore; cs_app itself only needs
        # Qt6::Core, so the test binary requests Test for itself rather than
        # widening what the shipped library links.
        Qt6::Quick
        Qt6::Test
        GTest::gtest
)

target_include_directories(cs_app_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)

cs_apply_warnings(cs_app_tests)

target_compile_definitions(cs_app_tests PRIVATE
    CS_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml"
)

# PRE_TEST discovery launches the binary before per-test ENVIRONMENT properties
# exist. Wrap both discovery and execution so Windows can resolve the Qt DLLs.
set_property(TARGET cs_app_tests PROPERTY TEST_LAUNCHER
    "${CMAKE_COMMAND};-E;env;QT_QUICK_CONTROLS_STYLE=Basic;--modify;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;--"
)

gtest_discover_tests(cs_app_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

add_executable(cs_r1_durable_edit_acceptance_tests
    acceptance/R1DurableEditAcceptanceTest.cpp
)
target_link_libraries(cs_r1_durable_edit_acceptance_tests PRIVATE
    cs_app
    cs_fakes
    Qt6::Quick
    Qt6::Test
    GTest::gtest
)
cs_apply_warnings(cs_r1_durable_edit_acceptance_tests)
set_property(TARGET cs_r1_durable_edit_acceptance_tests PROPERTY TEST_LAUNCHER
    "${CMAKE_COMMAND};-E;env;--modify;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;--"
)
gtest_discover_tests(cs_r1_durable_edit_acceptance_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)

if(WIN32)
    add_test(
        NAME MltBootstrapPolicy
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/MltBootstrapPolicyTest.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
endif()

if(CS_ENABLE_FFMPEG AND CS_ENABLE_MLT)
    add_executable(cs_r1_studio_workflow_acceptance_tests
        acceptance/R1StudioWorkflowAcceptanceTest.cpp
    )
    target_link_libraries(cs_r1_studio_workflow_acceptance_tests PRIVATE
        cs_app
        cs_ffmpeg_adapter
        cs_mlt_adapter
        Qt6::Quick
        Qt6::Test
        GTest::gtest
    )
    target_compile_definitions(cs_r1_studio_workflow_acceptance_tests PRIVATE
        CS_TEST_STAGED_MLT_ROOT="$<TARGET_FILE_DIR:creator_studio>/mlt-runtime"
    )
    target_include_directories(cs_r1_studio_workflow_acceptance_tests SYSTEM PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
    )
    if(WIN32 AND MSVC)
        target_link_libraries(cs_r1_studio_workflow_acceptance_tests PRIVATE
            delayimp
            psapi
        )
        target_link_options(cs_r1_studio_workflow_acceptance_tests PRIVATE
            "/DELAYLOAD:mlt++-7.dll"
            "/DELAYLOAD:mlt-7.dll"
        )
    endif()
    add_dependencies(cs_r1_studio_workflow_acceptance_tests creator_studio)
    cs_apply_warnings(cs_r1_studio_workflow_acceptance_tests)
    set_property(TARGET cs_r1_studio_workflow_acceptance_tests PROPERTY
        TEST_LAUNCHER
        "${CMAKE_COMMAND};-E;env;--modify;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;--"
    )
    gtest_discover_tests(cs_r1_studio_workflow_acceptance_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            TIMEOUT 60
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
endif()

if(CS_ENABLE_FFMPEG)
    if(WIN32 AND CS_ENABLE_MLT)
        add_executable(cs_r1_07_physical_acceptance_tests
            acceptance/R1FinalPhysicalAcceptanceTest.cpp
        )
        target_link_libraries(cs_r1_07_physical_acceptance_tests PRIVATE
            cs_app
            cs_ffmpeg_adapter
            cs_mlt_adapter
            Qt6::Quick
            Qt6::Test
            GTest::gtest
            delayimp
            psapi
            winmm
        )
        target_compile_definitions(cs_r1_07_physical_acceptance_tests PRIVATE
            CS_TEST_STAGED_MLT_ROOT="$<TARGET_FILE_DIR:creator_studio>/mlt-runtime"
        )
        target_include_directories(cs_r1_07_physical_acceptance_tests SYSTEM PRIVATE
            ${FFMPEG_INCLUDE_DIRS}
        )
        target_link_libraries(cs_r1_07_physical_acceptance_tests PRIVATE
            ${FFMPEG_LIBRARIES}
        )
        target_link_options(cs_r1_07_physical_acceptance_tests PRIVATE
            "/DELAYLOAD:mlt++-7.dll"
            "/DELAYLOAD:mlt-7.dll"
        )
        add_dependencies(cs_r1_07_physical_acceptance_tests creator_studio)
        cs_apply_warnings(cs_r1_07_physical_acceptance_tests)
        set_property(TARGET cs_r1_07_physical_acceptance_tests PROPERTY
            TEST_LAUNCHER
            "${CMAKE_COMMAND};-E;env;--modify;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;--"
        )
        gtest_discover_tests(cs_r1_07_physical_acceptance_tests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES
                TIMEOUT 7200
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                LABELS "physical;r1-07"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )

        add_executable(cs_r1_replay_stress_tests
            acceptance/R1ReplayStressTest.cpp
        )
        target_link_libraries(cs_r1_replay_stress_tests PRIVATE
            cs_app
            cs_ffmpeg_adapter
            cs_mlt_adapter
            Qt6::Quick
            Qt6::Test
            GTest::gtest
            delayimp
        )
        target_compile_definitions(cs_r1_replay_stress_tests PRIVATE
            CS_TEST_STAGED_MLT_ROOT="$<TARGET_FILE_DIR:creator_studio>/mlt-runtime"
        )
        target_include_directories(cs_r1_replay_stress_tests SYSTEM PRIVATE
            ${FFMPEG_INCLUDE_DIRS}
        )
        target_link_libraries(cs_r1_replay_stress_tests PRIVATE
            ${FFMPEG_LIBRARIES}
        )
        target_link_options(cs_r1_replay_stress_tests PRIVATE
            "/DELAYLOAD:mlt++-7.dll"
            "/DELAYLOAD:mlt-7.dll"
        )
        add_dependencies(cs_r1_replay_stress_tests creator_studio)
        cs_apply_warnings(cs_r1_replay_stress_tests)
        set_property(TARGET cs_r1_replay_stress_tests PROPERTY
            TEST_LAUNCHER
            "${CMAKE_COMMAND};-E;env;--modify;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;--"
        )
        gtest_discover_tests(cs_r1_replay_stress_tests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES
                TIMEOUT 1800
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
                LABELS "replay;r1"
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    add_executable(cs_ffmpeg_tests
        ffmpeg_adapter/WindowsCaptureBackendTest.cpp
        ffmpeg_adapter/FfmpegCapabilityProbeTest.cpp
        ffmpeg_adapter/FfmpegMediaProbeTest.cpp
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
        target_link_libraries(cs_ffmpeg_tests PRIVATE winmm)
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

if(CS_ENABLE_MLT)
    add_executable(cs_mlt_tests
        app/ProjectExportEngineIntegrationTest.cpp
        mlt_adapter/ExportEncoderProbeTest.cpp
        mlt_adapter/MltEditEngineTest.cpp
        mlt_adapter/MltRenderJobTest.cpp
        mlt_adapter/MltRuntimeManifestTest.cpp
    )
    target_link_libraries(cs_mlt_tests PRIVATE
        cs_app
        cs_project_store
        cs_ffmpeg_adapter
        cs_mlt_adapter
        GTest::gtest_main
    )
    target_compile_definitions(cs_mlt_tests PRIVATE
        CS_TEST_MLT_ROOT="${CS_MLT_ROOT}"
    )
    cs_apply_warnings(cs_mlt_tests)
    set_property(TARGET cs_mlt_tests PROPERTY TEST_LAUNCHER
        "${CMAKE_COMMAND};-E;env;--modify;PATH=path_list_prepend:${CS_MLT_ROOT}/bin;--"
    )
    gtest_discover_tests(cs_mlt_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${CS_MLT_ROOT}/bin"
    )

    add_executable(cs_r1_mlt_acceptance_tests
        acceptance/R1MltPreviewAcceptanceTest.cpp
    )
    target_link_libraries(cs_r1_mlt_acceptance_tests PRIVATE
        cs_app
        cs_mlt_adapter
        Qt6::Quick
        Qt6::Test
        GTest::gtest
    )
    target_compile_definitions(cs_r1_mlt_acceptance_tests PRIVATE
        CS_TEST_STAGED_MLT_ROOT="$<TARGET_FILE_DIR:creator_studio>/mlt-runtime"
    )
    if(WIN32 AND MSVC)
        target_link_libraries(cs_r1_mlt_acceptance_tests PRIVATE delayimp)
        target_link_options(cs_r1_mlt_acceptance_tests PRIVATE
            "/DELAYLOAD:mlt++-7.dll"
            "/DELAYLOAD:mlt-7.dll"
        )
    endif()
    add_dependencies(cs_r1_mlt_acceptance_tests creator_studio)
    cs_apply_warnings(cs_r1_mlt_acceptance_tests)
    set_property(TARGET cs_r1_mlt_acceptance_tests PROPERTY TEST_LAUNCHER
        "${CMAKE_COMMAND};-E;env;--modify;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;--"
    )
    gtest_discover_tests(cs_r1_mlt_acceptance_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )

    add_executable(cs_r1_effects_text_acceptance_tests
        acceptance/R1EffectsTextAcceptanceTest.cpp
    )
    target_link_libraries(cs_r1_effects_text_acceptance_tests PRIVATE
        cs_app
        cs_fakes
        cs_mlt_adapter
        Qt6::Quick
        Qt6::Test
        GTest::gtest
    )
    target_compile_definitions(cs_r1_effects_text_acceptance_tests PRIVATE
        CS_TEST_STAGED_MLT_ROOT="$<TARGET_FILE_DIR:creator_studio>/mlt-runtime"
    )
    if(WIN32 AND MSVC)
        target_link_libraries(cs_r1_effects_text_acceptance_tests PRIVATE
            delayimp
            psapi
        )
        target_link_options(cs_r1_effects_text_acceptance_tests PRIVATE
            "/DELAYLOAD:mlt++-7.dll"
            "/DELAYLOAD:mlt-7.dll"
        )
    endif()
    add_dependencies(cs_r1_effects_text_acceptance_tests creator_studio)
    cs_apply_warnings(cs_r1_effects_text_acceptance_tests)
    set_property(TARGET cs_r1_effects_text_acceptance_tests PROPERTY TEST_LAUNCHER
        "${CMAKE_COMMAND};-E;env;--modify;PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;--"
    )
    gtest_discover_tests(cs_r1_effects_text_acceptance_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            TIMEOUT 45
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
    )
endif()
