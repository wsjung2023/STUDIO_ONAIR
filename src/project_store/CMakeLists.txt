cs_add_qtfree_library(cs_project_store
    SOURCES
        IProjectStore.h
        JsonProjectStore.h
        JsonProjectStore.cpp
        ManifestSchemaValidator.h
        ManifestSchemaValidator.cpp
        internal/SqliteConnection.h
        internal/SqliteConnection.cpp
    DEPENDS
        cs_domain
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
)

target_include_directories(cs_project_store PRIVATE ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(cs_project_store PRIVATE cs_sqlite3)
