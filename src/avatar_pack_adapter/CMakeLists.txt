set(_cs_avatar_pack_anchor
    "${CMAKE_CURRENT_BINARY_DIR}/cs_avatar_pack_adapter_anchor.cpp"
)
file(GENERATE
    OUTPUT "${_cs_avatar_pack_anchor}"
    CONTENT
        "// Generated build anchor: package behavior is added in later tasks.\n"
)
set_source_files_properties("${_cs_avatar_pack_anchor}" PROPERTIES GENERATED TRUE)

cs_add_qtfree_library(cs_avatar_pack_adapter
    SOURCES
        "${_cs_avatar_pack_anchor}"
    DEPENDS
        miniz
        Sodium::Sodium
)
