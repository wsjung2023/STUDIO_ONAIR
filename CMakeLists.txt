cmake_minimum_required(VERSION 3.25)

project(CreatorStudio VERSION 0.1.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CS_BUILD_TESTS "Build unit tests" ON)
option(CS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CS_ENABLE_FFMPEG "Build the audited FFmpeg adapter" OFF)
option(CS_ENABLE_MLT "Build the audited MLT edit-engine adapter" OFF)
set(CS_FFMPEG_ROOT "" CACHE PATH "Audited FFmpeg dynamic-library prefix")
set(CS_MLT_ROOT "" CACHE PATH "Audited MLT dynamic-library prefix")

if(CS_ENABLE_MLT AND NOT CS_ENABLE_FFMPEG)
    message(FATAL_ERROR
        "CS_ENABLE_MLT requires CS_ENABLE_FFMPEG for audited export codecs")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CreatorStudioTargets)

if(CS_ENABLE_FFMPEG)
    if(NOT CS_FFMPEG_ROOT)
        message(FATAL_ERROR "CS_ENABLE_FFMPEG requires CS_FFMPEG_ROOT from scripts/bootstrap_ffmpeg.ps1")
    endif()
    list(PREPEND CMAKE_MODULE_PATH "${CS_FFMPEG_ROOT}/share/ffmpeg")
    find_package(FFMPEG 8.1.2 REQUIRED)
endif()

if(CS_ENABLE_MLT)
    if(NOT CS_MLT_ROOT)
        message(FATAL_ERROR "CS_ENABLE_MLT requires CS_MLT_ROOT from scripts/bootstrap_mlt.ps1")
    endif()
    execute_process(
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_mlt_runtime.ps1"
            -RuntimeRoot "${CS_MLT_ROOT}"
        RESULT_VARIABLE CS_MLT_VERIFY_RESULT
    )
    if(NOT CS_MLT_VERIFY_RESULT EQUAL 0)
        message(FATAL_ERROR "CS_MLT_ROOT failed the audited runtime verification")
    endif()
    find_package(MLT REQUIRED)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Quick QuickControls2 Test)
qt_standard_project_setup(REQUIRES 6.8)

include(FetchContent)

# nlohmann/json is MIT, which is in the LICENSE_POLICY 허용 기본군. It is used
# instead of QJsonDocument because cs_project_store must stay Qt-free: pulling
# Qt in here would defeat the module split this whole task is built on.
# Recorded in legal/OSS_BOM.csv. The tag is pinned so the BOM keeps describing
# what we actually build.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json_schema_validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG        2.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json_schema_validator)

FetchContent_Declare(
    sqlite_amalgamation
    URL      https://sqlite.org/2026/sqlite-amalgamation-3530300.zip
    URL_HASH SHA3_256=d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9
)
FetchContent_MakeAvailable(sqlite_amalgamation)

add_library(cs_sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(cs_sqlite3 PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_compile_definitions(cs_sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_API_ARMOR=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_THREADSAFE=1
)
set_target_properties(cs_sqlite3 PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/schemas/project.schema.json"
)
file(READ "${PROJECT_SOURCE_DIR}/schemas/project.schema.json" CS_PROJECT_SCHEMA_JSON)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/ProjectSchema.h.in"
    "${PROJECT_BINARY_DIR}/generated/project_store/ProjectSchema.h"
    @ONLY
)

set(CS_MIGRATION_001 "${PROJECT_SOURCE_DIR}/src/project_store/migrations/001_initial.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CS_MIGRATION_001}")
file(READ "${CS_MIGRATION_001}" CS_MIGRATION_001_SQL)
file(SHA256 "${CS_MIGRATION_001}" CS_MIGRATION_001_SHA256)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/Migration001.h.in"
    "${PROJECT_BINARY_DIR}/generated/project_store/Migration001.h"
    @ONLY
)

set(CS_MIGRATION_002
    "${PROJECT_SOURCE_DIR}/src/project_store/migrations/002_timeline_editing.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CS_MIGRATION_002}")
file(READ "${CS_MIGRATION_002}" CS_MIGRATION_002_SQL)
file(SHA256 "${CS_MIGRATION_002}" CS_MIGRATION_002_SHA256)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/Migration002.h.in"
    "${PROJECT_BINARY_DIR}/generated/project_store/Migration002.h"
    @ONLY
)

set(CS_MIGRATION_003
    "${PROJECT_SOURCE_DIR}/src/project_store/migrations/003_studio_workflow.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CS_MIGRATION_003}")
file(READ "${CS_MIGRATION_003}" CS_MIGRATION_003_SQL)
file(SHA256 "${CS_MIGRATION_003}" CS_MIGRATION_003_SHA256)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/Migration003.h.in"
    "${PROJECT_BINARY_DIR}/generated/project_store/Migration003.h"
    @ONLY
)

set(CS_MIGRATION_004
    "${PROJECT_SOURCE_DIR}/src/project_store/migrations/004_render_jobs.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CS_MIGRATION_004}")
file(READ "${CS_MIGRATION_004}" CS_MIGRATION_004_SQL)
file(SHA256 "${CS_MIGRATION_004}" CS_MIGRATION_004_SHA256)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/Migration004.h.in"
    "${PROJECT_BINARY_DIR}/generated/project_store/Migration004.h"
    @ONLY
)

# Layers below the application boundary. None of these link Qt.
add_subdirectory(src/core)
add_subdirectory(src/domain)
add_subdirectory(src/media)
add_subdirectory(src/edit_engine)
add_subdirectory(src/sync)
add_subdirectory(src/capture)
add_subdirectory(src/recorder)
if(CS_ENABLE_FFMPEG)
    add_subdirectory(src/ffmpeg_adapter)
endif()
add_subdirectory(src/mlt_adapter)
add_subdirectory(src/project_store)
add_subdirectory(src/transcription)
add_subdirectory(src/fakes)

# The application boundary: cs_app is the first, and only, library allowed to
# link Qt (this file's own comment above is the enforcement for everything
# above this line).
add_subdirectory(src/app)

qt_add_executable(creator_studio
    src/main.cpp
)

# Qt 6.8.3 generates unreachable C++ for EditorPage's intentionally dynamic
# controller boundary, which fails the MSVC /WX gate inside Qt headers. Isolate
# that single page as source/JIT while preserving native AOT compilation for
# every other QML file in the shipping module.
set_source_files_properties(qml/EditorPage.qml PROPERTIES
    QT_QML_SKIP_CACHEGEN TRUE
)

qt_add_qml_module(creator_studio
    URI CreatorStudio
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/HomePage.qml
        qml/RecoveryPage.qml
        qml/StudioPage.qml
        qml/EditorPage.qml
        qml/ExportPage.qml
        qml/TestPattern.qml
)

target_link_libraries(creator_studio
    PRIVATE
        cs_app
        Qt6::Quick
        Qt6::QuickControls2
)

if(CS_ENABLE_FFMPEG)
    target_link_libraries(creator_studio PRIVATE cs_ffmpeg_adapter)
    target_compile_definitions(creator_studio PRIVATE CS_APP_ENABLE_FFMPEG=1)
    if(WIN32)
        file(GLOB CS_APP_FFMPEG_RUNTIME_DLLS CONFIGURE_DEPENDS
            "${CS_FFMPEG_ROOT}/bin/*.dll")
        add_custom_command(TARGET creator_studio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CS_APP_FFMPEG_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:creator_studio>
            COMMAND_EXPAND_LISTS
        )
    elseif(APPLE)
        set_property(TARGET creator_studio APPEND PROPERTY
            BUILD_RPATH "${CS_FFMPEG_ROOT}/lib"
        )
    endif()
endif()

if(CS_ENABLE_MLT)
    target_link_libraries(creator_studio PRIVATE cs_mlt_adapter)
    target_compile_definitions(creator_studio PRIVATE
        CS_APP_ENABLE_MLT=1
    )
    if(WIN32)
        if(NOT MSVC)
            message(FATAL_ERROR "The audited Windows MLT package requires MSVC")
        endif()
        target_link_libraries(creator_studio PRIVATE delayimp)
        target_link_options(creator_studio PRIVATE
            "/DELAYLOAD:mlt++-7.dll"
            "/DELAYLOAD:mlt-7.dll"
        )
        add_custom_command(TARGET creator_studio POST_BUILD
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/stage_mlt_runtime.ps1"
                -SourceRoot "${CS_MLT_ROOT}"
                -DestinationRoot "$<TARGET_FILE_DIR:creator_studio>/mlt-runtime"
        )
    else()
        message(FATAL_ERROR
            "Audited MLT application staging is currently implemented for Windows")
    endif()
endif()

if(APPLE)
    target_link_libraries(creator_studio PRIVATE cs_capture_macos)
    set_target_properties(creator_studio PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/cmake/CreatorStudioInfo.plist.in"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${PROJECT_SOURCE_DIR}/cmake/CreatorStudio.entitlements"
    )
endif()

cs_apply_warnings(creator_studio)

install(TARGETS creator_studio
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(CS_ENABLE_MLT)
    install(DIRECTORY "$<TARGET_FILE_DIR:creator_studio>/mlt-runtime/"
        DESTINATION bin/mlt-runtime
    )
endif()

if(CS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
