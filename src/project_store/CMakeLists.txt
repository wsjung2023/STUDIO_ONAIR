cs_add_qtfree_library(cs_project_store
    SOURCES
        IProjectStore.h
        ITimelineStore.h
        IProjectPackageStore.h
        JsonProjectStore.h
        JsonProjectStore.cpp
        ManifestSchemaValidator.h
        ManifestSchemaValidator.cpp
        MigrationRunner.h
        MigrationRunner.cpp
        ProjectPackage.h
        ProjectPackageStore.h
        ProjectPackageStore.cpp
        SqliteProjectDatabase.h
        SqliteProjectDatabase.cpp
        SqliteTimelineStore.h
        SqliteTimelineStore.cpp
        migrations/001_initial.sql
        migrations/002_timeline_editing.sql
        internal/SqliteConnection.h
        internal/SqliteConnection.cpp
        internal/DurableFile.h
        internal/DurableFile.cpp
    DEPENDS
        cs_domain
        nlohmann_json::nlohmann_json
        nlohmann_json_schema_validator::validator
)

target_include_directories(cs_project_store PRIVATE ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(cs_project_store PRIVATE cs_sqlite3)
