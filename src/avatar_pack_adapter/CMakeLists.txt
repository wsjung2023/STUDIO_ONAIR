cs_add_qtfree_library(cs_avatar_pack_adapter
    SOURCES
        AvatarPackArchive.h
        AvatarPackArchive.cpp
        AvatarPackPromotionInternal.h
        AvatarPackStaging.h
        AvatarPackStaging.cpp
        SodiumSignatureVerifier.h
        SodiumSignatureVerifier.cpp
        AvatarPackValidator.h
        AvatarPackValidator.cpp
        FileAvatarCatalog.h
        FileAvatarCatalogInternal.h
        FileAvatarCatalog.cpp
    DEPENDS
        cs_core
        cs_avatar
        miniz
        Sodium::Sodium
)

if(WIN32)
    target_link_libraries(cs_avatar_pack_adapter PRIVATE advapi32)
endif()

set_target_properties(cs_avatar_pack_adapter PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
