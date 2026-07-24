if(NOT WIN32)
    message(FATAL_ERROR "The audited libsodium 1.0.22 package is the MSVC build")
endif()
if(TARGET Sodium::Sodium)
    message(FATAL_ERROR "Audited libsodium forbids a pre-existing Sodium::Sodium target")
endif()

# Command-line and stale cache entries must never redirect the audited target.
get_cmake_property(_cs_sodium_cache_variables CACHE_VARIABLES)
foreach(_cs_sodium_variable IN LISTS _cs_sodium_cache_variables)
    if(_cs_sodium_variable MATCHES "^Sodium_")
        unset("${_cs_sodium_variable}" CACHE)
    endif()
endforeach()
foreach(_cs_sodium_variable IN ITEMS
    Sodium_DIR
    Sodium_FOUND
    Sodium_INCLUDE_DIR
    Sodium_LIBRARY
    Sodium_RUNTIME_LIBRARY
    Sodium_ROOT
    Sodium_VERSION
)
    unset("${_cs_sodium_variable}")
endforeach()

cmake_path(ABSOLUTE_PATH CS_SODIUM_ROOT NORMALIZE
    OUTPUT_VARIABLE _cs_sodium_requested_root)
if(NOT IS_DIRECTORY "${_cs_sodium_requested_root}")
    message(FATAL_ERROR "CS_SODIUM_ROOT does not exist: ${_cs_sodium_requested_root}")
endif()
file(REAL_PATH "${_cs_sodium_requested_root}" _cs_sodium_root)

set(_cs_sodium_include_requested "${_cs_sodium_root}/include")
set(_cs_sodium_library_requested "${_cs_sodium_root}/lib/libsodium.lib")
set(_cs_sodium_runtime_requested "${_cs_sodium_root}/bin/libsodium.dll")
foreach(_cs_sodium_artifact IN ITEMS
    _cs_sodium_include_requested
    _cs_sodium_library_requested
    _cs_sodium_runtime_requested
)
    if(NOT EXISTS "${${_cs_sodium_artifact}}")
        message(FATAL_ERROR
            "Audited libsodium artifact is missing: ${${_cs_sodium_artifact}}")
    endif()
endforeach()
if(NOT EXISTS "${_cs_sodium_include_requested}/sodium.h")
    message(FATAL_ERROR
        "Audited libsodium public header is missing: "
        "${_cs_sodium_include_requested}/sodium.h")
endif()

file(REAL_PATH "${_cs_sodium_include_requested}" _cs_sodium_include)
file(REAL_PATH "${_cs_sodium_library_requested}" _cs_sodium_library)
file(REAL_PATH "${_cs_sodium_runtime_requested}" _cs_sodium_runtime)
set(_cs_sodium_root_for_prefix "${_cs_sodium_root}")
foreach(_cs_sodium_artifact IN ITEMS
    _cs_sodium_include
    _cs_sodium_library
    _cs_sodium_runtime
)
    cmake_path(IS_PREFIX _cs_sodium_root_for_prefix
        "${${_cs_sodium_artifact}}" NORMALIZE _cs_sodium_is_contained)
    if(NOT _cs_sodium_is_contained)
        message(FATAL_ERROR
            "Audited libsodium artifact escapes CS_SODIUM_ROOT: "
            "${${_cs_sodium_artifact}}")
    endif()
endforeach()

set(Sodium_FOUND TRUE)
set(Sodium_VERSION "1.0.22")
add_library(Sodium::Sodium SHARED IMPORTED)
set_target_properties(Sodium::Sodium PROPERTIES
    IMPORTED_IMPLIB "${_cs_sodium_library}"
    IMPORTED_LOCATION "${_cs_sodium_runtime}"
    INTERFACE_INCLUDE_DIRECTORIES "${_cs_sodium_include}"
)
get_target_property(
    _cs_sodium_target_include
    Sodium::Sodium
    INTERFACE_INCLUDE_DIRECTORIES
)
get_target_property(
    _cs_sodium_target_library
    Sodium::Sodium
    IMPORTED_IMPLIB
)
get_target_property(
    _cs_sodium_target_runtime
    Sodium::Sodium
    IMPORTED_LOCATION
)

message(STATUS "Found Sodium: ${_cs_sodium_include} (found version \"${Sodium_VERSION}\")")
message(STATUS "Sodium::Sodium include directory: ${_cs_sodium_target_include}")
message(STATUS "Sodium::Sodium import library: ${_cs_sodium_target_library}")
message(STATUS "Sodium::Sodium runtime library: ${_cs_sodium_target_runtime}")
