# R2-01 Cursor and Click Telemetry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record source-separated cursor coordinates and exact mouse-button transitions into durable, recoverable project telemetry on the same monotonic timebase as a Studio take.

**Architecture:** Add a Qt-free `cs_telemetry` value/queue layer, SQLite migration 005 and a durable NDJSON store, a bounded cursor recorder, and an application-owned Windows Raw Input adapter. Replace the single recording-preparation callback with ordered lifecycle participants so Studio and cursor telemetry prepare, abort, and complete without hidden coupling.

**Tech Stack:** C++20, CMake/Ninja, GoogleTest, SQLite 3.53.3, nlohmann/json 3.11.3, Qt 6.8.3 application boundary, Win32 User32 Raw Input on Windows 11 x64.

## Global Constraints

- R2-01 captures cursor coordinates and mouse-button transitions only; no keystrokes, text, clipboard, target UI element, window title, application name, device serial, or user identity.
- No `WH_MOUSE_LL`, injected DLL, driver, administrator privilege, subprocess, web runtime, cloud service, or new third-party dependency.
- Windows Raw Input registration is owned once by the executable composition root and is active only during an accepted recording session.
- Move events are coalesced to at most 120 Hz; button transitions are never replaced by movement and transition overflow fails telemetry visibly.
- Cursor telemetry failure never deletes, aborts, or corrupts valid screen, camera, microphone, or system-audio media.
- All persistent paths are package-relative below `telemetry/cursor`; reparse, hard-link, traversal, and identity-change defenses remain mandatory.
- C++ gates remain `/W4 /permissive- /utf-8 /WX`; non-application libraries stay Qt-free.
- Windows 11 x64 is the physical R2-01 target. macOS remains explicitly unavailable until native permission behavior is designed and tested.
- User-owned untracked PNG files in the repository root must not be staged, moved, or deleted.

---

### Task 1: Qt-free cursor telemetry values and canonical NDJSON records

**Files:**
- Create: `src/telemetry/CMakeLists.txt`
- Create: `src/telemetry/CursorTelemetryTypes.h`
- Create: `src/telemetry/CursorTelemetryTypes.cpp`
- Create: `src/telemetry/CursorTelemetryJson.h`
- Create: `src/telemetry/CursorTelemetryJson.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/telemetry/CursorTelemetryTypesTest.cpp`
- Create: `tests/telemetry/CursorTelemetryJsonTest.cpp`

**Interfaces:**
- Consumes: `creator::core::TimestampNs`, `creator::domain::SessionId`, and `creator::domain::SourceId`.
- Produces: `CursorTargetGeometry::create`, `CursorTelemetryEvent::createMove`, `CursorTelemetryEvent::createButton`, `CursorStreamHeader::create`, `CursorStreamFooter::create`, `encodeCursorRecord`, and `decodeCursorRecord`.

- [ ] **Step 1: Write failing invariant and round-trip tests**

```cpp
TEST(CursorTelemetryTypesTest, RequiresCanonicalMoveAndButtonFields) {
    auto geometry = CursorTargetGeometry::create(1, -1920, 0, 1920, 1080,
                                                  1920, 1080, "PMv2");
    ASSERT_TRUE(geometry.hasValue());
    auto move = CursorTelemetryEvent::createMove(
        1, TimestampNs{DurationNs{20}}, -10, 50, false, std::nullopt,
        std::nullopt, geometry.value().generation());
    ASSERT_TRUE(move.hasValue());
    EXPECT_FALSE(move.value().button().has_value());
    EXPECT_FALSE(CursorTelemetryEvent::createButton(
        2, TimestampNs{DurationNs{19}}, CursorEventType::ButtonDown,
        std::nullopt, 10, 20, true, 100, 200, 1).hasValue());
}

TEST(CursorTelemetryJsonTest, RoundTripsCanonicalUnicodeHeaderGeometryEventFooter) {
    const auto records = canonicalFixtureRecords();
    for (const auto& record : records) {
        auto decoded = decodeCursorRecord(encodeCursorRecord(record).value());
        ASSERT_TRUE(decoded.hasValue());
        EXPECT_EQ(decoded.value(), record);
    }
}
```

- [ ] **Step 2: Run the focused tests and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=CursorTelemetryTypesTest.*:CursorTelemetryJsonTest.*`

Expected: compile failure because `telemetry/CursorTelemetryTypes.h` does not exist.

- [ ] **Step 3: Implement validated values and strict canonical codec**

```cpp
enum class CursorEventType { Move, ButtonDown, ButtonUp };
enum class CursorButton { Left, Right, Middle, X1, X2 };

class CursorTelemetryEvent final {
public:
    static core::Result<CursorTelemetryEvent> createMove(
        std::uint64_t sequence, core::TimestampNs timestamp,
        std::int32_t desktopX, std::int32_t desktopY, bool insideTarget,
        std::optional<std::uint32_t> targetXPartsPerMillion,
        std::optional<std::uint32_t> targetYPartsPerMillion,
        std::uint64_t geometryGeneration);
    static core::Result<CursorTelemetryEvent> createButton(
        std::uint64_t sequence, core::TimestampNs timestamp,
        CursorEventType type, std::optional<CursorButton> button,
        std::int32_t desktopX, std::int32_t desktopY, bool insideTarget,
        std::optional<std::uint32_t> targetXPartsPerMillion,
        std::optional<std::uint32_t> targetYPartsPerMillion,
        std::uint64_t geometryGeneration);
    // Immutable accessors and value equality follow existing domain style.
};

using CursorRecord = std::variant<CursorStreamHeader, CursorTargetGeometry,
                                  CursorTelemetryEvent, CursorStreamFooter>;
core::Result<std::string> encodeCursorRecord(const CursorRecord& record);
core::Result<CursorRecord> decodeCursorRecord(std::string_view line);
```

Use `nlohmann::ordered_json`, reject unknown/missing keys, cap one line at 4 KiB,
write UTF-8 without BOM, and serialize all times/coordinates as checked integers.

- [ ] **Step 4: Run telemetry and complete Qt-free gates**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=CursorTelemetryTypesTest.*:CursorTelemetryJsonTest.*`

Expected: all discovered tests pass.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/telemetry tests/CMakeLists.txt tests/telemetry
git commit -m "feat(r2): add cursor telemetry value model"
```

---

### Task 2: Bounded move coalescing and transition queue

**Files:**
- Create: `src/telemetry/CursorEventQueue.h`
- Create: `src/telemetry/CursorEventQueue.cpp`
- Modify: `src/telemetry/CMakeLists.txt`
- Create: `tests/telemetry/CursorEventQueueTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: validated `CursorTelemetryEvent` from Task 1.
- Produces: `CursorEventQueue::pushMove`, `pushTransition`, `tryPop`, `close`, and `snapshot`.

- [ ] **Step 1: Write failing ordering, coalescing, overflow, and concurrency tests**

```cpp
TEST(CursorEventQueueTest, CoalescesMovesButNeverReplacesButtonTransitions) {
    CursorEventQueue queue{2};
    EXPECT_EQ(queue.pushMove(moveAt(1, 10)), CursorPushResult::Accepted);
    EXPECT_EQ(queue.pushMove(moveAt(2, 11)), CursorPushResult::Coalesced);
    EXPECT_EQ(queue.pushTransition(downAt(3, 12)), CursorPushResult::Accepted);
    EXPECT_EQ(queue.pushTransition(upAt(4, 13)), CursorPushResult::Accepted);
    ASSERT_EQ(drain(queue), (std::vector{moveAt(2, 11), downAt(3, 12), upAt(4, 13)}));
    EXPECT_EQ(queue.snapshot().coalescedMoves, 1U);
}

TEST(CursorEventQueueTest, TransitionOverflowIsTerminalAndMemoryStaysBounded) {
    CursorEventQueue queue{1};
    EXPECT_EQ(queue.pushTransition(downAt(1, 1)), CursorPushResult::Accepted);
    EXPECT_EQ(queue.pushTransition(upAt(2, 2)), CursorPushResult::Overflow);
    EXPECT_TRUE(queue.snapshot().terminalError.has_value());
}
```

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=CursorEventQueueTest.*`

Expected: compile failure because `CursorEventQueue` is undefined.

- [ ] **Step 3: Implement the bounded queue**

```cpp
struct CursorEventQueueSnapshot final {
    std::uint64_t acceptedMoves{};
    std::uint64_t coalescedMoves{};
    std::uint64_t acceptedTransitions{};
    std::size_t queuedTransitions{};
    bool closed{false};
    std::optional<core::AppError> terminalError;
};

class CursorEventQueue final {
public:
    explicit CursorEventQueue(std::size_t transitionCapacity = 4096);
    CursorPushResult pushMove(CursorTelemetryEvent event) noexcept;
    CursorPushResult pushTransition(CursorTelemetryEvent event) noexcept;
    std::optional<CursorTelemetryEvent> tryPop() noexcept;
    void close() noexcept;
    CursorEventQueueSnapshot snapshot() const noexcept;
};
```

Use one mutex and fixed-capacity transition storage. Keep only the latest pending
move; choose the lower sequence between it and the transition front. No producer
waits for the consumer or performs allocation after construction.

- [ ] **Step 4: Run focused tests 20 times**

Run: `1..20 | % { build/windows-debug/cs_tests.exe --gtest_filter=CursorEventQueueTest.*; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: every repetition passes with no timeout or unbounded allocation.

- [ ] **Step 5: Commit**

```powershell
git add src/telemetry tests/telemetry tests/CMakeLists.txt
git commit -m "feat(r2): bound cursor event delivery"
```

---

### Task 3: Migration 005 and telemetry stream metadata store

**Files:**
- Create: `src/project_store/migrations/005_cursor_telemetry.sql`
- Create: `cmake/Migration005.h.in`
- Create: `src/project_store/ICursorTelemetryStore.h`
- Create: `src/project_store/SqliteCursorTelemetryStore.h`
- Create: `src/project_store/SqliteCursorTelemetryStore.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/project_store/CMakeLists.txt`
- Modify: `src/project_store/MigrationRunner.h`
- Modify: `src/project_store/MigrationRunner.cpp`
- Modify: `src/domain/Identifiers.h`
- Modify: `tests/domain/IdentifiersTest.cpp`
- Modify: `tests/project_store/MigrationRunnerTest.cpp`
- Create: `tests/project_store/SqliteCursorTelemetryStoreTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: project/session/source identities and package-relative paths.
- Produces: `ICursorTelemetryStore::begin`, `updateWritingMetadata`, `markReady`, `markFailed`, `load`, and `listRecoverable`.

- [ ] **Step 1: Write failing version-5 migration and store tests**

```cpp
TEST(MigrationRunnerTest, AppliesCursorTelemetryMigrationFiveExactlyOnce) {
    auto connection = openMemoryDatabase();
    ASSERT_TRUE(MigrationRunner::apply(connection).hasValue());
    EXPECT_EQ(userVersion(connection), 5);
    EXPECT_EQ(appliedMigrationCount(connection), 5);
    EXPECT_TRUE(tableExists(connection, "telemetry_streams"));
}

TEST_F(SqliteCursorTelemetryStoreTest, EnforcesOneImmutableCursorStreamPerSessionSource) {
    ASSERT_TRUE(store_.begin(startFixture()).hasValue());
    EXPECT_TRUE(store_.begin(startFixture()).hasValue());
    auto conflicting = startFixture();
    conflicting.finalPath = "telemetry/cursor/other.ndjson";
    EXPECT_FALSE(store_.begin(conflicting).hasValue());
}
```

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=MigrationRunnerTest.*:SqliteCursorTelemetryStoreTest.*`

Expected: migration version remains 4 and the store type is missing.

- [ ] **Step 3: Add checksum-pinned migration 005**

```sql
CREATE TABLE telemetry_streams (
    stream_id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL REFERENCES projects(project_id) ON DELETE CASCADE,
    session_id TEXT NOT NULL REFERENCES recording_sessions(session_id) ON DELETE CASCADE,
    source_id TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind = 'CURSOR_V1'),
    final_path TEXT NOT NULL,
    part_path TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('WRITING','READY','FAILED')),
    started_at_ns INTEGER NOT NULL CHECK(started_at_ns >= 0),
    ended_at_ns INTEGER CHECK(ended_at_ns IS NULL OR ended_at_ns >= started_at_ns),
    event_count INTEGER CHECK(event_count IS NULL OR event_count >= 0),
    byte_size INTEGER CHECK(byte_size IS NULL OR byte_size >= 0),
    sha256 TEXT CHECK(sha256 IS NULL OR length(sha256) = 64),
    failure_message TEXT,
    CHECK(state != 'READY' OR
          (ended_at_ns IS NOT NULL AND event_count IS NOT NULL AND
           byte_size IS NOT NULL AND sha256 IS NOT NULL)),
    CHECK(state != 'FAILED' OR failure_message IS NOT NULL),
    UNIQUE(session_id, source_id, kind),
    UNIQUE(project_id, final_path),
    UNIQUE(project_id, part_path)
);
CREATE INDEX telemetry_streams_recovery
ON telemetry_streams(project_id, state, session_id);
CREATE TRIGGER telemetry_stream_project_identity
BEFORE INSERT ON telemetry_streams
WHEN NEW.project_id !=
     (SELECT project_id FROM recording_sessions WHERE session_id = NEW.session_id)
BEGIN
    SELECT RAISE(ABORT, 'telemetry stream project identity mismatch');
END;
CREATE TRIGGER telemetry_stream_terminal_immutable
BEFORE UPDATE ON telemetry_streams
WHEN OLD.state IN ('READY','FAILED')
BEGIN
    SELECT RAISE(ABORT, 'terminal telemetry stream is immutable');
END;
CREATE TRIGGER telemetry_stream_state_transition
BEFORE UPDATE OF state ON telemetry_streams
WHEN OLD.state = 'WRITING' AND NEW.state NOT IN ('READY','FAILED')
BEGIN
    SELECT RAISE(ABORT, 'invalid telemetry stream state transition');
END;
PRAGMA user_version = 5;
```

Embed its SHA-256 exactly like migrations 001-004 and set
`MigrationRunner::kLatestVersion = 5`. Add
`TelemetryStreamIdTag`/`TelemetryStreamId` beside `RenderJobIdTag` and cover its
validation in `IdentifiersTest.cpp`.

- [ ] **Step 4: Implement the port and SQLite adapter**

```cpp
class ICursorTelemetryStore {
public:
    virtual core::Result<CursorTelemetryStream> begin(CursorTelemetryStart start) = 0;
    virtual core::Result<void> updateWritingMetadata(
        const domain::TelemetryStreamId&, CursorTelemetryWritingMetadata) = 0;
    virtual core::Result<void> markReady(
        const domain::TelemetryStreamId&, CursorTelemetryCompletion) = 0;
    virtual core::Result<void> markFailed(
        const domain::TelemetryStreamId&, core::TimestampNs, std::string message) = 0;
    virtual core::Result<CursorTelemetryStream> load(
        const domain::TelemetryStreamId&) = 0;
    virtual core::Result<std::vector<CursorTelemetryStream>> listRecoverable(
        const domain::ProjectId&) = 0;
    virtual ~ICursorTelemetryStore() = default;
};
```

Use immediate SQLite transactions, checked integer conversion, idempotent exact
retries, and identity-conflict errors consistent with `SqliteStudioStore`.

- [ ] **Step 5: Run migration/store tests and the complete store suite**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=MigrationRunnerTest.*:SqliteCursorTelemetryStoreTest.*:SqliteTimelineStoreTest.*:SqliteStudioStoreTest.*`

Expected: all tests pass and migration checksums remain stable.

- [ ] **Step 6: Commit**

```powershell
git add CMakeLists.txt cmake/Migration005.h.in src/domain/Identifiers.h src/project_store tests/domain/IdentifiersTest.cpp tests/project_store tests/CMakeLists.txt
git commit -m "feat(r2): persist cursor telemetry streams"
```

---

### Task 4: Durable NDJSON writer, finalization, and crash recovery

**Files:**
- Create: `src/project_store/CursorTelemetryFile.h`
- Create: `src/project_store/CursorTelemetryFile.cpp`
- Create: `src/project_store/CursorTelemetryRecovery.h`
- Create: `src/project_store/CursorTelemetryRecovery.cpp`
- Modify: `src/project_store/CMakeLists.txt`
- Create: `tests/project_store/CursorTelemetryFileTest.cpp`
- Create: `tests/project_store/CursorTelemetryRecoveryTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 records and Task 3 stream rows.
- Produces: `CursorTelemetryFile::create`, `append`, `finalize`, `abort`, plus `recoverCursorTelemetryStream`.

- [ ] **Step 1: Write failure-boundary and recovery tests**

```cpp
TEST_F(CursorTelemetryFileTest, FinalizePublishesOneHashedCanonicalFile) {
    auto writer = CursorTelemetryFile::create(package_, stream_, header_).value();
    ASSERT_TRUE(writer.append(geometry_).hasValue());
    ASSERT_TRUE(writer.append(moveAt(1, 10)).hasValue());
    auto result = writer.finalize(footerAt(1, 10));
    ASSERT_TRUE(result.hasValue());
    EXPECT_FALSE(fs::exists(partPath_));
    EXPECT_TRUE(fs::is_regular_file(finalPath_));
    EXPECT_EQ(sha256(finalPath_), result.value().sha256);
}

TEST_F(CursorTelemetryRecoveryTest, TruncatesOnlyIncompleteTailAndClosesValidPart) {
    writeBytes(partPath_, headerLine() + geometryLine() + eventLine() + "{\"seq\":");
    auto recovered = recoverCursorTelemetryStream(package_, row_, store_);
    ASSERT_TRUE(recovered.hasValue());
    EXPECT_EQ(recovered.value().eventCount, 1U);
    EXPECT_TRUE(parseComplete(finalPath_).hasValue());
}
```

Add table-driven failures after create, header, geometry, event, flush, metadata
update, rename, and ready commit. Assert unrelated and READY files are byte-identical.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=CursorTelemetryFileTest.*:CursorTelemetryRecoveryTest.*`

Expected: compile failure because the file/recovery classes do not exist.

- [ ] **Step 3: Implement streaming durable output**

```cpp
class CursorTelemetryFile final {
public:
    static core::Result<CursorTelemetryFile> create(
        std::filesystem::path packageRoot, CursorTelemetryStream stream,
        telemetry::CursorStreamHeader header);
    core::Result<void> append(const telemetry::CursorTargetGeometry& geometry);
    core::Result<void> append(const telemetry::CursorTelemetryEvent& event);
    core::Result<CursorTelemetryFileResult> finalize(
        const telemetry::CursorStreamFooter& footer);
    core::Result<void> abort(std::string message);
    ~CursorTelemetryFile();
};
```

Open with exclusive create and no-follow/reparse checks, append full lines,
periodically flush at no more than ten times per second, close and hash before
same-directory durable rename. Destruction of an unfinalized writer closes the
handle but never marks ready.

- [ ] **Step 4: Implement exact recovery**

Parse at most the documented line and file limits, require one header and current
geometry before events, contiguous sequences, monotonic times, and valid button
state. Recovered footer status is `recovered_after_interruption`. Quarantine by
identity-safe rename on any invalid input; mark the corresponding row failed.

- [ ] **Step 5: Run focused tests 10 times and full package recovery tests**

Run: `1..10 | % { build/windows-debug/cs_tests.exe --gtest_filter=CursorTelemetryFileTest.*:CursorTelemetryRecoveryTest.*:RecoveryTest.*; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: all repetitions pass with no stale handles or `.part` leakage.

- [ ] **Step 6: Commit**

```powershell
git add src/project_store tests/project_store tests/CMakeLists.txt
git commit -m "feat(r2): recover durable cursor telemetry files"
```

---

### Task 5: Bounded cursor telemetry recorder worker

**Files:**
- Create: `src/telemetry/ICursorTelemetrySource.h`
- Create: `src/telemetry/ICursorTelemetrySink.h`
- Create: `src/recorder/CursorTelemetryRecorder.h`
- Create: `src/recorder/CursorTelemetryRecorder.cpp`
- Modify: `src/telemetry/CMakeLists.txt`
- Modify: `src/recorder/CMakeLists.txt`
- Create: `tests/recorder/CursorTelemetryRecorderTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 queue and Task 4 durable writer.
- Produces: `CursorTelemetryRecorder::start`, `accept`, `updateGeometry`, `stop`, `abort`, and `snapshot`.

- [ ] **Step 1: Write lifecycle, 120 Hz, timestamp, and overflow tests**

```cpp
TEST(CursorTelemetryRecorderTest, CoalescesMovesAt120HzAndPreservesEveryClick) {
    auto fixture = recorderFixture();
    ASSERT_TRUE(fixture.recorder.start(fixture.start).hasValue());
    for (int ns = 0; ns < 8'000'000; ns += 100'000)
        fixture.recorder.accept(nativeMoveAt(ns));
    fixture.recorder.accept(nativeButtonAt(4'000'000, CursorButton::Left, true));
    fixture.recorder.accept(nativeButtonAt(5'000'000, CursorButton::Left, false));
    auto result = fixture.recorder.stop(TimestampNs{DurationNs{10'000'000}});
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().buttonTransitionCount, 2U);
    EXPECT_GT(result.value().coalescedMoveCount, 0U);
}
```

Also prove a backward native timestamp clamps and counts correction, producer
calls return without waiting for blocked file I/O, telemetry overflow fails only
the telemetry result, and destruction joins the worker.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_tests.exe --gtest_filter=CursorTelemetryRecorderTest.*`

Expected: compile failure because `CursorTelemetryRecorder` is missing.

- [ ] **Step 3: Implement the recorder**

```cpp
struct CursorTelemetryRecorderSnapshot final {
    bool running{false};
    std::uint64_t eventCount{};
    std::uint64_t coalescedMoves{};
    std::uint64_t timestampCorrections{};
    std::optional<core::AppError> terminalError;
};

class CursorTelemetryRecorder final : public telemetry::ICursorTelemetrySink {
public:
    core::Result<void> start(CursorTelemetryRecorderStart start);
    void accept(telemetry::NativeCursorSample sample) noexcept override;
    core::Result<void> updateGeometry(telemetry::CursorTargetGeometry geometry);
    core::Result<CursorTelemetryRecorderResult> stop(core::TimestampNs stoppedAt);
    void abort(std::string message) noexcept;
    CursorTelemetryRecorderSnapshot snapshot() const noexcept;
    ~CursorTelemetryRecorder();
};
```

One worker owns the file writer. Use the existing capture timestamp mapper style,
fixed queue storage, a condition variable only on the consumer side, and explicit
terminal result publication.

- [ ] **Step 4: Run focused and thread-lifetime tests**

Run: `1..20 | % { build/windows-debug/cs_tests.exe --gtest_filter=CursorTelemetryRecorderTest.*; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: 20/20, no hang and no thread-destruction diagnostic.

- [ ] **Step 5: Commit**

```powershell
git add src/telemetry src/recorder tests/recorder tests/CMakeLists.txt
git commit -m "feat(r2): record bounded cursor telemetry"
```

---

### Task 6: Windows Raw Input source with injected native seam

**Files:**
- Create: `src/telemetry/windows/IWindowsRawInputApi.h`
- Create: `src/telemetry/windows/WindowsRawInputApi.h`
- Create: `src/telemetry/windows/WindowsRawInputApi.cpp`
- Create: `src/telemetry/windows/WindowsRawMouseSource.h`
- Create: `src/telemetry/windows/WindowsRawMouseSource.cpp`
- Create: `src/telemetry/windows/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/telemetry/WindowsRawMouseSourceTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ICursorTelemetrySink`, one executable-owned message thread, and injected User32 calls.
- Produces: production `WindowsRawMouseSource` and explicit unavailable adapter on non-Windows platforms.

- [ ] **Step 1: Write parser, registration, position, and cleanup tests**

```cpp
TEST(WindowsRawMouseSourceTest, MapsEveryRawMouseButtonFlagWithoutKeyboardData) {
    FakeWindowsRawInputApi api;
    CollectingCursorSink sink;
    WindowsRawMouseSource source{api};
    ASSERT_TRUE(source.start(startConfig(), sink).hasValue());
    api.deliver(mouseInput(RI_MOUSE_LEFT_BUTTON_DOWN |
                           RI_MOUSE_XBUTTON1_DOWN));
    ASSERT_EQ(sink.samples().size(), 2U);
    EXPECT_EQ(sink.samples()[0].button, CursorButton::Left);
    EXPECT_EQ(sink.samples()[1].button, CursorButton::X1);
    EXPECT_EQ(api.keyboardRegistrations(), 0U);
}

TEST(WindowsRawMouseSourceTest, StopUnregistersAndDestroysWindowExactlyOnce) {
    FakeWindowsRawInputApi api;
    WindowsRawMouseSource source{api};
    ASSERT_TRUE(source.start(startConfig(), sink_).hasValue());
    ASSERT_TRUE(source.stop().hasValue());
    EXPECT_EQ(api.removeRegistrations(), 1U);
    EXPECT_EQ(api.destroyWindowCalls(), 1U);
}
```

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_windows_telemetry_tests.exe --gtest_filter=WindowsRawMouseSourceTest.*`

Expected: target or source files are missing.

- [ ] **Step 3: Implement the seam and message-thread adapter**

```cpp
class IWindowsRawInputApi {
public:
    virtual core::Result<HWND> createMessageWindow(WNDPROC procedure,
                                                   void* context) = 0;
    virtual core::Result<void> registerMouse(HWND target) = 0;
    virtual core::Result<void> unregisterMouse() = 0;
    virtual core::Result<RAWMOUSE> readMouse(HRAWINPUT input) = 0;
    virtual core::Result<POINT> physicalCursorPosition() = 0;
    virtual ~IWindowsRawInputApi() = default;
};
```

Register usage page `0x01`, usage `0x02`, `RIDEV_INPUTSINK | RIDEV_DEVNOTIFY`.
Handle only `RIM_TYPEMOUSE`; translate all five button down/up flags, use
`GetPhysicalCursorPos`, and post samples to the sink. Never register keyboard
usage. Stop posts a private shutdown message, unregisters with `RIDEV_REMOVE`
and null target, destroys the window, joins the thread, and is idempotent.

- [ ] **Step 4: Run native tests 20 times and registration smoke**

Run: `1..20 | % { build/windows-debug/cs_windows_telemetry_tests.exe; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: every parser/lifecycle test passes; a separate smoke test registers and
unregisters without generating or moving real user input.

- [ ] **Step 5: Commit**

```powershell
git add CMakeLists.txt src/telemetry/windows tests/telemetry tests/CMakeLists.txt
git commit -m "feat(windows): capture raw cursor telemetry"
```

---

### Task 7: Ordered recording lifecycle participants and cursor binding

**Files:**
- Create: `src/app/IRecordingLifecycleParticipant.h`
- Create: `src/app/RecordingLifecycleCoordinator.h`
- Create: `src/app/RecordingLifecycleCoordinator.cpp`
- Create: `src/app/CursorTelemetryRecordingParticipant.h`
- Create: `src/app/CursorTelemetryRecordingParticipant.cpp`
- Modify: `src/app/LiveRecordingController.h`
- Modify: `src/app/LiveRecordingController.cpp`
- Modify: `src/app/StudioRecordingBinding.h`
- Modify: `src/app/StudioRecordingBinding.cpp`
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/main.cpp`
- Create: `tests/app/RecordingLifecycleCoordinatorTest.cpp`
- Create: `tests/app/CursorTelemetryRecordingParticipantTest.cpp`
- Modify: `tests/app/LiveRecordingControllerTest.cpp`
- Modify: `tests/app/StudioRecordingBindingTest.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: existing Studio workflow preparation and Task 5 recorder/Task 6 source.
- Produces: ordered participant prepare/complete/abort and telemetry snapshot for the HUD.

- [ ] **Step 1: Write ordering, isolation, retry, and destruction tests**

```cpp
TEST(RecordingLifecycleCoordinatorTest, PreparesInOrderAndAbortsInReverse) {
    auto studio = participant("studio");
    auto cursor = participant("cursor");
    RecordingLifecycleCoordinator coordinator{{studio, cursor}};
    ASSERT_TRUE(wait(coordinator.prepare(startFixture())));
    ASSERT_TRUE(wait(coordinator.abort()));
    EXPECT_EQ(events(), (std::vector<std::string>{
        "studio.prepare", "cursor.prepare", "cursor.abort", "studio.abort"}));
}

TEST(CursorTelemetryRecordingParticipantTest, TelemetryFailureDoesNotFailMediaCompletion) {
    auto fixture = participantFixtureWithCursorWriteFailure();
    ASSERT_TRUE(wait(fixture.participant.prepare(fixture.start)));
    auto completion = wait(fixture.participant.complete(fixture.mediaCompletion));
    EXPECT_TRUE(completion.hasValue());
    EXPECT_TRUE(fixture.participant.snapshot().terminalError.has_value());
    EXPECT_EQ(fixture.mediaStore.readySegments(), 4U);
}
```

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=RecordingLifecycleCoordinatorTest.*:CursorTelemetryRecordingParticipantTest.*:LiveRecordingControllerTest.*:StudioRecordingBindingTest.*`

Expected: coordinator/participant types are missing.

- [ ] **Step 3: Implement ordered participants**

```cpp
class IRecordingLifecycleParticipant {
public:
    using Completion = std::function<void(core::Result<void>)>;
    virtual void prepare(const LiveRecordingStart&, Completion) = 0;
    virtual void complete(const LiveRecordingCompletion&, Completion) = 0;
    virtual void abort(Completion) = 0;
    virtual ~IRecordingLifecycleParticipant() = default;
};
```

The coordinator serializes prepare in registration order, aborts prepared
participants in reverse, and completes in registration order before the project
session is committed. Generation tokens ignore stale callbacks. Replace
`setRecordingPreparation` with one coordinator owned by `LiveRecordingController`;
adapt Studio behavior as a participant without changing its durable ordering.

- [ ] **Step 4: Compose cursor participant in `main.cpp`**

Create the Windows source only under `_WIN32`; otherwise create an explicit
unavailable source. Select only screen-role sources, use their frozen target
geometry, begin rows/writers before native registration, stop native input before
draining writers, and expose one aggregated snapshot.

- [ ] **Step 5: Run app lifecycle and existing Studio acceptance tests**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=RecordingLifecycleCoordinatorTest.*:CursorTelemetryRecordingParticipantTest.*:LiveRecordingControllerTest.*:StudioRecordingBindingTest.*:StudioWorkflowControllerTest.*`

Expected: all pass; existing Studio ordering and reconciliation remain exact.

- [ ] **Step 6: Commit**

```powershell
git add src/app src/main.cpp tests/app tests/CMakeLists.txt
git commit -m "feat(r2): bind cursor telemetry to recording lifecycle"
```

---

### Task 8: Accessible cursor telemetry controls and honest HUD

**Files:**
- Modify: `src/app/LiveRecordingController.h`
- Modify: `src/app/LiveRecordingController.cpp`
- Modify: `qml/StudioPage.qml`
- Modify: `tests/app/QmlSmokeTest.cpp`
- Modify: `tests/app/LiveRecordingControllerTest.cpp`

**Interfaces:**
- Consumes: Task 7 aggregated telemetry snapshot.
- Produces: QML properties `cursorTelemetryEnabled`, `cursorTelemetryAvailable`, `cursorTelemetryState`, `cursorEventCount`, `cursorMovesCoalesced`, and `cursorTelemetryMessage`.

- [ ] **Step 1: Extend QML smoke and controller tests first**

```cpp
TEST(QmlSmokeTest, StudioShowsCursorTelemetryToggleCountsAndFailureHonestly) {
    auto page = loadStudioPage(cursorTelemetryControllerFixture());
    auto* toggle = page->findChild<QObject*>("cursorTelemetryToggle");
    auto* state = page->findChild<QObject*>("cursorTelemetryStateLabel");
    ASSERT_NE(toggle, nullptr);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(toggle->property("checked").toBool());
    EXPECT_TRUE(state->property("text").toString().contains("Failed"));
    EXPECT_TRUE(state->property("text").toString().contains("video continues"));
}
```

Assert the toggle is inaccessible during recording, keyboard focusable while
idle, has an accessible name/description, and the layout remains inside a
640x480 window.

- [ ] **Step 2: Run and confirm red**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=QmlSmokeTest.*Cursor*:LiveRecordingControllerTest.*Cursor*`

Expected: named QML objects and controller properties are absent.

- [ ] **Step 3: Implement controller properties and Studio HUD**

```cpp
Q_PROPERTY(bool cursorTelemetryEnabled READ cursorTelemetryEnabled
           WRITE setCursorTelemetryEnabled NOTIFY cursorTelemetryChanged)
Q_PROPERTY(bool cursorTelemetryAvailable READ cursorTelemetryAvailable CONSTANT)
Q_PROPERTY(QString cursorTelemetryState READ cursorTelemetryState
           NOTIFY cursorTelemetryChanged)
Q_PROPERTY(qulonglong cursorEventCount READ cursorEventCount
           NOTIFY cursorTelemetryChanged)
Q_PROPERTY(qulonglong cursorMovesCoalesced READ cursorMovesCoalesced
           NOTIFY cursorTelemetryChanged)
Q_PROPERTY(QString cursorTelemetryMessage READ cursorTelemetryMessage
           NOTIFY cursorTelemetryChanged)
```

Default enabled for screen sources. Reject changes outside Idle. The UI says
`Cursor data: On`, `Unavailable`, or `Failed`; failure copy explicitly says the
media recording continues.

- [ ] **Step 4: Run focused QML/controller tests and full QML smoke**

Run: `build/windows-debug/cs_app_tests.exe --gtest_filter=QmlSmokeTest.*:LiveRecordingControllerTest.*`

Expected: all QML smoke and controller tests pass.

- [ ] **Step 5: Commit**

```powershell
git add src/app/LiveRecordingController.* qml/StudioPage.qml tests/app
git commit -m "feat(studio): expose cursor telemetry recording state"
```

---

### Task 9: R2-01 physical acceptance, scale, audits, and closure

**Files:**
- Create: `tests/acceptance/R2CursorTelemetryAcceptanceTest.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `IMPLEMENTATION_ROADMAP.md`
- Modify: `docs/superpowers/plans/2026-07-18-r2-01-cursor-telemetry.md`
- Create: `docs/superpowers/reports/2026-07-18-r2-01-verification.md`

**Interfaces:**
- Consumes: the complete R2-01 product path.
- Produces: reproducible physical evidence and the R2-01 completion decision.

- [ ] **Step 1: Write end-to-end Unicode package acceptance**

Construct an actual Studio recording session with one screen source, inject
native-seam moves and all button transitions while real video/audio segments are
recorded, stop through `LiveRecordingController`, reopen the package, and assert:

```cpp
EXPECT_EQ(stream.state, CursorTelemetryStreamState::Ready);
EXPECT_EQ(stream.eventCount, expectedEvents.size());
EXPECT_EQ(parseFile(stream.path).events, expectedEvents);
EXPECT_EQ(sha256(stream.path), stream.sha256);
EXPECT_EQ(reopenedTimeline, committedTimeline);
EXPECT_EQ(readyMediaSegments, 4U);
```

- [ ] **Step 2: Add repeated failure and crash recovery acceptance**

Inject native registration, cursor-position, queue overflow, write, flush, hash,
rename, and SQLite failures. Require valid media to remain committed, no READY
telemetry lie, exact quarantine, clean retry, and repeated controller destruction
without a running thread.

- [ ] **Step 3: Add accelerated 30-minute scale**

Generate exactly 216,000 sampled moves, 10,000 button transitions, 60 geometry
changes, and 60 scene switches. Measure producer callback p99, maximum UI event
gap, working-set growth, handles, final file bytes, finalize time, reopen parse
time, and exact counts. Initial hard budgets:

- producer callback p99 below 1 ms;
- maximum UI event gap below 250 ms;
- working-set growth below 256 MiB;
- process handles below 16,384 and reopen growth below 192;
- finalize below 10 seconds and reopen parse below 10 seconds;
- zero lost button transitions and bounded file size below 64 MiB.

- [ ] **Step 4: Run focused acceptance five times**

Run: `1..5 | % { ctest --test-dir build/windows-debug -R R2CursorTelemetryAcceptanceTest --output-on-failure -j4 --no-tests=error; if ($LASTEXITCODE) { exit $LASTEXITCODE } }`

Expected: every physical/seam/scale test passes all five repetitions with no
skip, security dialog, input movement, partial collision, timeout, or thread error.

- [ ] **Step 5: Run complete product gates**

Run the warning-as-error audited FFmpeg/MLT build, complete sequential CTest,
complete parallel CTest, hidden app launch requiring `Responding=True`, source
scan proving no keyboard/hook capture, import/link/runtime audit, and
`git diff --check`.

Expected: zero compiler warnings, zero failed/skipped tests, no external process,
no new third-party runtime, no test/fake product link input, and only User32 as
the new Windows system dependency.

- [ ] **Step 6: Independent review and fix every confirmed issue**

Review privacy, registration ownership, timestamp/sequence invariants, package
containment, recovery identity, transition loss, lifecycle ordering, UI honesty,
and scale evidence. Add a regression test for every confirmed issue and rerun
focused plus complete gates.

- [ ] **Step 7: Document and integrate**

Record exact commands, counts, timings, platform limits, and source/link audit in
the verification report. Mark only R2-01 complete. Commit, fast-forward into
`feat/r1-usable-recorder-editor`, rerun the integrated full gate, clean the owned
worktree/branch, and continue directly to R2-02.
