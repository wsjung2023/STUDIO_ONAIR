cs_add_qtfree_library(cs_avatar_pack_adapter
    SOURCES
        AvatarPackArchive.h
        AvatarPackArchive.cpp
        AvatarPackStaging.h
        AvatarPackStaging.cpp
        SodiumSignatureVerifier.h
        SodiumSignatureVerifier.cpp
        AvatarPackValidator.h
        AvatarPackValidator.cpp
    DEPENDS
        cs_core
        cs_avatar
        miniz
        Sodium::Sodium
)

set_target_properties(cs_avatar_pack_adapter PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
