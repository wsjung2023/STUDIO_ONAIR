if(NOT WIN32)
    message(FATAL_ERROR "The audited libsodium 1.0.22 package is the MSVC build")
endif()

find_path(Sodium_INCLUDE_DIR
    NAMES sodium.h
    PATHS "${CS_SODIUM_ROOT}/include"
    NO_DEFAULT_PATH
)

find_library(Sodium_LIBRARY
    NAMES libsodium
    PATHS "${CS_SODIUM_ROOT}/lib"
    NO_DEFAULT_PATH
)

find_file(Sodium_RUNTIME_LIBRARY
    NAMES libsodium.dll
    PATHS "${CS_SODIUM_ROOT}/bin"
    NO_DEFAULT_PATH
)

set(Sodium_VERSION "1.0.22")
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Sodium
    REQUIRED_VARS
        Sodium_INCLUDE_DIR
        Sodium_LIBRARY
        Sodium_RUNTIME_LIBRARY
    VERSION_VAR Sodium_VERSION
)

if(Sodium_FOUND AND NOT TARGET Sodium::Sodium)
    add_library(Sodium::Sodium SHARED IMPORTED)
    set_target_properties(Sodium::Sodium PROPERTIES
        IMPORTED_IMPLIB "${Sodium_LIBRARY}"
        IMPORTED_LOCATION "${Sodium_RUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(
    Sodium_INCLUDE_DIR
    Sodium_LIBRARY
    Sodium_RUNTIME_LIBRARY
)
