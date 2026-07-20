# Helpers that make the architecture boundaries in ARCHITECTURE.md 14 and
# CLAUDE.md 3/5 enforceable at build time rather than at review time.

function(cs_apply_warnings target)
    if(MSVC)
        # MSVC otherwise interprets source files using the active system codepage,
        # so a non-ASCII comment (e.g. Korean text) without a byte-order mark trips
        # C4819, which /WX below escalates to a hard error. GCC and Clang already
        # default to UTF-8, so this option only needs to be set for MSVC.
        # CMake's Ninja+MSVC toolchain does not guarantee /EHsc. Without it,
        # standard-library headers emit C4530, which becomes a hard failure
        # under the repository's /WX CI gate. Every C++ target may use RAII
        # around recoverable Result/I/O failures, so exception unwinding must
        # be configured by the project rather than inherited from one IDE.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
        if(CS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        # The workflow command layer intentionally uses C++20 designated
        # initializers to select a request's payload while every omitted
        # field keeps its explicit in-class default. Clang diagnoses that
        # standards-defined pattern as a warning, whereas MSVC does not.
        # Keep every other warning fatal; only this non-actionable diagnostic
        # is disabled so Android builds have the same semantics as Windows.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
        endif()
        if(CS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Fails configuration if `target`, or anything it depends on at any depth,
# links Qt directly.
#
# This walks the whole dependency subtree with a breadth-first search rather
# than checking only `target`'s own direct links. Checking direct links alone
# is NOT sufficient: cs_add_qtfree_library()'s DEPENDS just calls
# target_link_libraries(), which does not care whether the thing it points at
# is itself Qt-free, so a Qt-free target can legally DEPENDS on something that
# links Qt two, three, or more levels down, and a direct-only check would say
# nothing about it. Proven while fixing this: a scratch target linking
# Qt6::Core, hung two levels below cs_domain (cs_domain -> scratch target ->
# Qt6::Core), configured silently under the old direct-only check and let
# `#include <QString>` compile in a file that is supposed to be Qt-free.
#
# get_target_property(<t> LINK_LIBRARIES) reads empty for INTERFACE library
# targets - they have no private link step of their own - so their
# dependencies are read from INTERFACE_LINK_LIBRARIES instead. nlohmann_json's
# imported target is already INTERFACE, and cs_add_qtfree_library() below can
# itself produce an INTERFACE target for a header-only module, so both target
# kinds have to be handled walking the graph, not just at the root.
#
# Generator expressions (an entry starting with `$<`) and any entry that is
# not itself a CMake target (a bare .lib path, a system library name) are
# skipped rather than resolved further: this repository never routes a Qt
# link through either today, so skipping them costs nothing right now, but it
# is a real, if narrow, gap - a Qt dependency hidden behind a generator
# expression would not be caught. Recorded here rather than silently ignored.
#
# Also scheduled a second time via cmake_language(DEFER ...) from
# cs_add_qtfree_library(), against the top-level directory scope, so a Qt link
# added to anything in the graph AFTER this first run - e.g. a
# target_link_libraries() call from a subdirectory processed later in the same
# configure - is still caught before configure finishes, not just links that
# already existed at the moment this target was declared.
function(cs_assert_qt_free target)
    set(_queue "${target}")
    set(_visited "")

    while(_queue)
        list(POP_FRONT _queue _path)

        # Queue entries are whole "a -> b -> c" chains, not bare names, so the
        # error below can name the path. The node being visited is the last
        # segment; strip everything up to and including the final " -> ".
        string(REGEX REPLACE ".* -> " "" _current "${_path}")

        if(_current IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_current}")

        if(_current MATCHES "^Qt[0-9]*::")
            message(FATAL_ERROR
                "${target} depends on ${_current} (${_path}), but targets below the "
                "application layer must stay Qt-free. See ARCHITECTURE.md 14 and "
                "CLAUDE.md 5. If this target genuinely belongs in the application layer, "
                "use add_library() directly instead of cs_add_qtfree_library().")
        endif()

        if(_current MATCHES "^\\$<")
            continue()  # generator expression: not resolved further, see comment above
        endif()
        if(NOT TARGET ${_current})
            continue()  # not a CMake target we can inspect, see comment above
        endif()

        get_target_property(_type ${_current} TYPE)
        if(_type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_libs ${_current} INTERFACE_LINK_LIBRARIES)
        else()
            get_target_property(_libs ${_current} LINK_LIBRARIES)
        endif()

        if(_libs)
            foreach(_lib IN LISTS _libs)
                if(NOT _lib IN_LIST _visited)
                    list(APPEND _queue "${_path} -> ${_lib}")
                endif()
            endforeach()
        endif()
    endwhile()
endfunction()

# Declares a static library that is forbidden from touching Qt, FFmpeg or MLT.
# The enforcement is physical: no Qt target is linked, so Qt headers are not on
# the include path and `#include <QString>` fails to compile. cs_assert_qt_free
# backs that up at configure time for the case where a Qt link reaches this
# target indirectly, through a dependency, rather than through its own
# #include list.
function(cs_add_qtfree_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})

    add_library(${name} STATIC ${ARG_SOURCES})
    add_library(CreatorStudio::${name} ALIAS ${name})

    # A target whose SOURCES are all headers (cs_media, cs_capture and
    # cs_recorder today) has no translation unit of its own, and CMake cannot
    # infer a linker language with none to look at. That is silently papered
    # over today by AUTOMOC: qt_standard_project_setup() in the root
    # CMakeLists turns AUTOMOC on at directory scope, so every target declared
    # below it gets a generated mocs_compilation.cpp, header-only or not, and
    # that incidental file is what currently gives these targets a linker
    # language. Setting LINKER_LANGUAGE explicitly removes that hidden
    # dependency: these targets configure and link the same way whether or not
    # AUTOMOC ever runs against them. (It must still run today - do not turn it
    # off here; that is a separate change this one only makes possible.)
    set(_cs_has_cpp FALSE)
    foreach(_cs_src IN LISTS ARG_SOURCES)
        if(_cs_src MATCHES "\\.cpp$")
            set(_cs_has_cpp TRUE)
        endif()
    endforeach()
    if(NOT _cs_has_cpp)
        set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)
    endif()

    target_include_directories(${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_compile_features(${name} PUBLIC cxx_std_20)

    if(ARG_DEPENDS)
        target_link_libraries(${name} PUBLIC ${ARG_DEPENDS})
    endif()

    cs_apply_warnings(${name})
    cs_assert_qt_free(${name})
    # See cs_assert_qt_free's own comment: this second, deferred check is what
    # catches a Qt link added to the graph after this line has already run.
    #
    # Routed through EVAL CODE rather than
    # `cmake_language(DEFER ... CALL cs_assert_qt_free "${name}")` directly:
    # DEFER CALL's own arguments are re-resolved lazily, in whatever variable
    # scope is current at the moment the call finally fires - which by then is
    # long after this function has returned and its local `name` has gone out
    # of scope, so a direct "${name}" silently evaluates to an empty string
    # instead of this target's name (verified empirically: a plain
    # `DEFER CALL foo "${name}"` from inside a function calls foo() with no
    # argument at all). Building the call as a string first, with `name`
    # substituted into it right now via ordinary (eager) argument expansion,
    # and only then handing that already-resolved text to DEFER, bakes the
    # value in as a literal before the lazy re-resolution ever happens.
    cmake_language(EVAL CODE
        "cmake_language(DEFER DIRECTORY [[${PROJECT_SOURCE_DIR}]] CALL cs_assert_qt_free [[${name}]])")
endfunction()
