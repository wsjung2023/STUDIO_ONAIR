# R0-02 Project Package Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a durable `.cstudio` project package with formal manifest validation, forward-only SQLite migrations, persisted recording/segment state, and user-confirmed crash recovery.

**Architecture:** `cs_project_store` remains Qt-free and exposes synchronous package operations over JSON, SQLite, and the filesystem. A serialized Qt application worker calls those operations off the UI thread; `ProjectController` owns project/recovery UI state, while `StudioController` only orchestrates capture/recorder state and an asynchronous persistence port.

**Tech Stack:** C++20, CMake 3.25+, Qt 6.8.3 Core/Quick/QuickControls2/Test, nlohmann/json 3.11.3, pboettch/json-schema-validator 2.4.0, SQLite 3.53.3 amalgamation, GoogleTest 1.15.2.

## Global Constraints

- Working projects are folders named with the `.cstudio` suffix; ZIP packaging is out of scope.
- `schemas/project.schema.json` declares Draft 7 and is the single manifest-schema source.
- SQLite is `sqlite-amalgamation-3530300.zip`, SHA3-256 `d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`.
- SQLite connections require `journal_mode=WAL`, `synchronous=FULL`, `foreign_keys=ON`, a 2000 ms busy timeout, and `quick_check` on open.
- Project-store/domain targets must remain Qt-free; capture sources and recorders never write the project DB directly.
- Filesystem/SQLite work never runs on the UI thread.
- No source-media file is deleted, renamed, or overwritten by recovery.
- Session transitions are only `RECORDING -> COMPLETED|RECOVERED|ABORTED`.
- Segment transitions are only `WRITING -> READY|FAILED`; terminal rows are immutable except for idempotent identical requests.
- All timebase values use `TimestampNs`/`DurationNs`; UTC is metadata/display only.
- Tests use no `sleep` and do not depend on wall-clock execution speed.
- Every external dependency is pinned and recorded in `legal/OSS_BOM.csv`.
- Windows Debug and Release must pass with `/W4 /permissive- /WX`; macOS remains explicitly unverified until CI or a machine runs it.

## File Structure

### Build and embedded resources

- Modify `CMakeLists.txt` — enable C, fetch/pin schema validator and SQLite, generate embedded schema/migration headers.
- Modify `schemas/project.schema.json` — change only the dialect URI to Draft 7.
- Create `cmake/ProjectSchema.h.in` — generated `std::string_view` for the committed schema.
- Create `cmake/Migration001.h.in` — generated SQL text plus SHA-256.
- Create `src/project_store/migrations/001_initial.sql` — initial project/session/segment schema.
- Modify `legal/OSS_BOM.csv` — record exact validator and SQLite versions and boundaries.

### Qt-free project-store

- Create `src/project_store/ManifestSchemaValidator.h/.cpp` — formal raw-JSON validation.
- Create `src/project_store/internal/SqliteConnection.h/.cpp` — SQLite RAII, statements, transactions, error translation.
- Create `src/project_store/MigrationRunner.h/.cpp` — future-version check, 001 application, checksum verification.
- Create `src/project_store/ProjectPackage.h` — package, persisted session, recovery value types.
- Create `src/project_store/SqliteProjectDatabase.h/.cpp` — project identity, recording/segment persistence, recovery.
- Create `src/project_store/IProjectPackageStore.h` — application-facing Qt-free port.
- Create `src/project_store/ProjectPackageStore.h/.cpp` — atomic package creation/open and DB delegation.
- Create `src/project_store/internal/DurableFile.h/.cpp` — same-directory temp write, OS flush, atomic replace.
- Modify `src/project_store/JsonProjectStore.cpp/.h` — schema validation and durable writer reuse.
- Modify `src/project_store/CMakeLists.txt` — compile/link the new files and pinned libraries.

### Application and QML

- Create `src/app/IRecordingPersistence.h` — asynchronous application port used by StudioController.
- Create `src/app/ProjectWorker.h/.cpp` — serialized worker-thread calls into `IProjectPackageStore`.
- Create `src/app/ProjectController.h/.cpp` — QML-facing project/recovery model and recording persistence adapter.
- Create `src/app/RecentProjectRegistry.h/.cpp` — atomic app-config registry, max 20 projects.
- Modify `src/app/StudioController.h/.cpp` — preparing/finalizing state and persisted session sequencing.
- Modify `src/app/CMakeLists.txt`, `src/main.cpp` — composition and links.
- Modify `qml/HomePage.qml`, `qml/Main.qml` — create/open/recent/recovery navigation.
- Create `qml/RecoveryPage.qml` — recover/later UI with no discard action.
- Modify `CMakeLists.txt` — add QML file and Qt Quick Dialogs dependency.

### Tests and documentation

- Create focused tests under `tests/project_store/` for schema, SQLite, migrations, package, session, segment, recovery, and crash exit.
- Create `tests/helpers/CrashRecoveryFixture.cpp` — commit active state and call `std::_Exit(73)`.
- Create `tests/app/ProjectControllerTest.cpp`, `tests/app/RecordingPersistenceFake.h`, `tests/app/QmlSmokeTest.cpp`.
- Modify `tests/CMakeLists.txt`, `tests/app/StudioControllerTest.cpp`.
- Modify `README.md`, `tests/README.md`, `.superpowers/sdd/progress.md` after verified completion.

---

## Common Commands

Run these from a Visual Studio Developer PowerShell with Qt on `PATH`:

```powershell
$env:CMAKE_PREFIX_PATH = 'C:\Qt\6.8.3\msvc2022_64'
$env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Focused Qt-free tests:

```powershell
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='SuiteName.TestName'
```

Focused application tests:

```powershell
& .\build\windows-debug\tests\cs_app_tests.exe --gtest_filter='SuiteName.TestName'
```

### Task 1: Pin Dependencies and Apply the Manifest Schema

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `schemas/project.schema.json:2`
- Create: `cmake/ProjectSchema.h.in`
- Create: `src/project_store/ManifestSchemaValidator.h`
- Create: `src/project_store/ManifestSchemaValidator.cpp`
- Modify: `src/project_store/JsonProjectStore.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `legal/OSS_BOM.csv`
- Create: `tests/project_store/ManifestSchemaValidatorTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `creator::core::Result<void>`, `nlohmann::json`, committed `project.schema.json`.
- Produces: `creator::project_store::validateManifestJson(const nlohmann::json&) -> Result<void>`.

- [ ] **Step 1: Write failing formal-schema tests**

Create tests that prove behavior domain validation cannot prove alone:

```cpp
#include "project_store/ManifestSchemaValidator.h"

#include "core/AppError.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {

nlohmann::json validManifest() {
    return nlohmann::json::parse(R"JSON({
      "schemaVersion": 1,
      "projectId": "123e4567-e89b-42d3-a456-426614174000",
      "name": "강의 프로젝트",
      "createdAt": "2026-07-16T09:30:00Z",
      "updatedAt": "2026-07-16T09:30:00Z",
      "canvas": {"width":1920,"height":1080,"frameRateNumerator":60,
                 "frameRateDenominator":1,"colorSpace":"rec709-sdr"},
      "database": "project.db",
      "directories": {"media":"media","audio":"audio","telemetry":"telemetry",
                      "proxies":"proxies","thumbnails":"thumbnails",
                      "autosave":"autosave","renders":"renders","logs":"logs"},
      "requiredFeatures": []
    })JSON");
}

TEST(ManifestSchemaValidatorTest, AcceptsCommittedManifestShape) {
    EXPECT_TRUE(creator::project_store::validateManifestJson(validManifest()).hasValue());
}

TEST(ManifestSchemaValidatorTest, RejectsAdditionalRootProperty) {
    auto json = validManifest();
    json["unexpected"] = true;
    const auto result = creator::project_store::validateManifestJson(json);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), creator::core::ErrorCode::ParseFailure);
}

TEST(ManifestSchemaValidatorTest, RejectsInvalidUuidAndDateTimeFormats) {
    for (const auto& mutation : {std::pair{"projectId", "not-a-uuid"},
                                 std::pair{"createdAt", "yesterday"}}) {
        auto json = validManifest();
        json[mutation.first] = mutation.second;
        EXPECT_FALSE(creator::project_store::validateManifestJson(json).hasValue())
            << mutation.first;
    }
}

TEST(ManifestSchemaValidatorTest, RejectsDuplicateRequiredFeatures) {
    auto json = validManifest();
    json["requiredFeatures"] = {"avatar-2d", "avatar-2d"};
    EXPECT_FALSE(creator::project_store::validateManifestJson(json).hasValue());
}

}  // namespace
```

- [ ] **Step 2: Run the test and verify the missing interface fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
```

Expected: FAIL because `project_store/ManifestSchemaValidator.h` does not exist.

- [ ] **Step 3: Pin the validator and embed the committed schema**

Change the project declaration and add FetchContent configuration after nlohmann/json:

```cmake
project(CreatorStudio VERSION 0.1.0 LANGUAGES C CXX)

set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JSON_VALIDATOR_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json_schema_validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG        2.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json_schema_validator)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/schemas/project.schema.json")
file(READ "${PROJECT_SOURCE_DIR}/schemas/project.schema.json" CS_PROJECT_SCHEMA_JSON)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/ProjectSchema.h.in"
    "${PROJECT_BINARY_DIR}/generated/project_store/ProjectSchema.h"
    @ONLY
)
```

Use this complete template:

```cpp
#pragma once

#include <string_view>

namespace creator::project_store::embedded {
inline constexpr std::string_view kProjectSchema = R"CS_PROJECT_SCHEMA(@CS_PROJECT_SCHEMA_JSON@)CS_PROJECT_SCHEMA";
}  // namespace creator::project_store::embedded
```

Change only the schema dialect line to:

```json
"$schema": "http://json-schema.org/draft-07/schema#",
```

- [ ] **Step 4: Implement schema validation without leaking document contents**

Declare:

```cpp
#pragma once

#include "core/Result.h"

#include <nlohmann/json_fwd.hpp>

namespace creator::project_store {
[[nodiscard]] creator::core::Result<void> validateManifestJson(
    const nlohmann::json& document);
}  // namespace creator::project_store
```

Implement a `basic_error_handler` that records only `json_pointer.to_string()` and the validator
message, never the `instance` argument. Construct `json_validator` with
`nlohmann::json_schema::default_string_format_check`, call `set_root_schema()` with the embedded
schema, validate the document, and translate schema compilation/validation exceptions to
`AppError{ErrorCode::ParseFailure, "manifest schema could not be compiled: " +
std::string{error.what()}}`. The returned validation message format must be:

```cpp
"manifest schema violation at '" + pointer + "': " + message
```

Add `${PROJECT_BINARY_DIR}/generated` as a private include directory and link
`nlohmann_json_schema_validator::validator` from `cs_project_store`.

- [ ] **Step 5: Apply formal validation on both load and save paths**

In `JsonProjectStore.cpp`, validate parsed JSON before `fromJson(json)`, and validate generated JSON
before serializing:

```cpp
if (auto valid = validateManifestJson(json); !valid.hasValue()) {
    return valid.error();
}
```

Preserve the existing early future-version behavior by extracting integer `schemaVersion` before
formal validation in `load()`; values above `ProjectManifest::kCurrentSchemaVersion` return
`UnsupportedVersion` even if the future document contains unknown properties.

- [ ] **Step 6: Run focused and existing manifest tests**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_tests
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='ManifestSchemaValidatorTest.*:JsonProjectStoreTest.*:ProjectManifestTest.*'
```

Expected: all selected tests PASS; invalid UUID/date-time and unknown properties fail through
formal schema validation.

- [ ] **Step 7: Record the dependency and commit**

Add BOM row:

```csv
pboettch/json-schema-validator,Project manifest JSON Schema validation,Static library inside cs_project_store,MIT,Static library; Qt-free adapter boundary,APPROVED,https://github.com/pboettch/json-schema-validator,"Pinned to 2.4.0; Draft 7 with default uuid/date-time format checker"
```

Commit:

```powershell
git add CMakeLists.txt schemas/project.schema.json cmake/ProjectSchema.h.in src/project_store tests/project_store/ManifestSchemaValidatorTest.cpp tests/CMakeLists.txt legal/OSS_BOM.csv
git commit -m "feat(project-store): validate manifests against schema"
```

### Task 2: Add SQLite RAII and Enforce Connection Policy

**Files:**
- Modify: `CMakeLists.txt`
- Create: `src/project_store/internal/SqliteConnection.h`
- Create: `src/project_store/internal/SqliteConnection.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `legal/OSS_BOM.csv`
- Create: `tests/project_store/SqliteConnectionTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: SQLite C API, `AppError`, `Result<T>`, native filesystem paths.
- Produces: move-only `SqliteConnection`, `SqliteStatement`, and `SqliteTransaction` internal helpers.

- [ ] **Step 1: Write failing connection-policy tests**

Tests must open a unique temporary DB and assert exact pragma values:

```cpp
TEST_F(SqliteConnectionTest, OpensWithRequiredPragmas) {
    auto opened = SqliteConnection::open(databasePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    auto connection = std::move(opened).value();

    EXPECT_EQ(connection.scalarText("PRAGMA journal_mode").value(), "wal");
    EXPECT_EQ(connection.scalarInt64("PRAGMA synchronous").value(), 2);
    EXPECT_EQ(connection.scalarInt64("PRAGMA foreign_keys").value(), 1);
    EXPECT_EQ(connection.scalarText("PRAGMA quick_check").value(), "ok");
}

TEST_F(SqliteConnectionTest, ReportsCorruptDatabaseWithoutThrowing) {
    std::ofstream out{databasePath_, std::ios::binary};
    out << "not sqlite";
    out.close();

    const auto opened = SqliteConnection::open(databasePath_);
    ASSERT_FALSE(opened.hasValue());
    EXPECT_EQ(opened.error().code(), ErrorCode::IoFailure);
}
```

- [ ] **Step 2: Verify the tests fail for the missing helper**

Run `cmake --build --preset windows-debug --target cs_tests`.

Expected: FAIL because `internal/SqliteConnection.h` does not exist.

- [ ] **Step 3: Fetch and build the pinned amalgamation**

Add:

```cmake
FetchContent_Declare(
    sqlite_amalgamation
    URL      https://sqlite.org/2026/sqlite-amalgamation-3530300.zip
    URL_HASH SHA3_256=d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9
)
FetchContent_MakeAvailable(sqlite_amalgamation)

add_library(cs_sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
target_include_directories(cs_sqlite3 PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
target_compile_definitions(cs_sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_API_ARMOR=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_THREADSAFE=1
)
```

Do not apply C++ warning flags to the upstream C translation unit. Link `cs_sqlite3` privately from
`cs_project_store`; the SQLite headers remain internal.

- [ ] **Step 4: Implement move-only RAII wrappers**

Use these exact public-internal declarations:

```cpp
enum class SqliteStep { Row, Done };

class SqliteStatement final {
public:
    SqliteStatement(SqliteStatement&& other) noexcept;
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;
    ~SqliteStatement();
    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    [[nodiscard]] core::Result<void> bindText(int index, std::string_view value);
    [[nodiscard]] core::Result<void> bindInt64(int index, std::int64_t value);
    [[nodiscard]] core::Result<void> bindNull(int index);
    [[nodiscard]] core::Result<SqliteStep> step();
    [[nodiscard]] std::int64_t columnInt64(int index) const noexcept;
    [[nodiscard]] std::string columnText(int index) const;
    [[nodiscard]] bool columnIsNull(int index) const noexcept;
    [[nodiscard]] core::Result<void> reset();
private:
    friend class SqliteConnection;
    SqliteStatement(sqlite3* database, sqlite3_stmt* statement);
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

class SqliteConnection final {
public:
    [[nodiscard]] static core::Result<SqliteConnection> open(
        const std::filesystem::path& databasePath);
    SqliteConnection(SqliteConnection&& other) noexcept;
    SqliteConnection& operator=(SqliteConnection&& other) noexcept;
    ~SqliteConnection();
    SqliteConnection(const SqliteConnection&) = delete;
    SqliteConnection& operator=(const SqliteConnection&) = delete;

    [[nodiscard]] core::Result<void> execute(std::string_view sql);
    [[nodiscard]] core::Result<SqliteStatement> prepare(std::string_view sql);
    [[nodiscard]] core::Result<std::int64_t> scalarInt64(std::string_view sql);
    [[nodiscard]] core::Result<std::string> scalarText(std::string_view sql);
private:
    explicit SqliteConnection(sqlite3* database) : database_(database) {}
    sqlite3* database_{};
};

class SqliteTransaction final {
public:
    [[nodiscard]] static core::Result<SqliteTransaction> beginImmediate(
        SqliteConnection& connection);
    SqliteTransaction(SqliteTransaction&& other) noexcept;
    ~SqliteTransaction();
    [[nodiscard]] core::Result<void> commit();
    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;
private:
    explicit SqliteTransaction(SqliteConnection& connection) : connection_(&connection) {}
    SqliteConnection* connection_{};
    bool committed_{false};
};
```

On Windows call `sqlite3_open16(databasePath.c_str(), &database)`; on POSIX use:

```cpp
const auto utf8 = databasePath.u8string();
sqlite3_open(reinterpret_cast<const char*>(utf8.c_str()), &database);
```

Immediately set the 2000 ms busy timeout, execute all required pragmas,
verify `journal_mode` returned `wal`, and run `quick_check`. Every non-OK result becomes
`IoFailure` with operation and numeric SQLite code, never SQL parameter values.

`SqliteTransaction` begins with `BEGIN IMMEDIATE`, rolls back in its destructor unless committed,
and never throws from the destructor.

- [ ] **Step 5: Run focused tests and commit**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_tests
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='SqliteConnectionTest.*'
```

Expected: all selected tests PASS.

Update the existing SQLite BOM row notes to include `3.53.3`, the amalgamation filename/hash, and
the compile definitions above. Commit:

```powershell
git add CMakeLists.txt src/project_store tests/project_store/SqliteConnectionTest.cpp tests/CMakeLists.txt legal/OSS_BOM.csv
git commit -m "feat(project-store): add pinned SQLite connection layer"
```

### Task 3: Create the Forward-Only Initial Migration

**Files:**
- Create: `src/project_store/migrations/001_initial.sql`
- Create: `cmake/Migration001.h.in`
- Modify: `CMakeLists.txt`
- Create: `src/project_store/MigrationRunner.h`
- Create: `src/project_store/MigrationRunner.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Create: `tests/project_store/MigrationRunnerTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `internal::SqliteConnection`, embedded SQL/checksum.
- Produces: `MigrationRunner::apply(SqliteConnection&) -> Result<void>`, internal descriptor-driven
  runner for failure tests, and `kLatestVersion = 1`.

- [ ] **Step 1: Write migration behavior tests**

Cover first apply, second apply, rollback, checksum mismatch, and future DB:

```cpp
TEST_F(MigrationRunnerTest, AppliesMigrationOneExactlyOnce) {
    auto connection = openDatabase();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 1);
    EXPECT_EQ(connection.scalarInt64(
        "SELECT count(*) FROM schema_migrations WHERE version=1").value(), 1);
}

TEST_F(MigrationRunnerTest, RejectsFutureDatabaseWithoutChangingVersion) {
    auto connection = openDatabase();
    ASSERT_TRUE(connection.execute("PRAGMA user_version=2").hasValue());
    const auto result = MigrationRunner::apply(connection);
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::UnsupportedVersion);
    EXPECT_EQ(connection.scalarInt64("PRAGMA user_version").value(), 2);
}

TEST_F(MigrationRunnerTest, RejectsChangedChecksum) {
    auto connection = openDatabase();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    ASSERT_TRUE(connection.execute(
        "UPDATE schema_migrations SET checksum='wrong' WHERE version=1").hasValue());
    EXPECT_FALSE(MigrationRunner::apply(connection).hasValue());
}
```

Expose this exact internal seam and use it to inject invalid SQL, proving no partial tables or
`user_version` remain after rollback:

```cpp
namespace creator::project_store::internal {
struct MigrationDescriptor final {
    std::int32_t version;
    std::string_view name;
    std::string_view checksum;
    std::string_view sql;
};
[[nodiscard]] core::Result<void> applyMigrations(
    SqliteConnection&, std::span<const MigrationDescriptor> migrations);
}  // namespace creator::project_store::internal
```

`MigrationRunner::apply()` creates the one production descriptor and delegates to
`internal::applyMigrations()`.

- [ ] **Step 2: Run and verify the tests fail**

Run `cmake --build --preset windows-debug --target cs_tests`.

Expected: FAIL because `MigrationRunner.h` is missing.

- [ ] **Step 3: Add the complete initial SQL**

```sql
CREATE TABLE schema_migrations(
    version INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    checksum TEXT NOT NULL,
    applied_at_utc TEXT NOT NULL
);

CREATE TABLE projects(
    project_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    manifest_schema_version INTEGER NOT NULL,
    created_at_utc TEXT NOT NULL,
    updated_at_utc TEXT NOT NULL
);

CREATE TABLE recording_sessions(
    session_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    state TEXT NOT NULL CHECK(state IN ('RECORDING','COMPLETED','RECOVERED','ABORTED')),
    started_ns INTEGER NOT NULL CHECK(started_ns >= 0),
    stopped_ns INTEGER CHECK(stopped_ns IS NULL OR stopped_ns >= started_ns),
    created_at_utc TEXT NOT NULL,
    finished_at_utc TEXT,
    failure_reason TEXT
);

CREATE TABLE segments(
    session_id TEXT NOT NULL REFERENCES recording_sessions(session_id),
    source_id TEXT NOT NULL,
    segment_index INTEGER NOT NULL CHECK(segment_index >= 0),
    start_ns INTEGER NOT NULL CHECK(start_ns >= 0),
    duration_ns INTEGER CHECK(duration_ns >= 0),
    status TEXT NOT NULL CHECK(status IN ('WRITING','READY','FAILED')),
    relative_path TEXT NOT NULL,
    CHECK(status != 'READY' OR duration_ns IS NOT NULL),
    PRIMARY KEY(session_id, source_id, segment_index)
);

CREATE INDEX recording_sessions_project_state
    ON recording_sessions(project_id, state);
CREATE INDEX segments_session_status
    ON segments(session_id, status);
```

- [ ] **Step 4: Embed migration bytes and implement the runner**

At configure time:

```cmake
set(CS_MIGRATION_001 "${PROJECT_SOURCE_DIR}/src/project_store/migrations/001_initial.sql")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CS_MIGRATION_001}")
file(READ "${CS_MIGRATION_001}" CS_MIGRATION_001_SQL)
file(SHA256 "${CS_MIGRATION_001}" CS_MIGRATION_001_SHA256)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/Migration001.h.in"
    "${PROJECT_BINARY_DIR}/generated/project_store/Migration001.h"
    @ONLY
)
```

The generated header exposes `kMigration001Sql`, `kMigration001Sha256`, name
`001_initial`, and version `1`. `MigrationRunner::apply` must:

1. read `PRAGMA user_version` before creating any table;
2. return `UnsupportedVersion` if it is above 1;
3. if version is 1, verify exactly one migration row and matching name/checksum;
4. if version is 0, begin immediate transaction, execute the SQL, insert the migration row with
   `Utc::now().toRfc3339()`, set `PRAGMA user_version=1`, and commit;
5. translate malformed/current-version metadata to `IoFailure` without altering it.

- [ ] **Step 5: Run tests and commit**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_tests
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='MigrationRunnerTest.*'
```

Expected: all selected tests PASS, including injected rollback.

Commit:

```powershell
git add CMakeLists.txt cmake/Migration001.h.in src/project_store/migrations src/project_store/MigrationRunner.* src/project_store/CMakeLists.txt tests/project_store/MigrationRunnerTest.cpp tests/CMakeLists.txt
git commit -m "feat(project-store): add initial forward-only migration"
```

### Task 4: Persist Project Identity and Recording Sessions

**Files:**
- Create: `src/project_store/ProjectPackage.h`
- Create: `src/project_store/SqliteProjectDatabase.h`
- Create: `src/project_store/SqliteProjectDatabase.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Create: `tests/project_store/SqliteProjectDatabaseTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ProjectManifest`, `SessionId`, `TimestampNs`, `Utc`, `MigrationRunner`.
- Produces: a move-only opened database with project identity and session lifecycle methods.

- [ ] **Step 1: Define persisted value types and write failing tests**

Create these Qt-free types:

```cpp
enum class PersistedSessionState { Recording, Completed, Recovered, Aborted };

struct RecordingSessionRecord final {
    creator::domain::SessionId id;
    PersistedSessionState state{PersistedSessionState::Recording};
    creator::core::TimestampNs startedAt{};
    std::optional<creator::core::TimestampNs> stoppedAt;
    creator::core::Utc createdAt;
    std::optional<creator::core::Utc> finishedAt;
    std::optional<std::string> failureReason;
};

struct ProjectPackage final {
    std::filesystem::path path;
    creator::domain::ProjectManifest manifest;
};
```

`RecordingSessionRecord` cannot default-construct because `SessionId` and `Utc` cannot; tests must
use designated initialization with every required field.

Write these tests:

```cpp
TEST_F(SqliteProjectDatabaseTest, CreatesProjectRowMatchingManifest) {
    const auto database = createDatabase(manifest());
    ASSERT_TRUE(database.hasValue()) << database.error().message();
    EXPECT_EQ(queryText(databasePath_, "SELECT project_id FROM projects"),
              manifest().projectId.value());
}

TEST_F(SqliteProjectDatabaseTest, RejectsManifestDatabaseIdentityMismatch) {
    ASSERT_TRUE(createDatabase(manifest()).hasValue());
    auto different = manifest();
    different.projectId = ProjectId::create("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa").value();
    const auto opened = SqliteProjectDatabase::open(databasePath_, different.projectId);
    ASSERT_FALSE(opened.hasValue());
    EXPECT_EQ(opened.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(SqliteProjectDatabaseTest, PersistsRecordingCompletedAndAbortedStates) {
    auto database = createDatabase(manifest()).value();
    const auto session = SessionId::create("session-1").value();
    ASSERT_TRUE(database.beginRecording(session, TimestampNs{}, utc("2026-07-16T10:00:00Z"))
                    .hasValue());
    EXPECT_EQ(database.session(session).value().state, PersistedSessionState::Recording);

    ASSERT_TRUE(database.completeRecording(
        session, TimestampNs{} + std::chrono::seconds{3}, {},
        utc("2026-07-16T10:00:03Z")).hasValue());
    EXPECT_EQ(database.session(session).value().state, PersistedSessionState::Completed);

    const auto aborted = SessionId::create("session-2").value();
    ASSERT_TRUE(database.beginRecording(aborted, TimestampNs{},
                                        utc("2026-07-16T10:01:00Z")).hasValue());
    ASSERT_TRUE(database.abortRecording(aborted, "source permission denied",
                                        utc("2026-07-16T10:01:01Z")).hasValue());
    EXPECT_EQ(database.session(aborted).value().state, PersistedSessionState::Aborted);
}
```

- [ ] **Step 2: Run and verify the missing database class fails**

Run `cmake --build --preset windows-debug --target cs_tests`.

Expected: FAIL because `SqliteProjectDatabase.h` does not exist.

- [ ] **Step 3: Declare the exact database API**

```cpp
class SqliteProjectDatabase final {
public:
    [[nodiscard]] static core::Result<SqliteProjectDatabase> create(
        const std::filesystem::path& databasePath,
        const domain::ProjectManifest& manifest);
    [[nodiscard]] static core::Result<SqliteProjectDatabase> open(
        const std::filesystem::path& databasePath,
        const domain::ProjectId& expectedProjectId);

    SqliteProjectDatabase(SqliteProjectDatabase&&) noexcept = default;
    SqliteProjectDatabase& operator=(SqliteProjectDatabase&&) noexcept = default;
    SqliteProjectDatabase(const SqliteProjectDatabase&) = delete;
    SqliteProjectDatabase& operator=(const SqliteProjectDatabase&) = delete;

    [[nodiscard]] core::Result<void> beginRecording(
        const domain::SessionId& sessionId, core::TimestampNs startedAt,
        const core::Utc& createdAt);
    [[nodiscard]] core::Result<void> completeRecording(
        const domain::SessionId& sessionId, core::TimestampNs stoppedAt,
        const std::vector<domain::SegmentInfo>& segments, const core::Utc& finishedAt);
    [[nodiscard]] core::Result<void> abortRecording(
        const domain::SessionId& sessionId, std::string_view reason,
        const core::Utc& finishedAt);
    [[nodiscard]] core::Result<RecordingSessionRecord> session(
        const domain::SessionId& sessionId);

private:
    SqliteProjectDatabase(internal::SqliteConnection connection, domain::ProjectId projectId);
    internal::SqliteConnection connection_;
    domain::ProjectId projectId_;
};
```

- [ ] **Step 4: Implement create/open and guarded session transitions**

`create()` opens the DB, applies migrations, and inserts `projects` with bound values in one
transaction. `open()` applies/verifies migrations and selects exactly one `projects.project_id`;
missing or multiple rows are `IoFailure`, mismatch is `InvalidArgument`.

Convert `TimestampNs` to SQLite with:

```cpp
const auto toNanoseconds = [](core::TimestampNs value) {
    return value.time_since_epoch().count();
};
```

`beginRecording()` inserts only a new `RECORDING` row. Duplicate session IDs return
`AlreadyExists`. `abortRecording()` executes:

```sql
UPDATE recording_sessions
SET state='ABORTED', stopped_ns=started_ns, finished_at_utc=?1, failure_reason=?2
WHERE session_id=?3 AND project_id=?4 AND state='RECORDING';
```

Require `sqlite3_changes()==1`; zero changes are `InvalidState`. Add an internal
`SqliteConnection::changes() const noexcept -> int` method rather than exposing `sqlite3*`.

`completeRecording()` begins an immediate transaction, verifies `stoppedAt >= startedAt`, stores
the supplied segments through the Task 5 internal helper, and changes the session to `COMPLETED`.
For this task an empty segment vector must work; Task 5 fills in segment persistence.

- [ ] **Step 5: Run tests and commit**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='SqliteProjectDatabaseTest.*'
```

Expected: all identity/session tests PASS.

Commit:

```powershell
git add src/project_store/ProjectPackage.h src/project_store/SqliteProjectDatabase.* src/project_store/internal/SqliteConnection.* src/project_store/CMakeLists.txt tests/project_store/SqliteProjectDatabaseTest.cpp tests/CMakeLists.txt
git commit -m "feat(project-store): persist project and recording sessions"
```

### Task 5: Persist Segment State and Recover Interrupted Sessions

**Files:**
- Modify: `src/project_store/ProjectPackage.h`
- Modify: `src/project_store/SqliteProjectDatabase.h`
- Modify: `src/project_store/SqliteProjectDatabase.cpp`
- Create: `tests/project_store/SegmentPersistenceTest.cpp`
- Create: `tests/project_store/RecoveryTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: opened `SqliteProjectDatabase`, `SegmentInfo`, active recording sessions.
- Produces: segment transition methods, `RecoveryCandidate`, `RecoveryResult`, scan/recover methods.

- [ ] **Step 1: Add recovery value types and failing segment tests**

```cpp
struct RecoveryCandidate final {
    std::filesystem::path packagePath;
    std::string projectName;
    creator::domain::SessionId sessionId;
    creator::core::Utc createdAt;
    std::size_t readySegments{0};
    std::size_t writingSegments{0};
};

struct RecoveryResult final {
    creator::domain::SessionId sessionId;
    creator::core::TimestampNs stoppedAt{};
    std::size_t readySegments{0};
    std::size_t failedSegments{0};
};
```

Write segment transition tests using this helper:

```cpp
SegmentInfo segment(SegmentStatus status, std::uint64_t index = 0) {
    return SegmentInfo{
        .index = index,
        .sourceId = SourceId::create("screen-1").value(),
        .startTime = TimestampNs{} + std::chrono::seconds{2 * index},
        .duration = std::chrono::seconds{2},
        .status = status,
        .relativePath = "media/screen-1/segment_00000" + std::to_string(index) + ".mkv",
    };
}

TEST_F(SegmentPersistenceTest, AllowsWritingToReadyAndRejectsTerminalRewrite) {
    auto database = activeDatabase();
    ASSERT_TRUE(database.beginSegment(sessionId_, segment(SegmentStatus::Writing)).hasValue());
    ASSERT_TRUE(database.markSegmentReady(sessionId_, segment(SegmentStatus::Ready)).hasValue());
    EXPECT_TRUE(database.markSegmentReady(sessionId_, segment(SegmentStatus::Ready)).hasValue());

    auto changed = segment(SegmentStatus::Ready);
    changed.duration = std::chrono::seconds{3};
    const auto rewrite = database.markSegmentReady(sessionId_, changed);
    ASSERT_FALSE(rewrite.hasValue());
    EXPECT_EQ(rewrite.error().code(), ErrorCode::InvalidState);
}

TEST_F(SegmentPersistenceTest, RejectsAbsoluteAndEscapingPaths) {
    auto database = activeDatabase();
    for (const std::string path : {"C:/outside.mkv", "../outside.mkv", "media/../../x.mkv"}) {
        auto value = segment(SegmentStatus::Writing);
        value.relativePath = path;
        EXPECT_FALSE(database.beginSegment(sessionId_, value).hasValue()) << path;
    }
}
```

- [ ] **Step 2: Write failing recovery tests**

```cpp
TEST_F(RecoveryTest, ScanReturnsOnlyRecordingSessionsWithCounts) {
    auto database = databaseWithRecordingReadyAndWriting();
    const auto candidates = database.scanRecovery(packagePath_, "강의");
    ASSERT_TRUE(candidates.hasValue());
    ASSERT_EQ(candidates.value().size(), 1u);
    EXPECT_EQ(candidates.value()[0].readySegments, 1u);
    EXPECT_EQ(candidates.value()[0].writingSegments, 1u);
}

TEST_F(RecoveryTest, RecoverKeepsReadyAndFailsOnlyWriting) {
    auto database = databaseWithRecordingReadyAndWriting();
    const auto recovered = database.recover(sessionId_, utc("2026-07-16T11:00:00Z"));
    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(recovered.value().stoppedAt,
              TimestampNs{} + std::chrono::seconds{2});
    EXPECT_EQ(recovered.value().readySegments, 1u);
    EXPECT_EQ(recovered.value().failedSegments, 1u);
    EXPECT_EQ(database.session(sessionId_).value().state, PersistedSessionState::Recovered);
}

TEST_F(RecoveryTest, RecoverIsIdempotent) {
    auto database = databaseWithRecordingReadyAndWriting();
    const auto first = database.recover(sessionId_, utc("2026-07-16T11:00:00Z"));
    const auto second = database.recover(sessionId_, utc("2026-07-16T11:05:00Z"));
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(second.hasValue());
    EXPECT_EQ(second.value().stoppedAt, first.value().stoppedAt);
    EXPECT_EQ(second.value().readySegments, first.value().readySegments);
    EXPECT_EQ(second.value().failedSegments, first.value().failedSegments);
}
```

- [ ] **Step 3: Run and verify missing methods fail**

Run `cmake --build --preset windows-debug --target cs_tests`.

Expected: FAIL for undeclared segment/recovery methods.

- [ ] **Step 4: Declare and implement exact transition methods**

Add:

```cpp
[[nodiscard]] core::Result<void> beginSegment(
    const domain::SessionId&, const domain::SegmentInfo&);
[[nodiscard]] core::Result<void> markSegmentReady(
    const domain::SessionId&, const domain::SegmentInfo&);
[[nodiscard]] core::Result<void> markSegmentFailed(
    const domain::SessionId&, const domain::SourceId&, std::uint64_t segmentIndex);
[[nodiscard]] core::Result<std::vector<RecoveryCandidate>> scanRecovery(
    const std::filesystem::path& packagePath, std::string_view projectName);
[[nodiscard]] core::Result<RecoveryResult> recover(
    const domain::SessionId&, const core::Utc& finishedAt);
```

Normalize with `std::filesystem::path::lexically_normal()`. Reject empty, absolute,
rooted/root-name paths and any normalized component equal to `..`. Convert paths to generic UTF-8
for storage and never join an unvalidated path to the package root.

`beginSegment()` inserts `WRITING` with `duration_ns=NULL`. `markSegmentReady()` first selects the
existing row. If it is identical `READY`, return success. If it is not matching `WRITING`, return
`InvalidState`; otherwise update every final metadata column and set `READY`. `markSegmentFailed()`
uses the same terminal/idempotent rule.

`completeRecording()` calls a private `storeCompletedSegment()` for each returned `READY` segment
inside the same session-completion transaction. It may insert a missing row directly as `READY`
for the R0-02 fake recorder, but it may not replace a conflicting terminal row.

- [ ] **Step 5: Implement recovery with checked time arithmetic**

Within one immediate transaction:

```sql
SELECT state, started_ns FROM recording_sessions
WHERE session_id=?1 AND project_id=?2;

SELECT MAX(start_ns + duration_ns)
FROM segments WHERE session_id=?1 AND status='READY';

UPDATE segments SET status='FAILED'
WHERE session_id=?1 AND status='WRITING';

UPDATE recording_sessions
SET state='RECOVERED', stopped_ns=?1, finished_at_utc=?2
WHERE session_id=?3 AND project_id=?4 AND state='RECORDING';
```

Do not use the SQL addition result as the authoritative stopped time. Select individual
`start_ns,duration_ns` rows and use checked C++ addition:

```cpp
if (duration > std::numeric_limits<std::int64_t>::max() - start) {
    return AppError{ErrorCode::InvalidArgument, "segment end timestamp overflows int64"};
}
```

If no READY rows exist, use `started_ns`. For a session already `RECOVERED`, query and return the
stored result without any UPDATE. Other terminal states return `InvalidState`.

- [ ] **Step 6: Run focused tests and commit**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='SegmentPersistenceTest.*:RecoveryTest.*:SqliteProjectDatabaseTest.*'
```

Expected: all selected tests PASS.

Commit:

```powershell
git add src/project_store/ProjectPackage.h src/project_store/SqliteProjectDatabase.* tests/project_store/SegmentPersistenceTest.cpp tests/project_store/RecoveryTest.cpp tests/CMakeLists.txt
git commit -m "feat(project-store): persist segments and recover sessions"
```

### Task 6: Assemble and Durably Open `.cstudio` Packages

**Files:**
- Create: `src/project_store/internal/DurableFile.h`
- Create: `src/project_store/internal/DurableFile.cpp`
- Modify: `src/project_store/JsonProjectStore.cpp`
- Create: `src/project_store/IProjectPackageStore.h`
- Create: `src/project_store/ProjectPackageStore.h`
- Create: `src/project_store/ProjectPackageStore.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Create: `tests/project_store/ProjectPackageStoreTest.cpp`
- Modify: `tests/project_store/JsonProjectStoreTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: manifest store/validator, `SqliteProjectDatabase`, native filesystem.
- Produces: atomic package create/open and a mockable `IProjectPackageStore` port.

- [ ] **Step 1: Write failing atomic package tests**

```cpp
TEST_F(ProjectPackageStoreTest, CreatePublishesCompletePackageAtOnce) {
    const auto result = store_.create(packagePath_, "강의 프로젝트");
    ASSERT_TRUE(result.hasValue()) << result.error().message();
    EXPECT_TRUE(fs::exists(packagePath_ / "manifest.json"));
    EXPECT_TRUE(fs::exists(packagePath_ / "project.db"));
    EXPECT_TRUE(fs::is_directory(packagePath_ / "media"));
    EXPECT_TRUE(store_.open(packagePath_).hasValue());
    EXPECT_TRUE(findCreatingSiblings(packagePath_).empty());
}

TEST_F(ProjectPackageStoreTest, CreateNeverOverwritesExistingTarget) {
    fs::create_directories(packagePath_);
    std::ofstream{packagePath_ / "keep.txt"} << "keep";
    const auto result = store_.create(packagePath_, "Second");
    ASSERT_FALSE(result.hasValue());
    EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
    EXPECT_TRUE(fs::exists(packagePath_ / "keep.txt"));
}

TEST_F(ProjectPackageStoreTest, OpenRejectsManifestDatabaseMismatch) {
    ASSERT_TRUE(store_.create(packagePath_, "Original").hasValue());
    replaceManifestProjectId(packagePath_, "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    EXPECT_FALSE(store_.open(packagePath_).hasValue());
}
```

Add a deterministic create-failure test whose target parent is a regular file rather than a
directory. Assert `IoFailure`, no final package, and no sibling name containing `.creating-`. Do not
introduce a virtual filesystem solely for this test.

- [ ] **Step 2: Run and verify missing package store fails**

Run `cmake --build --preset windows-debug --target cs_tests`.

Expected: FAIL because `ProjectPackageStore.h` is missing.

- [ ] **Step 3: Define the application-facing port**

```cpp
struct OpenProjectResult final {
    ProjectPackage package;
    std::vector<RecoveryCandidate> recoveryCandidates;
};

class IProjectPackageStore {
public:
    virtual ~IProjectPackageStore() = default;
    [[nodiscard]] virtual core::Result<OpenProjectResult> create(
        const std::filesystem::path&, const std::string& name) = 0;
    [[nodiscard]] virtual core::Result<OpenProjectResult> open(
        const std::filesystem::path&) = 0;
    [[nodiscard]] virtual core::Result<void> beginRecording(
        const std::filesystem::path&, const domain::SessionId&,
        core::TimestampNs, const core::Utc&) = 0;
    [[nodiscard]] virtual core::Result<void> completeRecording(
        const std::filesystem::path&, const domain::RecordingSession&,
        const core::Utc&) = 0;
    [[nodiscard]] virtual core::Result<void> abortRecording(
        const std::filesystem::path&, const domain::SessionId&,
        std::string_view reason, const core::Utc&) = 0;
    [[nodiscard]] virtual core::Result<void> beginSegment(
        const std::filesystem::path&, const domain::SessionId&,
        const domain::SegmentInfo&) = 0;
    [[nodiscard]] virtual core::Result<void> markSegmentReady(
        const std::filesystem::path&, const domain::SessionId&,
        const domain::SegmentInfo&) = 0;
    [[nodiscard]] virtual core::Result<void> markSegmentFailed(
        const std::filesystem::path&, const domain::SessionId&,
        const domain::SourceId&, std::uint64_t segmentIndex) = 0;
    [[nodiscard]] virtual core::Result<RecoveryResult> recover(
        const std::filesystem::path&, const domain::SessionId&,
        const core::Utc&) = 0;
protected:
    IProjectPackageStore() = default;
};
```

Delete copy/move operations in the same style as `IProjectStore`.

- [ ] **Step 4: Implement durable manifest replacement**

Expose only:

```cpp
[[nodiscard]] core::Result<void> writeFileDurably(
    const std::filesystem::path& target, std::string_view contents);
```

Create a unique same-directory `.<filename>.part-<uuid>` file. On Windows use `CreateFileW`,
`WriteFile`, `FlushFileBuffers`, then `MoveFileExW` with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. On POSIX use `open/write/fsync/close`,
`rename`, then open and `fsync` the parent directory. Every partial write loops until all bytes are
written. On failure close handles, remove only the unique temp path, and preserve the target.

Replace `JsonProjectStore.cpp`'s local `writeFileAtomically` with this helper. Keep existing
failure-preserves-old-manifest tests and add one successful overwrite test on Windows.

- [ ] **Step 5: Implement staging create and strict open order**

`ProjectPackageStore::create()` must:

1. reject an existing final target before creating anything;
2. generate a sibling `<filename>.creating-<uuid>` path;
3. call `JsonProjectStore::create(staging, name)`;
4. call `SqliteProjectDatabase::create(staging / manifest.database, manifest)` and close it;
5. rename staging to final;
6. on failure, resolve/compare parent and remove only the generated sibling staging path.

`open()` must load/validate manifest first, reject absolute or escaping `database`, open/migrate DB,
verify project ID, and call `scanRecovery(packagePath, manifest.name)`. It returns both package and
candidates.

Every recording, segment, and recovery method opens the package through the same validation path,
performs one DB operation, and closes the connection. The app worker serializes calls, so no
connection pool is introduced.

- [ ] **Step 6: Run focused tests and commit**

Run:

```powershell
cmake --build --preset windows-debug --target cs_tests
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='ProjectPackageStoreTest.*:JsonProjectStoreTest.*'
```

Expected: package and manifest tests PASS; no `.creating-*` or `.part-*` remains.

Commit:

```powershell
git add src/project_store tests/project_store/ProjectPackageStoreTest.cpp tests/project_store/JsonProjectStoreTest.cpp tests/CMakeLists.txt
git commit -m "feat(project-store): create durable project packages"
```

### Task 7: Reproduce Process Death and Prove Recovery

**Files:**
- Create: `tests/helpers/CrashRecoveryFixture.cpp`
- Create: `tests/project_store/CrashRecoveryIntegrationTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ProjectPackageStore`, process executable path, persisted recovery API.
- Produces: deterministic cross-process crash-equivalent acceptance test.

- [ ] **Step 1: Write the parent integration test**

The test receives the helper path through compile definition
`CS_CRASH_FIXTURE_PATH="$<TARGET_FILE:cs_crash_recovery_fixture>"`, creates a unique package path,
launches the helper with package path, waits for process termination, and asserts exit code 73.
Use `CreateProcessW`/`WaitForSingleObject` on Windows and `fork`/`exec`/`waitpid` on POSIX in a small
test-local `runFixture()` function. No polling delay is allowed.

Then:

```cpp
TEST_F(CrashRecoveryIntegrationTest, ReopensAndRecoversAfterExitWithoutDestructors) {
    ASSERT_EQ(runFixture(packagePath_), 73);

    ProjectPackageStore store;
    const auto opened = store.open(packagePath_);
    ASSERT_TRUE(opened.hasValue()) << opened.error().message();
    ASSERT_EQ(opened.value().recoveryCandidates.size(), 1u);

    const auto candidate = opened.value().recoveryCandidates.front();
    const auto before = snapshotFixtureFiles(packagePath_);
    const auto recovered = store.recover(packagePath_, candidate.sessionId,
                                         Utc::now());
    ASSERT_TRUE(recovered.hasValue()) << recovered.error().message();
    EXPECT_EQ(snapshotFixtureFiles(packagePath_), before);
    EXPECT_TRUE(store.open(packagePath_).value().recoveryCandidates.empty());
}
```

- [ ] **Step 2: Run and verify the helper target is missing**

Run `cmake --build --preset windows-debug --target cs_tests`.

Expected: configure/build FAIL because `cs_crash_recovery_fixture` is not defined.

- [ ] **Step 3: Implement the crash fixture**

The helper must parse exactly one native path argument, create a package, open a
`SqliteProjectDatabase` directly and keep that object alive, begin `session-crash`, insert one READY
and one WRITING segment through the DB API, create two small fixture media files, and terminate with:

```cpp
std::_Exit(73);
```

Do not close the store/database explicitly and do not return from `main`; `_Exit` is what proves the
next process can recover committed WAL state without destructors.

- [ ] **Step 4: Build and run the integration test**

Run:

```powershell
cmake --build --preset windows-debug --target cs_crash_recovery_fixture cs_tests
& .\build\windows-debug\tests\cs_tests.exe --gtest_filter='CrashRecoveryIntegrationTest.*'
```

Expected: PASS with helper exit 73, one recovery candidate, unchanged media bytes, and no candidate
after recovery.

- [ ] **Step 5: Commit**

```powershell
git add tests/helpers/CrashRecoveryFixture.cpp tests/project_store/CrashRecoveryIntegrationTest.cpp tests/CMakeLists.txt
git commit -m "test(project-store): prove recovery after process death"
```

### Task 8: Add the Recent Registry and Serialized Project Worker

**Files:**
- Create: `src/app/RecentProjectRegistry.h`
- Create: `src/app/RecentProjectRegistry.cpp`
- Create: `src/app/ProjectWorker.h`
- Create: `src/app/ProjectWorker.cpp`
- Create: `src/app/ProjectController.h`
- Create: `src/app/ProjectController.cpp`
- Modify: `src/app/CMakeLists.txt`
- Create: `tests/app/ProjectControllerTest.cpp`
- Create: `tests/app/FakeProjectPackageStore.h`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `IProjectPackageStore`, Qt queued signals, app config path.
- Produces: QML-facing async project state and a serialized production worker.

- [ ] **Step 1: Write failing controller tests with a thread-observing fake**

`FakeProjectPackageStore` implements every port method, records
`QThread::currentThreadId()` for each call, and returns configured results. Tests:

```cpp
TEST_F(ProjectControllerTest, CreateRunsStoreOffUiThreadAndPublishesProject) {
    QSignalSpy opened{controller_.get(), &ProjectController::projectOpened};
    controller_->createProject(QUrl::fromLocalFile(
                                   QString::fromStdWString(packagePath_.wstring())),
                               QStringLiteral("강의"));
    ASSERT_TRUE(opened.wait(3000));
    EXPECT_NE(fake_->lastThreadId(), QThread::currentThreadId());
    EXPECT_TRUE(controller_->hasOpenProject());
    EXPECT_EQ(controller_->projectName(), QStringLiteral("강의"));
    EXPECT_FALSE(controller_->busy());
}

TEST_F(ProjectControllerTest, OpenPublishesRecoveryCandidates) {
    fake_->setOpenResult(openResultWithOneRecovery());
    QSignalSpy required{controller_.get(), &ProjectController::recoveryRequired};
    controller_->openProject(QUrl::fromLocalFile(
        QString::fromStdWString(packagePath_.wstring())));
    ASSERT_TRUE(required.wait(3000));
    EXPECT_EQ(controller_->recoveries().size(), 1);
}

TEST_F(ProjectControllerTest, RejectsSecondCommandWhileBusy) {
    fake_->holdNextCall();
    controller_->openProject(QUrl::fromLocalFile(
        QString::fromStdWString(packagePath_.wstring())));
    controller_->openProject(QUrl::fromLocalFile(
        QString::fromStdWString(otherPath_.wstring())));
    EXPECT_EQ(controller_->statusMessage(), QStringLiteral("A project operation is already running"));
    fake_->releaseHeldCall();
}
```

The fake uses `std::promise<void>`/`std::shared_future<void>` for hold/release; no sleeps.

- [ ] **Step 2: Run and verify the controller is missing**

Run `cmake --build --preset windows-debug --target cs_app_tests`.

Expected: FAIL because `ProjectController.h` is missing.

- [ ] **Step 3: Implement the bounded recent registry**

Use:

```cpp
struct RecentProject final {
    std::filesystem::path path;
    creator::core::Utc lastOpenedAt;
};

class RecentProjectRegistry final {
public:
    explicit RecentProjectRegistry(std::filesystem::path filePath);
    [[nodiscard]] creator::core::Result<std::vector<RecentProject>> load() const;
    [[nodiscard]] creator::core::Result<void> remember(
        const std::filesystem::path&, const creator::core::Utc&);
    [[nodiscard]] const std::filesystem::path& filePath() const noexcept;
private:
    std::filesystem::path filePath_;
};
```

The JSON root is:

```json
{"version":1,"projects":[{"path":"<native-path-encoded-as-UTF-8>","lastOpenedAt":"2026-07-16T12:00:00Z"}]}
```

Deduplicate by native-path equality, put the newest first, keep 20, and use `QSaveFile` inside the
worker thread for atomic replacement. A missing registry returns an empty vector. Malformed content
returns `ParseFailure` from `load()`; the next explicit successful create/open backs the malformed
file up to `recent-projects.json.corrupt-<uuid>` and writes a new registry containing that project.
No project package is changed by this registry repair.

The production constructor obtains the file with:

```cpp
const QDir config{QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)};
const auto registryPath = config.filePath(QStringLiteral("recent-projects.json"));
```

Create the config directory on the worker before the first successful write. Convert that QString
to a native `std::filesystem::path` with the same Windows/POSIX helper used for project URLs.

- [ ] **Step 4: Implement ProjectWorker and ProjectController**

`ProjectWorker` is a `QObject` moved to a dedicated `QThread`. It owns the
`std::unique_ptr<IProjectPackageStore>` and `RecentProjectRegistry`; only its methods access them.
`ProjectController` queues those methods with functor `QMetaObject::invokeMethod`, so domain values
are captured by value and do not require Qt metatype registration. Worker completion signals use
only built-in queued types:

```cpp
signals:
    void openFinished(bool success, QVariantMap project, QVariantList recoveries,
                      QString errorMessage);
    void recoveryFinished(bool success, QVariantMap recovery, QString errorMessage);
    void recentScanFinished(QVariantList recentProjects, QVariantList recoveries,
                            QString errorMessage);
    void recordingCommandFinished(quint64 commandId, bool success, QString errorMessage);
```

`ProjectController` properties:

```cpp
Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
Q_PROPERTY(bool hasOpenProject READ hasOpenProject NOTIFY projectChanged)
Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
Q_PROPERTY(QUrl projectUrl READ projectUrl NOTIFY projectChanged)
Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
Q_PROPERTY(QVariantList recoveries READ recoveries NOTIFY recoveriesChanged)
Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
```

Invokables:

```cpp
Q_INVOKABLE void createProject(const QUrl& packageUrl, const QString& displayName);
Q_INVOKABLE void openProject(const QUrl& packageUrl);
Q_INVOKABLE void recoverSession(const QString& sessionId);
Q_INVOKABLE void leaveRecoveryForLater();
Q_INVOKABLE void refreshRecentProjects();

signals:
    void projectOpened();
    void recoveryRequired();
    void recoveryDeferred();
```

Convert local-file URLs to `std::filesystem::path` without ANSI conversion: use `toLocalFile()` and
`QString::toStdWString()` on Windows, UTF-8 on POSIX. Reject non-local URLs with
`InvalidArgument`. Set `busy=true` before queueing; reset only on the matching completion signal.
If a create path lacks `.cstudio` case-insensitively, append the suffix before queueing it.
Expose recovery maps with keys `sessionId`, `projectName`, `projectUrl`, `createdAt`,
`readySegments`, and `writingSegments`.

`leaveRecoveryForLater()` performs no store call, retains the recovery model, clears the pending
open-project selection, and emits `recoveryDeferred()` so Main returns Home.

`refreshRecentProjects()` loads the registry on the worker, opens each existing registered package
one at a time, and returns recent rows plus all recovery candidates. Queue it once from the
controller constructor so Home is populated at startup. Missing paths remain visible as unavailable
recent rows but are not opened and do not create recovery candidates. Add
`StartupScansRegisteredProjectsForRecoveries` using a temporary registry and fake store.

Connect `workerThread_.finished` to `worker_->deleteLater`; the controller destructor disconnects
callbacks, calls `workerThread_.quit()`, then `workerThread_.wait()`. This canonical Qt connection
destroys the worker and its store with the worker thread shutdown. No worker callback may target a
destroyed controller; all result connections carry the controller as QObject context.

- [ ] **Step 5: Run focused tests and commit**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests
& .\build\windows-debug\tests\cs_app_tests.exe --gtest_filter='ProjectControllerTest.*'
```

Expected: async create/open/recovery tests PASS and fake calls occur on a non-UI thread.

Commit:

```powershell
git add src/app/RecentProjectRegistry.* src/app/ProjectWorker.* src/app/ProjectController.* src/app/CMakeLists.txt tests/app/FakeProjectPackageStore.h tests/app/ProjectControllerTest.cpp tests/CMakeLists.txt
git commit -m "feat(app): add asynchronous project controller"
```

### Task 9: Sequence Recording Persistence Around Capture

**Files:**
- Create: `src/app/IRecordingPersistence.h`
- Modify: `src/app/ProjectController.h`
- Modify: `src/app/ProjectController.cpp`
- Modify: `src/app/StudioController.h`
- Modify: `src/app/StudioController.cpp`
- Modify: `tests/app/StudioControllerTest.cpp`
- Create: `tests/app/RecordingPersistenceFake.h`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Consumes: open project, asynchronous package-store operations, recorder/capture ports.
- Produces: persisted-before-recording start and committed-before-stopped UI behavior.

- [ ] **Step 1: Define the async persistence port**

```cpp
class IRecordingPersistence {
public:
    using Completion = std::function<void(creator::core::Result<void>)>;
    virtual ~IRecordingPersistence() = default;
    virtual void begin(const creator::domain::SessionId&, creator::core::TimestampNs,
                       Completion) = 0;
    virtual void complete(const creator::domain::RecordingSession&, Completion) = 0;
    virtual void abort(const creator::domain::SessionId&, std::string reason,
                       Completion) = 0;
protected:
    IRecordingPersistence() = default;
};
```

`ProjectController` implements this interface by queueing work on `ProjectWorker` against the
current package. Completion callbacks are always invoked on the controller/UI thread and exactly
once. No open project returns `InvalidState` asynchronously.

- [ ] **Step 2: Write failing sequencing tests**

`RecordingPersistenceFake` stores pending callbacks so tests decide when commits complete:

```cpp
TEST_F(StudioControllerTest, DoesNotStartRecorderBeforeSessionCommit) {
    controller_->startRecording();
    EXPECT_TRUE(controller_->busy());
    EXPECT_FALSE(controller_->isRecording());
    EXPECT_EQ(recorder_->stats().framesAccepted, 0u);

    persistence_->succeedBegin();
    EXPECT_TRUE(controller_->isRecording());
    EXPECT_FALSE(controller_->busy());
}

TEST_F(StudioControllerTest, BeginFailureNeverStartsCapture) {
    controller_->startRecording();
    persistence_->failBegin(AppError{ErrorCode::IoFailure, "database full"});
    EXPECT_FALSE(controller_->isRecording());
    EXPECT_EQ(controller_->statusMessage(), QStringLiteral("database full"));
}

TEST_F(StudioControllerTest, StopShowsStoppedOnlyAfterDatabaseCommit) {
    controller_->startRecording();
    persistence_->succeedBegin();
    controller_->onCaptureTick();
    controller_->stopRecording();
    EXPECT_TRUE(controller_->busy());
    EXPECT_NE(controller_->statusMessage(), QStringLiteral("Stopped"));

    persistence_->succeedComplete();
    EXPECT_FALSE(controller_->busy());
    EXPECT_EQ(controller_->statusMessage(), QStringLiteral("Stopped"));
}
```

- [ ] **Step 3: Run and verify old synchronous behavior fails**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests
& .\build\windows-debug\tests\cs_app_tests.exe --gtest_filter='StudioControllerTest.*'
```

Expected: new sequencing tests FAIL because current controller starts immediately and has no busy
property.

- [ ] **Step 4: Add explicit controller operation state**

Add:

```cpp
enum class RecordingOperationState { Idle, Preparing, Recording, Finalizing };
Q_PROPERTY(bool busy READ isBusy NOTIFY operationStateChanged)
Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
```

`isBusy()` is true for Preparing/Finalizing; `isRecording()` is true only for Recording.

Start flow:

1. create and retain session ID;
2. set Preparing and status `Preparing recording`;
3. call persistence `begin`;
4. on success start recorder then source, unwind/abort on either failure;
5. only after both start set Recording and status `Recording`.

Stop flow:

1. stop timer/input and set Finalizing;
2. stop recorder and source;
3. retain returned session in `pendingFinalSession_`;
4. call persistence `complete`;
5. only on success publish segment count/duration and status `Stopped`;
6. on persistence failure show the exact error and leave DB state recoverable.

The destructor releases source/recorder resources but deliberately does not call persistence
complete; an app exit during Recording must leave `RECORDING` in DB for recovery.

- [ ] **Step 5: Run controller regressions and commit**

Run:

```powershell
cmake --build --preset windows-debug --target cs_app_tests
& .\build\windows-debug\tests\cs_app_tests.exe --gtest_filter='StudioControllerTest.*:ProjectControllerTest.*'
```

Expected: all app tests PASS, including prior source-unwind and duration behavior.

Commit:

```powershell
git add src/app/IRecordingPersistence.h src/app/ProjectController.* src/app/StudioController.* src/app/CMakeLists.txt tests/app/RecordingPersistenceFake.h tests/app/StudioControllerTest.cpp
git commit -m "feat(app): persist recording lifecycle before UI success"
```

### Task 10: Connect Home, Recent Projects, and Recovery UI

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `qml/HomePage.qml`
- Modify: `qml/Main.qml`
- Create: `qml/RecoveryPage.qml`
- Modify: `src/main.cpp`
- Create: `tests/app/QmlSmokeTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `projectController` properties/invokables and `studioController` recording state.
- Produces: visible create/open/recent/recover/later flow.

- [ ] **Step 1: Add a failing QML smoke test**

Create a separate `cs_qml_tests` binary with `QGuiApplication`, `Qt6::Quick`, `Qt6::Test`, and
`GTest::gtest`. Set CTest environment `QT_QPA_PLATFORM=offscreen` and compile definition
`CS_QML_SOURCE_DIR="${PROJECT_SOURCE_DIR}/qml"`.

The test creates a fake QObject exposing every property/invokable used by `RecoveryPage.qml`, sets it
as `projectController`, and loads the source file:

```cpp
class FakeProjectController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy CONSTANT)
    Q_PROPERTY(QVariantList recoveries READ recoveries CONSTANT)
    Q_PROPERTY(QString statusMessage READ statusMessage CONSTANT)
public:
    using QObject::QObject;
    [[nodiscard]] bool busy() const noexcept { return false; }
    [[nodiscard]] QVariantList recoveries() const {
        return {QVariantMap{{"sessionId", "session-1"},
                            {"projectName", "강의"},
                            {"projectUrl", QUrl::fromLocalFile("C:/fixture.cstudio")},
                            {"createdAt", "2026-07-16T12:00:00Z"},
                            {"readySegments", 1},
                            {"writingSegments", 1}}};
    }
    [[nodiscard]] QString statusMessage() const { return {}; }
    Q_INVOKABLE void recoverSession(const QString&) {}
    Q_INVOKABLE void leaveRecoveryForLater() {}
};

TEST(QmlSmokeTest, RecoveryPageLoadsWithProjectControllerContract) {
    QQmlEngine engine;
    FakeProjectController controller;
    engine.rootContext()->setContextProperty("projectController", &controller);
    QQmlComponent component{&engine,
        QUrl::fromLocalFile(QString::fromUtf8(CS_QML_SOURCE_DIR "/RecoveryPage.qml"))};
    std::unique_ptr<QObject> object{component.create()};
    ASSERT_NE(object, nullptr) << component.errorString().toStdString();
}

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "QmlSmokeTest.moc"
```

- [ ] **Step 2: Run and verify the page is missing**

Run `cmake --build --preset windows-debug --target cs_qml_tests`.

Expected: FAIL because `qml/RecoveryPage.qml` does not exist.

- [ ] **Step 3: Compose production controllers**

In `main.cpp`, construct in this lifetime order:

```cpp
auto packageStore = std::make_unique<creator::project_store::ProjectPackageStore>();
creator::app::ProjectController projectController{std::move(packageStore), &app};
creator::app::StudioController studioController{
    std::make_unique<creator::fakes::FakeCaptureSource>(
        creator::domain::SourceId::create("screen-1").value(), "Test Pattern"),
    std::make_unique<creator::fakes::FakeRecorder>(),
    &projectController, &app};
```

Expose both context properties. The declaration order above guarantees reverse destruction:
`StudioController` is destroyed first, then `ProjectController`, then `QGuiApplication`.

- [ ] **Step 4: Implement the Home production flow**

Import `QtQuick.Dialogs`. Home contains:

- a display-name `TextField`;
- `FileDialog` in save mode with `Creator Studio Project (*.cstudio)` for new recording;
- `FolderDialog` for opening an existing package folder;
- a busy indicator and disabled create/open controls while `projectController.busy`;
- repeaters for `recentProjects` and `recoveries`.

On create dialog acceptance call:

```qml
projectController.createProject(newProjectDialog.selectedFile, projectName.text)
```

On recent click call `openProject(modelData.projectUrl)`. Never navigate directly to Studio before
`projectOpened`.

- [ ] **Step 5: Implement RecoveryPage and navigation**

The page shows project name/path, created time, READY count, WRITING count, plus exactly two actions:

```qml
Button {
    text: qsTr("복구")
    enabled: !projectController.busy && recoveryList.currentIndex >= 0
    onClicked: projectController.recoverSession(
        projectController.recoveries[recoveryList.currentIndex].sessionId)
}

Button {
    text: qsTr("나중에")
    enabled: !projectController.busy
    onClicked: projectController.leaveRecoveryForLater()
}
```

There is no discard/delete action. `Main.qml` listens to `projectOpened` and `recoveryRequired`:

```qml
Connections {
    target: projectController
    function onProjectOpened() { window.currentPage = "Studio" }
    function onRecoveryRequired() { window.currentPage = "Recovery" }
    function onRecoveryDeferred() { window.currentPage = "Home" }
}
```

Add `Recovery` to the internal stack but not the normal top navigation buttons. Disable Record while
`studioController.busy` or no project is open.

- [ ] **Step 6: Run QML and app tests, then commit**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target cs_qml_tests cs_app_tests creator_studio
& .\build\windows-debug\tests\cs_qml_tests.exe
& .\build\windows-debug\tests\cs_app_tests.exe
```

Expected: both test binaries PASS and the application links with the new QML page.

Commit:

```powershell
git add CMakeLists.txt qml/HomePage.qml qml/Main.qml qml/RecoveryPage.qml src/main.cpp tests/app/QmlSmokeTest.cpp tests/CMakeLists.txt
git commit -m "feat(ui): add project creation and recovery flow"
```

### Task 11: Full Verification, Documentation, and R0-02 Closeout

**Files:**
- Modify: `README.md`
- Modify: `tests/README.md`
- Modify: `.superpowers/sdd/progress.md`

**Interfaces:**
- Consumes: all R0-02 code and acceptance tests.
- Produces: verified branch state and an honest R0-03 handoff.

- [ ] **Step 1: Run formatting/static repository checks**

Run:

```powershell
git diff --check
rg -n "TO[D]O|TB[D]|EXPECT_TRUE\(true\)|ASSERT_TRUE\(true\)|sleep_for|QThread::sleep" src tests qml
```

Expected: `git diff --check` exits 0; the search finds no new placeholders, tautological tests, or
sleep-based synchronization. Existing legitimate documentation occurrences must be reviewed and
listed in the progress note rather than silently ignored.

- [ ] **Step 2: Configure/build/test Debug from the documented toolchain**

Run:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: configure and `/WX` build succeed; every discovered Qt-free, app, QML, and crash recovery
test passes.

- [ ] **Step 3: Configure/build/test Release**

Run:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

Expected: all Release tests PASS. Any dependency configuration warning introduced by R0-02 is fixed
before continuing.

- [ ] **Step 4: Perform an application smoke and manual force-close check**

Run the app with Qt DLLs on PATH:

```powershell
& .\build\windows-debug\creator_studio.exe
```

Verify visibly:

1. create a Korean-named `.cstudio` project;
2. confirm Home opens Studio only after package creation;
3. start fake recording and stop it; confirm final status appears after persistence;
4. start another recording, terminate the process from Task Manager, restart, and open the package;
5. confirm Recovery appears, Recover succeeds, and no project files disappear;
6. reopen once more and confirm the candidate is gone.

Record the exact package path and before/after file listing in `.superpowers/sdd/progress.md`. Remove
only the manually created smoke package after checking its resolved path is inside the chosen test
directory; do not remove any user package.

- [ ] **Step 5: Update documentation with only verified claims**

README must state:

- R0-02 package creation/open/recovery is connected to the app;
- SQLite/schema validator versions and the local-filesystem requirement;
- exact Debug/Release test totals from this run;
- macOS remains unverified unless CI actually ran;
- actual screen capture/media bytes still begin at R0-03/R0-05.

`tests/README.md` lists new suites and the `_Exit` helper. Progress notes include commits, commands,
results, known limitations, and R0-03 timestamp carry-forward from the prior handoff.

- [ ] **Step 6: Review the branch diff and commit closeout docs**

Run:

```powershell
git status --short
git diff $(git merge-base main HEAD) HEAD --stat
git log --oneline main..HEAD
```

Verify the three existing untracked screenshots remain uncommitted unless the user explicitly asks
to add them. Commit:

```powershell
git add README.md tests/README.md .superpowers/sdd/progress.md
git commit -m "docs(r0-02): record package recovery verification"
```

- [ ] **Step 7: Final verification after the documentation commit**

Run:

```powershell
cmake --build --preset windows-debug
ctest --preset windows-debug
git status --short
```

Expected: build/test remain green; status contains only the pre-existing untracked screenshots.
