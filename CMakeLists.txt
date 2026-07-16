cmake_minimum_required(VERSION 3.25)

project(CreatorStudio VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CS_BUILD_TESTS "Build unit tests" ON)
option(CS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CreatorStudioTargets)

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

# Layers below the application boundary. None of these link Qt.
add_subdirectory(src/core)
add_subdirectory(src/domain)
add_subdirectory(src/media)
add_subdirectory(src/capture)
add_subdirectory(src/recorder)
add_subdirectory(src/project_store)
add_subdirectory(src/fakes)

# The application boundary: cs_app is the first, and only, library allowed to
# link Qt (this file's own comment above is the enforcement for everything
# above this line).
add_subdirectory(src/app)

qt_add_executable(creator_studio
    src/main.cpp
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
        qml/TestPattern.qml
)

target_link_libraries(creator_studio
    PRIVATE
        cs_app
        Qt6::Quick
        Qt6::QuickControls2
)

cs_apply_warnings(creator_studio)

install(TARGETS creator_studio
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(CS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
