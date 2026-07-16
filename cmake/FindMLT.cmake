find_path(MLT_INCLUDE_DIR
    NAMES framework/mlt.h
    PATHS "${CS_MLT_ROOT}/include/mlt-7"
    NO_DEFAULT_PATH
)

find_library(MLT_FRAMEWORK_LIBRARY
    NAMES mlt-7
    PATHS "${CS_MLT_ROOT}/lib"
    NO_DEFAULT_PATH
)

find_library(MLT_PLUSPLUS_LIBRARY
    NAMES mlt++-7
    PATHS "${CS_MLT_ROOT}/lib"
    NO_DEFAULT_PATH
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MLT
    REQUIRED_VARS MLT_INCLUDE_DIR MLT_FRAMEWORK_LIBRARY MLT_PLUSPLUS_LIBRARY
    VERSION_VAR MLT_VERSION
)

if(MLT_FOUND AND NOT TARGET MLT::Framework)
    add_library(MLT::Framework SHARED IMPORTED)
    set_target_properties(MLT::Framework PROPERTIES
        IMPORTED_IMPLIB "${MLT_FRAMEWORK_LIBRARY}"
        IMPORTED_LOCATION "${CS_MLT_ROOT}/bin/mlt-7.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${MLT_INCLUDE_DIR};${CS_MLT_ROOT}/include/mlt-deps"
    )

    add_library(MLT::PlusPlus SHARED IMPORTED)
    set_target_properties(MLT::PlusPlus PROPERTIES
        IMPORTED_IMPLIB "${MLT_PLUSPLUS_LIBRARY}"
        IMPORTED_LOCATION "${CS_MLT_ROOT}/bin/mlt++-7.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${MLT_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES MLT::Framework
    )
endif()

mark_as_advanced(MLT_INCLUDE_DIR MLT_FRAMEWORK_LIBRARY MLT_PLUSPLUS_LIBRARY)
