# Deliberately NOT cs_add_qtfree_library: this is the application layer, the
# first place Qt is allowed. Using plain add_library here is the visible marker
# of that boundary (ARCHITECTURE.md 14).
#
# AUTOMOC comes from qt_standard_project_setup() in the root CMakeLists, so
# Q_OBJECT works without extra wiring.
add_library(cs_app STATIC
    IRecordingPersistence.h
    StudioController.h
    StudioController.cpp
    ScreenCaptureController.h
    ScreenCaptureController.cpp
    RecentProjectRegistry.h
    RecentProjectRegistry.cpp
    ProjectWorker.h
    ProjectWorker.cpp
    ProjectController.h
    ProjectController.cpp
)
add_library(CreatorStudio::cs_app ALIAS cs_app)

target_include_directories(cs_app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(cs_app PUBLIC cxx_std_20)

target_link_libraries(cs_app
    PUBLIC
        Qt6::Core
        cs_capture
        cs_recorder
    PRIVATE
        # R0-03 deletes this line and the fakes leave the shipped binary.
        cs_fakes
        cs_project_store
)

cs_apply_warnings(cs_app)
