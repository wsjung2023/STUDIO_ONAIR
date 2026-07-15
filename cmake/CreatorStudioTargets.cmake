# Helpers that make the architecture boundaries in ARCHITECTURE.md 14 and
# CLAUDE.md 3/5 enforceable at build time rather than at review time.

function(cs_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(CS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(CS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Fails configuration if a target links Qt directly. Checking direct links is
# sufficient: every Qt-free target may only depend on other Qt-free targets, so
# if each one is clean then the whole subtree is clean transitively.
function(cs_assert_qt_free target)
    get_target_property(_libs ${target} LINK_LIBRARIES)
    if(_libs)
        foreach(_lib IN LISTS _libs)
            if(_lib MATCHES "^Qt[0-9]*::")
                message(FATAL_ERROR
                    "${target} links ${_lib}, but targets below the application layer must "
                    "stay Qt-free. See ARCHITECTURE.md 14 and CLAUDE.md 5. If this target "
                    "genuinely belongs in the application layer, use add_library() directly "
                    "instead of cs_add_qtfree_library().")
            endif()
        endforeach()
    endif()
endfunction()

# Declares a static library that is forbidden from touching Qt, FFmpeg or MLT.
# The enforcement is physical: no Qt target is linked, so Qt headers are not on
# the include path and `#include <QString>` fails to compile.
function(cs_add_qtfree_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})

    add_library(${name} STATIC ${ARG_SOURCES})
    add_library(CreatorStudio::${name} ALIAS ${name})

    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(${name} PUBLIC cxx_std_20)

    if(ARG_DEPENDS)
        target_link_libraries(${name} PUBLIC ${ARG_DEPENDS})
    endif()

    cs_apply_warnings(${name})
    cs_assert_qt_free(${name})
endfunction()
