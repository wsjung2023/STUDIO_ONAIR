cmake_minimum_required(VERSION 3.25)

project(CreatorStudio VERSION 0.1.0 LANGUAGES CXX)

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
