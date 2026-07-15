cmake_minimum_required(VERSION 3.25)

project(CreatorStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CS_BUILD_TESTS "Build unit tests" ON)
option(CS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CreatorStudioTargets)

find_package(Qt6 6.8 REQUIRED COMPONENTS Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

# Layers below the application boundary. None of these link Qt.
add_subdirectory(src/core)
add_subdirectory(src/domain)
add_subdirectory(src/media)
add_subdirectory(src/capture)
add_subdirectory(src/recorder)
add_subdirectory(src/project_store)

if(CS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
