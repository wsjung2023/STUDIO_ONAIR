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

add_executable(cs_platform_release_tests
    platform_release/PlatformCapabilityTest.cpp
    platform_release/ReleaseManifestTest.cpp
)
target_link_libraries(cs_platform_release_tests
    PRIVATE
        cs_core
        cs_platform_release
        nlohmann_json::nlohmann_json
        GTest::gtest_main
)
set_target_properties(cs_platform_release_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
cs_apply_warnings(cs_platform_release_tests)
gtest_discover_tests(cs_platform_release_tests)

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
    mlt_adapter/CursorVisualEffectsTest.cpp
    domain/ProjectManifestTest.cpp
    domain/RecordingSessionTest.cpp
    media/MediaTypesTest.cpp
    sync/ClockCoordinatorTest.cpp
    sync/VideoSyncPlannerTest.cpp
    sync/AudioRateCompensatorTest.cpp
    sync/SyncPipelineAcceptanceTest.cpp
    capture/PortContractTest.cpp
    capture/CaptureTimestampMapperTest.cpp
    capture/AndroidProjectionSessionTest.cpp
    capture/AndroidDeviceSessionTest.cpp
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
    capture/AvatarRenderCaptureSourceTest.cpp
    avatar/ExpressionParametersTest.cpp
    avatar/AvatarParameterMapperTest.cpp
    avatar/AvatarModelDescriptorTest.cpp
    avatar/AvatarRenderPipelineTest.cpp
    avatar/AvatarRenderFrameMediaTest.cpp
    avatar/AvatarSoftwareRasterizerTest.cpp
    avatar/AvatarSoftwareRendererTest.cpp
    avatar/TrackingPortContractTest.cpp
    avatar/CalibrationProfileTest.cpp
    avatar/CalibrationCaptureTest.cpp
    avatar/ExpressionNormalizerTest.cpp
    avatar/ExpressionSmootherTest.cpp
    avatar/AvatarMotionSerializerTest.cpp
    avatar/AvatarMotionNdjsonSinkTest.cpp
    avatar/FakeTrackingProviderTest.cpp
    avatar/PipelineTest.cpp
    avatar/PrimaryFaceSelectorTest.cpp
    avatar/openseeface/OpenSeeFaceParserTest.cpp
    avatar/OpenSeeFaceUdpSourceTest.cpp
    avatar/OpenSeeFaceProcessSupervisorTest.cpp
    avatar/AvatarMotionPipelineTest.cpp
    avatar/AvatarMotionPlaybackTest.cpp
    avatar/AvatarMotionPlaybackRendererTest.cpp
    avatar/inochi2d/Inochi2dModelRuntimeTest.cpp
    avatar/inochi2d/Inochi2dAvatarRendererTest.cpp
    avatar/AvatarTrackingSessionTest.cpp
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
        cs_avatar
        cs_project_store
        cs_edit_engine
        cs_mlt_graph_plan
        cs_fakes
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)

target_compile_definitions(cs_tests PRIVATE
    CS_CRASH_FIXTURE_PATH="$<TARGET_FILE:cs_crash_recovery_fixture>"
    # Read directly rather than embedded at compile time (unlike
    # cs_project_store's ProjectSchema.h.in): this schema only needs to be
    # loaded by this test binary, not shipped in a production library, so a
    # source-tree path is simpler than adding another configure_file() step.
    CS_EVENT_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/event.schema.json"
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
    cursor/CursorEventPumpTest.cpp
)

if(WIN32)
    target_sources(cs_cursor_tests PRIVATE
        cursor/WindowsRawInputCursorSourceTest.cpp
    )
endif()

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

# cs_cursor_emphasis (R2-02 click-emphasis + cursor hide/replace plan core) is
# Qt-free like cs_cursor, so its tests get their own Qt-free binary. The
# emphasis-plan schema path is injected so the serializer test validates against
# the single shipped schemas/emphasis_plan.schema.json, mirroring the cursor and
# auto-zoom pipeline tests.
add_executable(cs_cursor_emphasis_tests
    cursor_emphasis/EmphasisValueTest.cpp
    cursor_emphasis/EmphasisPlannerTest.cpp
    cursor_emphasis/EmphasisPlanSerializerTest.cpp
)

target_link_libraries(cs_cursor_emphasis_tests
    PRIVATE
        cs_core
        cs_domain
        cs_cursor
        cs_cursor_emphasis
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)

target_include_directories(cs_cursor_emphasis_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cursor_emphasis)

target_compile_definitions(cs_cursor_emphasis_tests PRIVATE
    CS_EMPHASIS_PLAN_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/emphasis_plan.schema.json"
)

set_target_properties(cs_cursor_emphasis_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

cs_apply_warnings(cs_cursor_emphasis_tests)

gtest_discover_tests(cs_cursor_emphasis_tests)

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
    app/AvatarFrameTrackingCoordinatorTest.cpp
    app/TranscriptionProviderFactoryTest.cpp
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

# R2-06 audio DSP core: a dedicated Qt-free test binary. Kept separate from
# cs_tests so it links only cs_audio_dsp (+ the synthetic-signal test support),
# proving the DSP module stands on its own without the rest of the tree.
add_executable(cs_audio_dsp_tests
    audio_dsp/support/SyntheticAudio.cpp
    audio_dsp/AudioFormatTest.cpp
    audio_dsp/AudioBufferTest.cpp
    audio_dsp/SyntheticAudioTest.cpp
    audio_dsp/GainProcessorTest.cpp
    audio_dsp/CompressorProcessorTest.cpp
    audio_dsp/LimiterProcessorTest.cpp
    audio_dsp/AudioProcessingChainTest.cpp
    audio_dsp/AudioCleanupChainTest.cpp
    audio_dsp/ExportLoudnessAnalysisTest.cpp
    audio_dsp/UnavailableDenoiseProcessorTest.cpp
    audio_dsp/LoudnessNormalizerTest.cpp
    audio_dsp/KWeightingFilterTest.cpp
    audio_dsp/LoudnessMeterTest.cpp
    audio_dsp/AudioLoudnessSerializerTest.cpp
    audio_dsp/AudioLoudnessNdjsonSinkTest.cpp
)
target_link_libraries(cs_audio_dsp_tests
    PRIVATE
        cs_audio_dsp
        # Test-only: the loudness telemetry tests validate serialized events
        # against schemas/event.schema.json. The validator (MIT, already
        # vendored, no Qt) is linked into the TEST binary only — cs_audio_dsp
        # itself stays free of it, keeping the shipped DSP module lean.
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)
target_include_directories(cs_audio_dsp_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/tests/audio_dsp
)
target_compile_definitions(cs_audio_dsp_tests PRIVATE
    CS_EVENT_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/event.schema.json"
)
set_target_properties(cs_audio_dsp_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
cs_apply_warnings(cs_audio_dsp_tests)
gtest_discover_tests(cs_audio_dsp_tests)

# Audited RNNoise denoise adapter tests: enabled-only, mirroring cs_mlt_tests.
# The manifest test is self-contained; the real-denoise test drives the linked
# audited library from CS_RNNOISE_ROOT. Not part of the default (OFF) build.
if(CS_ENABLE_RNNOISE)
    add_executable(cs_rnnoise_tests
        rnnoise_adapter/RnnoiseRuntimeManifestTest.cpp
        rnnoise_adapter/RnnoiseDenoiseProcessorTest.cpp
    )
    target_link_libraries(cs_rnnoise_tests
        PRIVATE
            cs_rnnoise_adapter
            cs_audio_dsp
            GTest::gtest_main
    )
    target_compile_definitions(cs_rnnoise_tests PRIVATE
        CS_TEST_RNNOISE_ROOT="${CS_RNNOISE_ROOT}"
    )
    set_target_properties(cs_rnnoise_tests PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
    )
    cs_apply_warnings(cs_rnnoise_tests)
    gtest_discover_tests(cs_rnnoise_tests)
endif()

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

# R2-04 transcription core. Qt-free: this binary links no Qt, so it doubles as a
# standing check that the transcription module builds without Qt on the include
# path. json-schema-validator is linked test-only (the shipped module does not
# depend on it); the committed schema path is passed as a compile definition so
# the pipeline test validates the serialized document against it, mirroring the
# project_store manifest tests.
add_executable(cs_transcription_tests
    transcription/TranscriptValueObjectsTest.cpp
    transcription/FakeTranscriptionProviderTest.cpp
    transcription/TranscriptPipelineTest.cpp
    transcription/TranscriptStoreTest.cpp
    transcription/UnavailableTranscriptionProviderTest.cpp
)
target_link_libraries(cs_transcription_tests
    PRIVATE
        cs_core
        cs_domain
        cs_transcription
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)
target_compile_definitions(cs_transcription_tests PRIVATE
    CS_TRANSCRIPT_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/transcript.schema.json"
)
set_target_properties(cs_transcription_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
cs_apply_warnings(cs_transcription_tests)
gtest_discover_tests(cs_transcription_tests)

# R2-07 creator-intelligence acceptance test. Qt-free and gate-OFF by design: it
# assembles the R2 intelligence surface (transcription, cursor -> autozoom /
# emphasis, audio cleanup chain + export loudness analysis) with only the FAKE
# providers, so it needs no real MLT/whisper/rnnoise and stands in the default
# build. The committed schema paths are passed as compile definitions so the
# serialized artifacts are validated against the exact files the product ships
# (mirroring the transcription / audio_dsp schema tests).
add_executable(cs_r2_creator_intelligence_acceptance_tests
    acceptance/R2CreatorIntelligenceAcceptanceTest.cpp
)
target_link_libraries(cs_r2_creator_intelligence_acceptance_tests
    PRIVATE
        cs_core
        cs_domain
        cs_audio_dsp
        cs_transcription
        cs_cursor
        cs_autozoom
        cs_cursor_emphasis
        cs_fakes
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)
target_compile_definitions(cs_r2_creator_intelligence_acceptance_tests PRIVATE
    CS_TRANSCRIPT_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/transcript.schema.json"
    CS_ZOOM_CANDIDATE_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/zoom_candidate.schema.json"
    CS_EMPHASIS_PLAN_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/emphasis_plan.schema.json"
)
set_target_properties(cs_r2_creator_intelligence_acceptance_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_include_directories(cs_r2_creator_intelligence_acceptance_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests
)
cs_apply_warnings(cs_r2_creator_intelligence_acceptance_tests)
gtest_discover_tests(cs_r2_creator_intelligence_acceptance_tests)

# R2-05 cut-suggestion core. Qt-free like cs_autozoom/cs_transcription, so its
# tests get their own Qt-free binary that links no Qt: a standing check that the
# module builds below the application boundary. json-schema-validator is linked
# test-only; the committed cut_suggestion schema path is injected so the
# serializer test validates against the single shipped schema.
add_executable(cs_cut_suggest_tests
    cut_suggest/CutValueTest.cpp
    cut_suggest/SilenceDetectorTest.cpp
    cut_suggest/FillerDetectorTest.cpp
    cut_suggest/CutSuggestionAnalyzerTest.cpp
    cut_suggest/CutSuggestionSerializerTest.cpp
)
target_link_libraries(cs_cut_suggest_tests
    PRIVATE
        cs_core
        cs_domain
        cs_audio_dsp
        cs_transcription
        cs_cut_suggest
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
        GTest::gtest_main
)
target_include_directories(cs_cut_suggest_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cut_suggest)
target_compile_definitions(cs_cut_suggest_tests PRIVATE
    CS_CUT_SUGGESTION_SCHEMA_PATH="${PROJECT_SOURCE_DIR}/schemas/cut_suggestion.schema.json"
)
set_target_properties(cs_cut_suggest_tests PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
cs_apply_warnings(cs_cut_suggest_tests)
gtest_discover_tests(cs_cut_suggest_tests)

if(CS_ENABLE_WHISPER)
    add_executable(cs_whisper_tests
        whisper_adapter/WhisperRuntimeManifestTest.cpp
        whisper_adapter/WhisperTranscriptionProviderTest.cpp
    )
    target_link_libraries(cs_whisper_tests PRIVATE
        cs_core
        cs_domain
        cs_transcription
        cs_whisper_adapter
        GTest::gtest_main
    )
    target_compile_definitions(cs_whisper_tests PRIVATE
        CS_TEST_WHISPER_ROOT="${CS_WHISPER_ROOT}"
        CS_TEST_WHISPER_SAMPLE="${CS_WHISPER_ROOT}/share/whisper-samples/jfk.wav"
    )
    set_target_properties(cs_whisper_tests PROPERTIES
        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    cs_apply_warnings(cs_whisper_tests)
    if(WIN32)
        gtest_discover_tests(cs_whisper_tests
            DISCOVERY_MODE PRE_TEST
            PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${CS_WHISPER_ROOT}/bin"
        )
    else()
        gtest_discover_tests(cs_whisper_tests)
    endif()
endif()

if(WIN32)
    add_test(
        NAME AndroidProjectionBridgePolicy
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/AndroidProjectionBridgePolicyTest.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME AndroidProjectionFramePolicy
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/AndroidProjectionFramePolicyTest.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME AndroidProjectionAcceptance
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/AndroidProjectionAcceptance.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME AndroidDeviceCapturePolicy
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/AndroidDeviceCapturePolicyTest.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME AndroidManifestPolicy
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/AndroidManifestPolicyTest.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME ReleaseManifestScript
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/ReleaseManifestScriptTest.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME MltBootstrapPolicy
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/MltBootstrapPolicyTest.ps1"
            -RepositoryRoot "${PROJECT_SOURCE_DIR}"
    )
    add_test(
        NAME RnnoiseBootstrapPolicy
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/tests/scripts/RnnoiseBootstrapPolicyTest.ps1"
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
