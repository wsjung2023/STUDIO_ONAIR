cs_add_qtfree_library(cs_cursor
    SOURCES
        CursorPoint.h
        CursorButton.h
        CursorMoveEvent.h
        CursorClickEvent.h
        CursorNormalizer.h
        CursorNormalizer.cpp
        ICursorSource.h
        CursorEventSerializer.h
        CursorEventSerializer.cpp
        CursorNdjsonSink.h
        CursorNdjsonSink.cpp
    DEPENDS
        cs_core
        cs_domain
        nlohmann_json::nlohmann_json
)

if(WIN32)
    target_sources(cs_cursor PRIVATE
        windows/WindowsRawInputCursorSource.h
        windows/WindowsRawInputCursorSource.cpp
    )
    target_link_libraries(cs_cursor PRIVATE user32)
endif()
