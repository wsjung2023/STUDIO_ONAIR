if(NOT DEFINED RepositoryRoot)
    message(FATAL_ERROR "RepositoryRoot is required")
endif()

file(READ
    "${RepositoryRoot}/src/project_store/AvatarSpecFileStore.cpp"
    source
)

foreach(required_marker IN ITEMS
    "exactChildEntryMatches"
    "inspectPromotedTarget"
    "finalizeReadIdentity"
)
    string(FIND "${source}" "${required_marker}" marker_position)
    if(marker_position EQUAL -1)
        message(FATAL_ERROR
            "Avatar spec authority is missing ${required_marker}"
        )
    endif()
endforeach()
