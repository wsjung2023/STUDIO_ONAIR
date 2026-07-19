# R0-02 프로젝트 패키지 설계

**작성일:** 2026-07-16
**상태:** 승인됨
**대상 브랜치:** `feat/r0-02-project-package`

## 1. 목적

R0-02는 `manifest.json`만 쓰는 현재 골격을 실제 작업 프로젝트 패키지로 확장한다.
사용자는 `.cstudio` 폴더를 만들고 다시 열 수 있어야 하며, 녹화 도중 앱이 비정상
종료되어도 다음 실행에서 미완료 세션을 발견하고 안전하게 복구할 수 있어야 한다.

이 단계의 핵심은 화면을 흉내 내는 것이 아니라 R0-03의 실제 캡처와 R0-05의 실제
세그먼트 기록기가 그대로 사용할 저장·복구 계약을 만드는 것이다.

## 2. 완료 조건

다음 조건을 모두 만족해야 R0-02가 완료된 것으로 본다.

1. 새 프로젝트가 `<name>.cstudio/` 폴더로 생성된다.
2. 패키지에는 유효한 `manifest.json`, 마이그레이션된 `project.db`, 표준 하위
   디렉터리가 함께 존재한다.
3. `manifest.json`은 파싱과 도메인 검증뿐 아니라 실제 JSON Schema 검증을 통과해야
   열린다.
4. SQLite 마이그레이션은 순방향으로만 실행되고, 재실행해도 같은 결과를 내며, 더
   새로운 DB 버전은 수정하지 않고 거부한다.
5. 녹화 시작 전에 `RECORDING` 세션이 커밋되고 정상 종료 시 세그먼트와
   `COMPLETED` 상태가 커밋된다.
6. 세그먼트의 `WRITING`, `READY`, `FAILED` 상태와 허용된 전이만 저장된다.
7. 프로세스가 정상 정리 없이 종료된 뒤 같은 프로젝트를 다시 열면 미완료 세션이
   복구 후보로 표시된다.
8. 사용자가 복구하면 완료된 세그먼트는 보존되고 미완료 세그먼트만 실패 상태로
   확정된다. 어떤 원본 파일도 자동으로 삭제하거나 덮어쓰지 않는다.
9. 프로젝트 파일 및 SQLite I/O는 UI 스레드에서 실행되지 않는다.
10. Windows 빌드, 전체 테스트, 앱 실행 검증을 통과한다. macOS는 CI가 없다면
    미검증 사실을 명시한다.

## 3. 범위 밖

다음은 후속 로드맵의 책임이므로 R0-02에서 구현하지 않는다.

- ScreenCaptureKit/Windows.Graphics.Capture 실제 화면 캡처
- FFmpeg MKV/MKA 기록과 컨테이너 무결성 검사
- `.part` 파일을 미디어 컨테이너로 수리하는 기능
- 정상 종료 후 단일 파일 remux
- 프로젝트 삭제, 녹화 폐기, 원본 미디어 자동 정리
- `.cstudiozip` 내보내기
- 편집 타임라인, Undo 명령 로그, 자동 저장 내용
- 네트워크 파일시스템 지원

R0-02의 복구는 **메타데이터 복구**다. `READY`로 확정된 항목을 보존하고
`WRITING` 항목을 실패로 확정해 프로젝트를 다시 열 수 있게 한다. 실제 컨테이너
검사와 `.part` 승격은 파일 기록기가 들어오는 R0-05에서 이 계약 위에 추가한다.

## 4. 선택한 접근

### 4.1 채택: 독립 프로젝트 패키지 저장 계층

`cs_project_store`가 패키지 조립, manifest, SQLite, 마이그레이션, 세션·세그먼트
저장, 복구 스캔을 책임진다. Qt-free 동기 API를 제공하고, 애플리케이션 계층의 전용
작업 스레드가 이 API를 호출한다.

이 구조는 캡처 소스나 recorder가 DB에 직접 접근하지 않으면서도 R0-03~R0-05가 같은
저장 계약을 사용할 수 있게 한다.

### 4.2 기각: `StudioController`에서 SQLite 직접 호출

구현량은 적지만 QML 파사드가 파일 경로, SQL, 녹화 상태까지 모두 책임지게 된다.
UI 스레드 I/O 금지와 `QML → application → port` 경계를 동시에 지키기 어렵기 때문에
기각한다.

### 4.3 기각: 전체 이벤트 소싱

모든 세션·세그먼트 변화를 append-only 이벤트로 저장하면 감사와 Undo에 유리하지만,
R0-02의 복구 요구를 넘는다. 명령 이벤트와 Undo는 후속 편집기 단계에서 별도 설계한다.

## 5. 모듈과 책임

### 5.1 `ManifestSchemaValidator`

- 빌드에 포함된 정확한 `schemas/project.schema.json`을 사용한다.
- raw JSON을 도메인 객체로 변환하기 전에 검증한다.
- 검증기 예외를 `AppError(ParseFailure)`로 바꾼다.
- 오류에는 JSON Pointer와 위반 이유를 포함하되 파일 내용은 포함하지 않는다.
- 저장 전에 생성한 JSON도 같은 검증기를 통과시킨다.

현재 스키마는 2020-12 전용 키워드를 사용하지 않는다. 실제 채택 검증기가 지원하는
규격과 선언을 일치시키기 위해 `$schema`를 Draft 7 URI로 변경한다. manifest 데이터
형식과 `schemaVersion`은 바뀌지 않는다.

검증기는 `pboettch/json-schema-validator` 2.4.0을 커밋/tag로 고정한다. 이 버전은
Draft 7을 지원하고 기본 format checker가 `uuid`와 `date-time`을 검사하며 MIT
라이선스다. 기존 `nlohmann/json`을 그대로 사용하므로 두 번째 JSON DOM을 도입하지
않는다. validator를 만들 때 default format checker를 명시적으로 전달해 `format`이
단순 annotation으로 무시되지 않게 한다.

스키마 파일은 CMake가 생성 헤더에 포함한다. 실행 디렉터리나 개발 저장소 경로에
의존하지 않으며, committed schema가 단일 원본이다. 스키마 변경은 CMake 재구성을
유발해야 한다.

### 5.2 `SqliteProjectDatabase`

- SQLite 연결의 RAII 소유자다.
- SQL statement 준비, bind, step, transaction을 작은 내부 래퍼로 감싼다.
- `sqlite3*`와 SQLite 오류 코드는 `cs_project_store` 밖으로 노출하지 않는다.
- 모든 SQL 입력은 bind parameter를 사용한다.
- 연결 시 다음 설정을 확인한다.
  - `PRAGMA journal_mode=WAL`
  - `PRAGMA synchronous=FULL`
  - `PRAGMA foreign_keys=ON`
  - `sqlite3_busy_timeout(..., 2000)`
- `journal_mode`가 실제로 `wal`이 되지 않으면 네트워크/비호환 파일시스템 가능성을
  설명하는 `IoFailure`로 연다.
- 프로젝트 열기 시 작업 스레드에서 `PRAGMA quick_check`를 실행한다.

SQLite 공식 `sqlite-amalgamation-3530300.zip`을 SHA3-256
`d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9`로 검증해 정적 C
라이브러리로 빌드한다. 프로젝트 언어에 C를 추가하고 시스템별 우연한 SQLite 설치에
의존하지 않는다. BOM에는 Public Domain, 3.53.3, 공식 원본 URL, 해시를 기록한다.

### 5.3 `ProjectPackageStore`

기존 `JsonProjectStore`의 manifest 기능을 재사용·정리하고 다음 고수준 동작을 제공한다.

- `create(packagePath, name) -> ProjectPackage`
- `open(packagePath) -> OpenProjectResult`
- `beginRecording(package, session) -> void`
- `completeRecording(package, stoppedAt, segments) -> void`
- `abortRecording(package, sessionId, reason) -> void`
- `beginSegment(package, sessionId, segment) -> void`
- `markSegmentReady(package, sessionId, segment) -> void`
- `markSegmentFailed(package, sessionId, segmentIndex) -> void`
- `scanRecovery(package) -> vector<RecoveryCandidate>`
- `recover(package, sessionId) -> RecoveryResult`

공개 타입은 domain/core 타입과 `std::filesystem::path`만 사용한다. SQLite 타입과 Qt
타입을 노출하지 않는다.

`open()`은 다음 순서로 실행한다.

1. `manifest.json`을 JSON으로 파싱한다.
2. `schemaVersion`만 먼저 읽어 지원 버전보다 크면 `UnsupportedVersion`을 반환한다.
3. 전체 JSON Schema 검증을 실행한다.
4. `ProjectManifest`로 변환하고 도메인 불변조건을 검증한다.
5. manifest가 가리키는 `project.db`가 패키지 안의 파일인지 확인한다.
6. DB를 열고 마이그레이션·`quick_check`를 실행한다.
7. manifest의 project ID와 DB의 project ID가 일치하는지 확인한다.
8. 미완료 세션을 조회해 manifest와 복구 후보를 함께 반환한다.

### 5.4 애플리케이션 계층

- `ProjectController`는 QML이 호출하는 프로젝트 생성·열기·복구 파사드다.
- `ProjectWorker`는 전용 `QThread`에서 하나의 열린 프로젝트와 DB 작업을 직렬화한다.
- `StudioController`는 recorder/capture를 계속 조정하지만 저장 요청은
  `ProjectWorker`의 application service 계약으로 보낸다.
- 두 컨트롤러가 SQLite나 구체 `ProjectPackageStore`를 직접 생성하지 않는다. 조립은
  `main.cpp`의 composition root가 담당한다.
- QML에는 domain/SQLite 객체 대신 단순 property, model, invokable만 노출한다.

프로젝트가 준비·저장·복구 중일 때 버튼을 다시 누를 수 없도록 명시적 busy 상태를
노출한다. UI는 작업 완료 신호를 받은 뒤에만 화면을 전환한다.

## 6. 패키지 생성과 파일 내구성

### 6.1 생성 프로토콜

사용자는 표시용 프로젝트 이름과 저장할 package path를 각각 고른다. save dialog는
이름에서 만든 `<name>.cstudio`를 제안할 뿐이며, 표시 이름을 파일명으로 강제하거나
임의로 문자를 치환하지 않는다. application 계층은 사용자가 고른 path에 `.cstudio`
suffix가 없으면 붙이고 다음 순서로 생성한다.

1. 대상과 같은 부모에 충돌하지 않는 `<target>.creating-<uuid>` 폴더를 만든다.
2. staging 폴더 안에 하위 디렉터리, manifest, DB를 모두 만든다.
3. 초기 마이그레이션과 `projects` 행 삽입을 한 transaction으로 커밋한다.
4. DB 연결을 닫는다.
5. staging 폴더를 최종 `.cstudio` 경로로 rename한다.

최종 대상이 이미 존재하면 `AlreadyExists`로 거부하며 덮어쓰지 않는다. 실패 시에는
이번 호출이 생성한 UUID staging 폴더만 삭제한다. 삭제 전 resolved parent와 staging
경로를 확인해 사용자 폴더를 잘못 지울 수 없게 한다.

### 6.2 manifest 저장

manifest 갱신은 같은 디렉터리의 임시 파일에 쓴 뒤 OS 수준 flush와 원자적 replace를
사용한다.

- Windows: `FlushFileBuffers` 후 replace/write-through 계열 Win32 API
- macOS/POSIX: 임시 파일 `fsync` 후 `rename`, 이어서 부모 디렉터리 `fsync`

실패하면 기존 manifest를 유지하고 임시 파일만 정리한다. 경로 문자열은 플랫폼 native
path를 유지해 비ASCII 경로를 ANSI codepage로 왕복하지 않는다.

## 7. SQLite 스키마와 마이그레이션

### 7.1 마이그레이션 규칙

마이그레이션 SQL은 `migrations/001_initial.sql`처럼 번호가 붙은 파일을 단일 원본으로
두고 빌드에 포함한다. 각 migration은 한 transaction 안에서 적용된다.

`schema_migrations`에는 다음을 저장한다.

- `version INTEGER PRIMARY KEY`
- `name TEXT NOT NULL`
- `checksum TEXT NOT NULL`
- `applied_at_utc TEXT NOT NULL`

checksum은 저장소가 LF로 고정한 migration SQL의 UTF-8 bytes에 대한 SHA-256이다.
build descriptor에 version, name, checksum, embedded SQL을 함께 둔다. 동일 버전의
checksum이 빌드와 다르면 DB를 수정하지 않고 실패한다. DB의 최고 버전이 앱이 아는
최고 버전보다 크면 `UnsupportedVersion`이다. rollback migration은 만들지 않는다.

### 7.2 초기 테이블

```sql
projects(
    project_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    manifest_schema_version INTEGER NOT NULL,
    created_at_utc TEXT NOT NULL,
    updated_at_utc TEXT NOT NULL
)

recording_sessions(
    session_id TEXT PRIMARY KEY,
    project_id TEXT NOT NULL REFERENCES projects(project_id),
    state TEXT NOT NULL CHECK(state IN
        ('RECORDING', 'COMPLETED', 'RECOVERED', 'ABORTED')),
    started_ns INTEGER NOT NULL CHECK(started_ns >= 0),
    stopped_ns INTEGER CHECK(stopped_ns IS NULL OR stopped_ns >= started_ns),
    created_at_utc TEXT NOT NULL,
    finished_at_utc TEXT,
    failure_reason TEXT
)

segments(
    session_id TEXT NOT NULL REFERENCES recording_sessions(session_id),
    source_id TEXT NOT NULL,
    segment_index INTEGER NOT NULL CHECK(segment_index >= 0),
    start_ns INTEGER NOT NULL CHECK(start_ns >= 0),
    duration_ns INTEGER CHECK(duration_ns >= 0),
    status TEXT NOT NULL CHECK(status IN ('WRITING', 'READY', 'FAILED')),
    relative_path TEXT NOT NULL,
    CHECK(status != 'READY' OR duration_ns IS NOT NULL),
    PRIMARY KEY(session_id, source_id, segment_index)
)
```

`recording_sessions(project_id, state)`와 `segments(session_id, status)`에 index를 둔다.
`sources` 등 후속 기능용 빈 테이블은 미리 만들지 않는다.

relative path는 비어 있거나 absolute이거나 정규화 후 `..`로 패키지 밖을 가리키면
거부한다. 이 검사는 DB constraint만으로 흉내 내지 않고 C++ 경계에서 수행한다.

## 8. 상태 전이와 데이터 흐름

### 8.1 세션 상태

허용되는 전이는 다음뿐이다.

```text
RECORDING ──clean stop────────> COMPLETED
RECORDING ──recovery──────────> RECOVERED
RECORDING ──start unwind──────> ABORTED
```

완료·복구·중단 상태에서 다시 `RECORDING`으로 돌아갈 수 없다. 이어 녹화는 새 session
ID를 만든다.

### 8.2 세그먼트 상태

```text
WRITING ──file durable────────> READY
WRITING ──writer/recovery fail> FAILED
```

`READY`와 `FAILED`는 terminal 상태다. 동일 키·동일 값의 재요청은 idempotent하게
성공하지만 내용이 다른 덮어쓰기는 `InvalidState`로 거부한다.

### 8.3 녹화 시작

1. UI가 `Preparing` 상태가 된다.
2. 작업 스레드가 `recording_sessions(RECORDING)`을 commit한다.
3. commit 성공 신호를 받은 UI/application 계층이 recorder와 capture source를
   시작한다.
4. recorder 또는 source 시작이 실패하면 자원을 unwind하고 세션을 `ABORTED`로
   확정한다.
5. DB commit 자체가 실패하면 recorder를 시작하지 않는다.

따라서 화면에 `Recording`이 표시된 세션은 이미 복구 스캔에서 발견할 수 있다.

### 8.4 정상 종료

1. capture 입력을 멈춘다.
2. recorder가 마지막 세그먼트를 닫고 `RecordingSession`을 반환한다.
3. 작업 스레드가 반환된 세그먼트를 저장하고 세션을 `COMPLETED`로 바꾸는 한
   transaction을 commit한다.
4. commit 후에만 UI가 `Stopped`와 최종 세그먼트 수를 표시한다.

3단계가 실패하면 DB의 세션은 `RECORDING`으로 남는다. 사용자는 성공 메시지를 보지
않으며 다음 열기에서 복구할 수 있다.

R0-02의 fake recorder는 실제 파일을 쓰지 않으므로 정상 종료 시 반환한 세그먼트를
transaction으로 저장한다. `beginSegment`/`markSegmentReady` 계약과 상태 전이 테스트는
지금 완성하되, 실제 writer가 각 경계에서 호출하는 연결은 R0-05에서 수행한다.

### 8.5 복구

복구 후보는 `state='RECORDING'`인 세션이다. 화면에는 프로젝트명·경로, 시작 시각,
`READY` 개수, `WRITING` 개수를 표시한다.

사용자가 **복구**를 선택하면 한 transaction에서 다음을 수행한다.

1. 해당 세션의 모든 `WRITING` 세그먼트를 `FAILED`로 변경한다.
2. `READY`와 기존 `FAILED` 세그먼트는 변경하지 않는다.
3. `stopped_ns`는 `READY` 세그먼트의 최대 `start_ns + duration_ns`로 정한다.
4. `READY`가 없으면 `stopped_ns = started_ns`로 정한다.
5. 세션을 `RECOVERED`로 변경하고 `finished_at_utc`를 기록한다.

3단계의 덧셈은 C++에서 overflow를 검사한 뒤 SQLite에 bind한다. 이미 `RECOVERED`인
동일 session을 다시 복구하라는 요청은 행을 수정하지 않고 기존 결과를 반환한다.

파일 삭제·rename·수정은 하지 않는다. **나중에**를 선택하면 DB도 수정하지 않고 Home에
후보를 계속 남긴다. R0-02에는 폐기 버튼을 만들지 않는다.

## 9. Home 및 복구 UI

Home에는 다음 생산 경로를 추가한다.

- 새 녹화 프로젝트: 표시 이름과 package path를 선택해 패키지를 만든 뒤 Studio로 이동
- 기존 프로젝트 열기: 폴더를 선택해 검증·마이그레이션·복구 스캔
- 최근 프로젝트: 성공적으로 생성·연 프로젝트 경로
- 복구 가능한 녹화: 등록된 최근 프로젝트에서 발견한 recovery candidate

최근 프로젝트 registry는 `QStandardPaths::AppConfigLocation/recent-projects.json`에
최대 20개의 native path와 마지막 성공 open UTC를 저장한다. `ProjectWorker`에서 읽고
`QSaveFile`로 교체하며, 성공적으로 생성하거나 연 프로젝트만 갱신한다. 프로젝트 내부
데이터의 source of truth는 아니다. registry에서 경로가 사라졌다고 프로젝트나 미디어를
삭제하지 않는다.

`RecoveryPage.qml`은 후보별로 **복구**와 **나중에**만 제공한다. 작업 중에는 중복 클릭을
막고, 성공 후 프로젝트를 열며, 실패 시 같은 후보와 구체 오류 메시지를 유지한다.

## 10. 오류 처리

- JSON 구문/스키마 위반: `ParseFailure`
- manifest 또는 DB의 미래 버전: `UnsupportedVersion`
- 누락된 manifest/DB: `NotFound`
- 기존 대상 프로젝트: `AlreadyExists`
- DB 손상, WAL 전환 실패, 쓰기/flush/rename 실패: `IoFailure`
- 금지된 상태 전이 또는 기존 terminal 행과 충돌: `InvalidState`
- 잘못된 상대 경로·음수 시간·빈 ID: `InvalidArgument`

라이브러리 예외는 모듈 경계를 넘지 않는다. SQLite 오류 메시지는 operation과 SQLite
result code를 기록하되 SQL parameter, 파일 내용, 사용자 녹화 내용은 기록하지 않는다.
사용자 메시지는 실패한 작업과 다음 행동(로컬 디스크로 복사, 새 버전으로 열기 등)을
설명한다.

## 11. 테스트 전략

### 11.1 manifest/schema

- 생성한 manifest가 실제 스키마를 통과한다.
- 추가 property, 잘못된 UUID/date-time, 중복 `requiredFeatures`, 경계 밖 canvas를
  거부한다.
- 미래 `schemaVersion`은 다른 스키마 오류보다 먼저 `UnsupportedVersion`으로 보고한다.
- 저장 전 검증 실패가 기존 manifest를 손상하지 않는다.
- 한글·일본어·이모지 이름과 경로를 round-trip한다.

### 11.2 패키지/내구성

- 성공 시 manifest·DB·모든 표준 폴더가 함께 존재한다.
- manifest/DB 생성 실패를 주입하면 최종 `.cstudio`가 생기지 않는다.
- 기존 대상은 덮어쓰지 않는다.
- atomic replace 실패 시 기존 manifest가 유지된다.
- staging cleanup이 대상 부모 밖 경로를 절대 삭제하지 않는다.

### 11.3 SQLite/migration

- 빈 DB에 001이 한 번 적용된다.
- 재오픈은 migration을 재실행하지 않는다.
- transaction 중 실패하면 일부 테이블/버전이 남지 않는다.
- checksum 불일치와 미래 migration을 수정 없이 거부한다.
- foreign key, unique key, CHECK constraint가 실제로 동작한다.
- manifest/DB project ID 불일치를 거부한다.

### 11.4 세션/세그먼트

- 시작 commit 전 recorder가 호출되지 않는다.
- 정상 종료가 세그먼트와 `COMPLETED`를 한 transaction으로 남긴다.
- 시작 실패가 `ABORTED`를 남긴다.
- `WRITING → READY|FAILED`만 허용한다.
- terminal 상태의 동일 재요청은 성공하고 다른 내용 덮어쓰기는 실패한다.
- 상대 경로 탈출을 거부한다.

### 11.5 강제 종료/복구

별도 test helper 프로세스가 패키지를 열고 `RECORDING` 세션과 세그먼트 상태를 commit한
직후 `std::_Exit`로 destructor와 정상 종료 경로를 건너뛴다. 부모 테스트는 프로세스
종료를 기다린 뒤 프로젝트를 다시 연다. `sleep`이나 실행 속도 가정은 사용하지 않는다.

검증 항목:

- WAL 복구 후 DB가 열린다.
- 미완료 세션이 정확히 한 후보로 나타난다.
- 복구 시 READY는 보존되고 WRITING만 FAILED가 된다.
- 파일 fixture의 내용과 개수가 변하지 않는다.
- 복구를 두 번 요청해도 결과가 안정적이다.
- 나중에 선택하면 DB가 바뀌지 않는다.

### 11.6 application/QML

- `ProjectController`의 busy, 성공, 오류, 후보 model signal을 `QSignalSpy`로 검증한다.
- 느린/실패 저장을 fake store로 주입해 UI 스레드가 막히지 않고 중복 명령이 거부되는지
  검증한다.
- QML module load test로 `RecoveryPage` binding과 invokable 이름을 검증한다.
- 기존 Studio Record/Stop 회귀 테스트를 유지한다.

## 12. 라이선스와 공급망

- SQLite 3.53.3: Public Domain, 공식 amalgamation URL과 SHA3-256 고정
- pboettch/json-schema-validator 2.4.0: MIT, Git tag/commit 고정
- nlohmann/json 3.11.3: 기존 MIT 고정 유지

`legal/OSS_BOM.csv`에 runtime/static 포함 여부와 버전을 기록한다. AGPL/상용 이중
라이선스인 Blaze는 기능이 맞더라도 현재 closed-core 정책과 충돌하므로 사용하지 않는다.

## 13. 구현 순서의 경계

상세 계획은 별도 문서에서 TDD 단위로 나눈다. 구현은 다음 의존 순서를 지킨다.

1. 의존성·스키마 검증기와 테스트
2. SQLite RAII와 migration runner
3. 패키지 원자 생성·열기
4. 세션·세그먼트 persistence와 recovery
5. application worker/controller
6. Home/Recovery QML 연결
7. 강제 종료 통합 테스트와 전체 회귀 검증

각 단계는 빌드 가능하고 테스트 가능한 작은 커밋으로 끝낸다.
