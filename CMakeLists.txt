cmake_minimum_required(VERSION 3.25)

project(CreatorStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(creator_studio
    src/main.cpp
    src/core/Timebase.h
    src/domain/Identifiers.h
    src/capture/ICaptureSource.h
    src/media/MediaTypes.h
)

qt_add_qml_module(creator_studio
    URI CreatorStudio
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(creator_studio
    PRIVATE
        Qt6::Quick
        Qt6::QuickControls2
)

if(MSVC)
    target_compile_options(creator_studio PRIVATE /W4 /permissive-)
else()
    target_compile_options(creator_studio PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS creator_studio
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
