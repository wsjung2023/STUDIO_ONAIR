# R0-01 Bootstrap 설계

- 작업지시서: `prompts/01-bootstrap.md`
- 로드맵 항목: `IMPLEMENTATION_ROADMAP.md` → R0-01 저장소와 빌드
- 상태: 승인됨 (2026-07-16)

## 1. 목표

저장소를 macOS와 Windows에서 빌드 가능한 Qt 6/QML + C++20 기반으로 완성한다.
실제 화면 캡처는 구현하지 않는다. 네이티브 캡처가 나중에 안전하게 들어올 수 있도록
**경계와 테스트를 먼저 고정**하는 것이 이번 작업의 전부다.

## 2. 검증된 환경 기준점

구현을 시작하기 전에 툴체인을 설치하고, 스타터팩 원본이 빌드·실행되는 것을 확인했다.
이 기준점이 있어야 이후 실패가 "내 변경 탓"인지 "환경 탓"인지 가릴 수 있다.

| 구성요소 | 버전 | 경로 |
|---|---|---|
| MSVC 툴셋 | 14.44.35207 (cl 19.44.35228) | VS 2022 BuildTools 17.14.36 |
| Windows SDK | 10.0.22621.0, 10.0.26100.0 | — |
| CMake | 3.31.6 (VS 번들) | 요구 3.25 이상 충족 |
| Ninja | 1.12.1 | VS 번들 |
| Qt | 6.8.3 LTS `win64_msvc2022_64` (+qtmultimedia, qtshadertools) | `C:\Qt\6.8.3\msvc2022_64` |

검증 결과 (스타터팩 원본, 무수정 상태):

- configure: 성공
- build: 성공 (21/21)
- 실행: `creator_studio.exe` 기동, 창 제목 `Creator Studio`, 응답 정상

Qt 6.8을 고른 이유는 두 가지다. `CMakeLists.txt`가 이미 `find_package(Qt6 6.8 REQUIRED)`와
`qt_standard_project_setup(REQUIRES 6.8)`로 6.8을 요구하고 있고, 6.8이 LTS라 상용 제품
수명과 맞다. MSVC를 고른 이유는 `ARCHITECTURE.md` §1.2가 Windows 캡처를
Windows.Graphics.Capture(WinRT) + D3D11로 못박았기 때문이다. MinGW로 시작하면 R0에서
반드시 갈아엎어야 한다.

## 3. 핵심 결정: 아키텍처 규칙을 링커로 강제한다

### 문제

세 문서가 같은 규칙을 반복해서 못박고 있다.

- `ARCHITECTURE.md` §4.2 — "도메인 계층은 FFmpeg, Qt Multimedia, MLT 타입을 노출하지 않는다"
- `ARCHITECTURE.md` §14 — 의존 방향 `qml/ui → application → domain`
- `CLAUDE.md` §5 — "Domain 계층은 Qt Multimedia, FFmpeg, MLT의 타입을 포함하지 않는다"
- `prompts/01-bootstrap.md` 품질기준 — "domain에 Qt Multimedia/FFmpeg/MLT include 금지"

그런데 스타터팩은 **단일 타깃**이다. 모든 소스가 `qt_add_executable(creator_studio ...)`
하나에 들어간다. 이 구조에서 위 규칙은 전부 주석에 불과하다. 누가 `domain/Project.h`에
`#include <QString>`을 넣어도 빌드는 통과한다. 위반을 잡아낼 수 있는 건 리뷰어의 기억력뿐이고,
그건 R2쯤 코드가 쌓이면 반드시 실패한다.

### 결정

모듈을 CMake static library로 분리하고, **하위 계층에는 Qt를 링크하지 않는다.**
`cs_domain`이 Qt를 링크하지 않으면 `#include <QString>`은 물리적으로 컴파일되지 않는다.
규칙이 문서가 아니라 빌드 실패로 지켜진다.

검토했으나 채택하지 않은 대안:

- **단일 타깃 + clang-tidy 규칙** — 우회가 쉽고, R0-01 시점에 tidy 세팅까지는 과하다.
- **현행 유지 + 코드리뷰** — 상용 v1까지 가는 수명에서 사람 기억력에 의존하는 건 실패한다.

지금은 소스가 5개뿐이라 분리 비용이 거의 없다. 코드가 쌓인 뒤에는 사실상 되돌릴 수 없다.

## 4. 타깃 구조

모듈 이름과 책임은 `ARCHITECTURE.md` §4를 그대로 따른다. 새로 발명하지 않는다.

```
creator_studio (exe, Qt Quick)
         │
         ▼
      cs_app  (Qt Core)          ── QML이 호출하는 유일한 경계
         │
         └──────────────┬──────────────────┐
                        ▼                  ▼
                    cs_fakes        cs_project_store
                        │                  │
              ┌─────────┴─────────┐        │
              ▼                   ▼        │
         cs_capture          cs_recorder   │
              │                   │        │
              └─────────┬─────────┘        │
                        ▼                  │
                    cs_media               │
                        │                  │
                        ▼                  │
                    cs_domain ◄────────────┘
                        │
                        ▼
                     cs_core
```

`cs_fakes`는 `cs_capture`/`cs_recorder`의 포트를 구현하므로 그쪽에 의존한다.
`cs_project_store`는 `IProjectStore`와 그 구현을 함께 담으며 `cs_domain`만 필요로 한다.

| 타깃 | 소스 | Qt 링크 | 내용 |
|---|---|---|---|
| `cs_core` | `src/core/` | **없음** | Timebase, `Result<T>`, `AppError`, UuidV4, 로깅 |
| `cs_domain` | `src/domain/` | **없음** | ProjectId/SourceId/SessionId, TimestampNs/DurationNs, ProjectManifest, RecordingSession, Segment |
| `cs_media` | `src/media/` | **없음** | VideoFrame, AudioBlock, PixelFormat, ColorSpace |
| `cs_capture` | `src/capture/` | **없음** | `ICaptureSource` 포트 |
| `cs_recorder` | `src/recorder/` | **없음** | `IRecorder` 포트, Segment 메타데이터 |
| `cs_project_store` | `src/project_store/` | **없음** | `IProjectStore` 포트 + `JsonProjectStore` 구현 |
| `cs_fakes` | `src/fakes/` | **없음** | `FakeCaptureSource`(60fps 타임스탬프), `FakeRecorder`(세그먼트 메타 메모리 기록) |
| `cs_app` | `src/app/` | Qt Core | `StudioController` |
| `creator_studio` | `src/main.cpp`, `qml/` | Qt Quick, QuickControls2 | 진입점 + UI |
| `cs_tests` | `tests/` | 없음 (GTest) | 단위 테스트 |

`cs_fakes`를 별도 타깃으로 두는 이유: R0-03에서 실제 캡처가 들어오면 `cs_app`의 링크
목록에서 한 줄 지우는 것으로 제거된다. 프로덕션 코드에 fake가 섞이지 않는다.

이번 작업에서 `src/capture/ICaptureSource.h`는 위치를 바꾸지 않는다. `ARCHITECTURE.md`
§4.3이 `ICaptureSource`를 `capture` 모듈에 두고 있으므로 그대로 따른다.

## 5. 의존성과 라이선스

`CLAUDE.md` §7이 "GPL 의존성을 추가하지 않는다. 추가 전 `legal/OSS_BOM.csv`를 갱신한다"고
못박고 있다. `legal/LICENSE_POLICY.md`의 허용 기본군은 MIT, BSD-2, BSD-3, Apache-2.0,
Public Domain이다.

| 라이브러리 | 라이선스 | 정책 판정 | 도입 이유 |
|---|---|---|---|
| GoogleTest | BSD-3-Clause | 허용군. **테스트 전용, 배포물 미포함** | Qt-free라서 domain 테스트가 Qt 경계를 실제로 증명한다 |
| nlohmann/json | MIT | 허용군 | `QJsonDocument`를 쓰면 `cs_project_store`가 Qt에 묶여 §3 결정이 무너진다 |

두 항목 모두 `legal/OSS_BOM.csv`에 추가한다. GoogleTest는 배포물에 포함되지 않으므로
`linking_or_boundary` 칼럼에 테스트 전용임을 명시한다. 이 구분이 없으면 나중에 SBOM이
배포하지도 않는 것을 배포한다고 보고하게 된다.

**manifest 검증 범위 (오해하기 쉬운 지점)**: nlohmann/json은 JSON 파서지 JSON Schema
검증기가 아니다. 이번 작업의 지시 항목 7은 "manifest 생성·읽기"까지고, 정식 스키마 검증
(`schemas/project.schema.json`을 실제로 적용하는 것)은 로드맵상 R0-02의 "manifest JSON
스키마 검증" 항목이다. 따라서 R0-01에서는 손으로 쓴 필수 필드·타입·범위 확인까지만 하고,
JSON Schema 검증기 도입은 R0-02에서 별도로 판단한다. 지금 스키마 검증기를 끌어들이면
의존성 결정을 R0-02의 요구사항을 모르는 상태에서 미리 하는 셈이 된다.

UUID v4는 라이브러리를 추가하지 않고 `cs_core`에 직접 구현한다. `schemas/project.schema.json`이
`projectId`에 `format: uuid`를 요구하지만, 이것 하나 때문에 의존성을 늘리는 건 과하다.
생성기는 20줄 남짓이고 테스트로 형식을 검증한다.

두 라이브러리 모두 CMake `FetchContent`로 가져온다.

## 6. 산출물 (작업지시서 10개 항목 대응)

| # | 지시 항목 | 구현 |
|---|---|---|
| 1 | CMake Presets | `CMakePresets.json` — `windows-debug/release`, `macos-debug/release` (모두 Ninja) + `CS_WARNINGS_AS_ERRORS` 옵션 |
| 2 | Home/Studio/Editor 3화면 | `qml/`을 화면별 파일로 분리, `StudioController`가 상태 소유 |
| 3 | value object 5종 | `ProjectId`, `SourceId`, `SessionId` (기존 `Identifier<Tag>` 활용), `TimestampNs`, `DurationNs` |
| 4 | `Result<T>` / `AppError` | `cs_core`. `std::expected`는 C++23이라 사용 불가(표준 C++20 고정) → 자체 구현 |
| 5 | 포트 3종 | `IProjectStore`, `ICaptureSource`, `IRecorder` |
| 6 | in-memory fake | `FakeCaptureSource`(60fps 타임스탬프만 발생), `FakeRecorder`(세그먼트 메타 메모리 기록) |
| 7 | manifest 생성·읽기 | `JsonProjectStore` — `schemas/project.schema.json`의 형태를 준수. 검증 범위는 아래 참고 |
| 8 | unit test | 아래 §7 |
| 9 | CI 초안 | `.github/workflows/ci.yml` — windows-latest + macos-latest |
| 10 | README 빌드 명령 검증 | 문서의 명령을 실제로 실행해 확인하고, 틀리면 README를 고친다 |

UI 요구사항 (작업지시서 "UI 요구"):

- Studio는 실제 영상 대신 test pattern 표시
- 좌측 Scenes/Sources, 중앙 Preview, 우측 Inspector, 하단 Audio/Stats placeholder
- Record → fake 세션 시작, Stop → 세그먼트 개수와 duration 표시
- QML은 `StudioController`만 호출한다. QML에서 도메인 객체를 직접 만지지 않는다.

## 7. 테스트

작업지시서 8항이 요구하는 4종에, `CLAUDE.md` §8이 요구하는 오류 경로를 더한다.
`CLAUDE.md` §8은 "unit test, 오류 경로 test, 로그 또는 metric 검증, 리소스 정리 검증"을
기능 PR의 최소 조건으로 정하고 있다.

| 대상 | 정상 경로 | 오류 경로 |
|---|---|---|
| 시간 단위 | ns↔ms↔s 변환, 프레임 번호↔타임스탬프 | 음수 duration, 0 프레임레이트 |
| ID | 직렬화/역직렬화, 동등성, 타입 안전성 | 빈 문자열 ID |
| manifest | round-trip (쓰기→읽기→동일) | 손상된 JSON, 필수 필드 누락, 미래 schemaVersion, 존재하지 않는 경로 |
| fake 녹화 세션 | start→stop, 세그먼트 수·duration | 중복 start, stop 없이 파괴, 미시작 상태 stop |
| UuidV4 | 형식 검증, 중복 없음 | — |
| `Result<T>`/`AppError` | 값 전달, 오류 전파 | 오류 상태에서 값 접근 |

타입 안전성 테스트는 컴파일 타임 검증이므로 `static_assert`로 표현한다.
(예: `ProjectId`를 `SourceId` 자리에 넣으면 컴파일되지 않아야 한다.)

## 8. CI

`.github/workflows/ci.yml` 초안. windows-latest와 macos-latest 두 잡.

1. Qt 6.8.3 설치 (`jurplel/install-qt-action`)
2. CMake preset으로 configure
3. build (warnings-as-errors 켜고)
4. `ctest` 실행

이번 작업은 "초안"까지다. 실제 러너에서 녹색이 뜨는 것을 확인하는 건 원격 저장소가
연결된 뒤의 일이다. 이 한계는 문서에 남긴다.

## 9. 부수적으로 처리할 것

기준점 확보 과정에서 발견한, 이번 작업 범위 안에서 고치는 게 맞는 것들이다.

- **`.gitattributes` 추가** — 최초 커밋에서 git이 24개 파일 전부에 LF→CRLF 경고를 냈다.
  이 프로젝트는 macOS와 Windows CI를 동시에 돌리므로 줄바꿈 정규화가 없으면 두 플랫폼이
  서로의 커밋을 계속 뒤집는다.
- **중복 blueprint 문서** — `PRODUCT_BLUEPRINT.md`와 `creator-studio-product-blueprint-v0.1.md`는
  SHA256이 동일한 완전 중복본(11,411 bytes)이다. 두 벌을 두면 반드시 갈라진다.
  다만 삭제는 제품 문서에 대한 결정이므로 이번 작업에서 임의로 하지 않고 별도로 확인받는다.

## 10. 비범위

작업지시서 "이번 작업에서 하지 말 것"을 그대로 따른다. 아래는 한 줄도 건드리지 않는다.

- FFmpeg 다운로드 자동화
- MLT 연결
- ScreenCaptureKit
- Windows.Graphics.Capture
- MediaPipe
- whisper.cpp
- 임시 Electron/Tauri 추가

추가로 이번 범위가 아닌 것:

- SQLite와 `project.db` (R0-02 프로젝트 패키지)
- 복구 스캐너 (R0-02)
- clang-format / clang-tidy 설정 (R0-01 로드맵 항목이지만 작업지시서 10개 항목에 없다. 별도 작업으로 분리한다.)

## 11. 완료 기준

- `cmake --preset windows-debug` → configure 성공
- `cmake --build --preset windows-debug` → warning 0으로 빌드 성공
- `ctest --preset windows-debug` → 전 테스트 통과
- 앱 실행 → Home/Studio/Editor 전환 동작
- Studio에서 Record → Stop → 세그먼트 개수와 duration 표시
- `cs_core`, `cs_domain`, `cs_media`, `cs_capture`, `cs_recorder`, `cs_project_store`,
  `cs_fakes` 어느 것도 Qt를 링크하지 않는다 (CMake로 검증 가능)
- `legal/OSS_BOM.csv`에 GoogleTest, nlohmann/json 반영
- README의 빌드 명령이 실제로 동작

macOS 빌드는 이 머신에서 검증할 수 없다. CI 워크플로가 그 역할을 하되, 이번 작업에서는
실행 검증이 불가능하다는 것을 명시적 한계로 남긴다.
