# 세로 9:16 쇼츠 프로젝트 모드 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 새 프로젝트를 세로(9:16)로 만들 수 있게 하고, 편집기·MLT·내보내기가 세로로 동작하며, 가로 소스가 세로 캔버스에 화면-위 + 카메라/아바타-아래로 기본 합성된다.

**Architecture:** 방향은 매니페스트 `CanvasSettings`(1080×1920)에 1급 저장. 편집기/프리뷰/오버레이는 기존 canvas-dims 소비 경로를 그대로 써 자동 적응. 소스 임포트 시 캔버스가 세로면 역할별 기본 `VisualTransform`을 적용. 세로 `RenderPreset` 추가.

**Tech Stack:** C++20, Qt6/QML, MLT, GoogleTest. 스펙: docs/superpowers/specs/2026-07-25-vertical-shorts-project-mode-design.md

## Global Constraints
- RAII, raw owning pointer 금지, `std::chrono` 시간, 매직 넘버 대신 named 상수/typed value (CLAUDE.md 4).
- Domain 계층(TimelineTypes/ProjectManifest)에 Qt/FFmpeg/MLT 타입 금지 (CLAUDE.md 5).
- 각 기능은 unit + 오류경로 test + 문서 (CLAUDE.md 8). TDD, 작은 buildable commit.
- 녹화 캡처 경로는 건드리지 않는다(세로는 합성/캔버스 속성).
- 빌드/테스트: `windows-ffmpeg-release`, vcvars64 + `cmake --build`. 테스트 타깃: 프리셋/캔버스=`cs_tests` 또는 소유 타깃; export 통합=`cs_onetool_real_screen_driver`.

---

### Task 1: 세로 RenderPreset

**Files:**
- Modify: `src/edit_engine/EditEngineTypes.h:163-164` (팩토리 선언 추가)
- Modify: `src/edit_engine/EditEngineTypes.cpp:262-275` (팩토리 정의 추가)
- Modify: `src/app/ExportController.cpp:122-126` (presetId → 팩토리 분기 추가)
- Test: `tests/edit_engine/EditEngineTypesTest.cpp` (또는 RenderPreset를 테스트하는 기존 파일)

**Interfaces:**
- Produces: `RenderPreset::h2641080x1920p30()` → id `"h264-1080x1920p30"`, 1080×1920, 30fps, 12'000'000 vBitrate, 192'000 aBitrate. `RenderPreset::h264720x1280p30()` → id `"h264-720x1280p30"`, 720×1280, 30fps, 8'000'000, 192'000.
- Consumes: 기존 `RenderPreset::create(id,w,h,frameRate,videoBitrate,audioBitrate)`, `FrameRate` 30fps 헬퍼(기존 h2641080p30 방식 그대로).

- [ ] **Step 1: 실패 테스트** — 세로 프리셋 dims/id 단언:
```cpp
TEST(RenderPresetTest, VerticalPresetsHavePortraitDimensions) {
    auto a = creator::edit_engine::RenderPreset::h2641080x1920p30();
    ASSERT_TRUE(a.hasValue());
    EXPECT_EQ(a.value().width(), 1080u);
    EXPECT_EQ(a.value().height(), 1920u);
    auto b = creator::edit_engine::RenderPreset::h264720x1280p30();
    ASSERT_TRUE(b.hasValue());
    EXPECT_EQ(b.value().width(), 720u);
    EXPECT_EQ(b.value().height(), 1280u);
    EXPECT_GT(a.value().height(), a.value().width());  // portrait
}
```
- [ ] **Step 2: 빌드해 실패 확인** (팩토리 미정의). Run: 해당 test 타깃 빌드 → 컴파일 에러/FAIL.
- [ ] **Step 3: 팩토리 구현** — `EditEngineTypes.cpp`에 h2641080p30 패턴 그대로, dims만 세로:
```cpp
Result<RenderPreset> RenderPreset::h2641080x1920p30() {
    auto frameRate = core::FrameRate::create(30, 1);
    if (!frameRate.hasValue()) return frameRate.error();
    return create("h264-1080x1920p30", 1080, 1920, frameRate.value(),
                  12'000'000, 192'000);
}
Result<RenderPreset> RenderPreset::h264720x1280p30() {
    auto frameRate = core::FrameRate::create(30, 1);
    if (!frameRate.hasValue()) return frameRate.error();
    return create("h264-720x1280p30", 720, 1280, frameRate.value(),
                  8'000'000, 192'000);
}
```
헤더에 선언 2줄 추가.
- [ ] **Step 4: ExportController 분기** — `ExportController.cpp` presetId 매핑에 추가:
```cpp
    : presetId == QStringLiteral("h264-1080x1920p30")
          ? edit_engine::RenderPreset::h2641080x1920p30()
    : presetId == QStringLiteral("h264-720x1280p30")
          ? edit_engine::RenderPreset::h264720x1280p30()
```
(기존 if-else 체인에 삽입)
- [ ] **Step 5: 통과 확인** — 테스트 빌드+실행 PASS.
- [ ] **Step 6: 커밋** — `feat(vertical): add 9:16 render presets (1080x1920, 720x1280)`

---

### Task 2: 프로젝트 방향 → 캔버스 치수

**Files:**
- Modify: `src/app/ProjectController.h:57` + `.cpp:156-170` (createProject에 방향 인자)
- Modify: `src/app/ProjectWorker.cpp:50` (createProject → 매니페스트 canvas dims)
- Modify: 매니페스트 생성 지점(ProjectWorker가 부르는 store/manifest 빌더)에서 `CanvasSettings{}` 대신 방향 dims.
- Test: `tests/app/ProjectController*` 또는 `tests/project_store/JsonProjectStoreTest.cpp` (canvas 라운드트립은 이미 있음 — 세로 케이스 추가)

**Interfaces:**
- Produces: `ProjectController::createProject(QUrl, displayName, bool portrait)` (또는 int orientation). 기본 가로(portrait=false)로 기존 호출 보존. Portrait → `CanvasSettings{width:1080, height:1920, ...}`.
- Consumes: `domain::CanvasSettings{width,height,frameRateNumerator,frameRateDenominator,colorSpace}` (ProjectManifest.h:35).

- [ ] **Step 1: 실패 테스트** — 세로 매니페스트 canvas 라운드트립:
```cpp
TEST(JsonProjectStoreTest, PersistsPortraitCanvas) {
    creator::domain::CanvasSettings portrait{.width = 1080, .height = 1920};
    // write manifest with portrait canvas, read back
    // EXPECT round-trip width==1080 height==1920
}
```
(기존 canvas 라운드트립 테스트 패턴 재사용)
- [ ] **Step 2: 실패 확인.**
- [ ] **Step 3: 방향 배선** — ProjectWorker::createProject가 방향/dims를 받아 매니페스트 canvas를 설정(기본 가로). ProjectController::createProject에 인자 추가, worker로 전달. named 상수:
```cpp
namespace { constexpr std::int32_t kLandscapeW=1920, kLandscapeH=1080,
                    kPortraitW=1080, kPortraitH=1920; }
```
- [ ] **Step 4: 통과 확인.**
- [ ] **Step 5: 커밋** — `feat(vertical): create project with portrait (1080x1920) canvas option`

---

### Task 3: 역할별 세로 기본 VisualTransform

**Files:**
- Create: `src/app/VerticalLayout.h` + `.cpp` (또는 RecordingImportPlanner 내 free 함수) — `verticalDefaultTransform(role, canvasW, canvasH)`.
- Modify: `src/app/RecordingImportPlanner.cpp:193-231` (`stateInScene`) — 캔버스가 세로면 screen/camera/avatar에 세로 기본 transform 적용(씬 transform 대신).
- Modify: 플래너에 canvas dims 전달(reconciler/planner 진입점). 이미 매니페스트 접근 가능한지 확인 후 배선.
- Test: `tests/app/RecordingImportPlannerTest.cpp` (또는 새 `VerticalLayoutTest.cpp`)

**Interfaces:**
- Produces: `std::optional<domain::VisualTransform> verticalDefaultTransform(StudioSourceRole role, std::int32_t canvasW, std::int32_t canvasH)`.
  세로(canvasH>canvasW)에서:
  - Screen: `VisualTransform::create(0.0, 0.04, 1.0, 0.316, 1,1,0, 0,0,0,0, 1.0, z_screen)` — 폭맞춤 상단(1080×608 16:9).
  - Camera: `create(0.0, 0.60, 1.0, 0.316, 1,1,0, 0,0,0,0, 1.0, z_camera)` — 하단 띠.
  - Avatar: `create(0.62, 0.66, 0.36, 0.30, 1,1,0, 0,0,0,0, 1.0, 20)` — 하단 우측 코너 PiP(z=20 위).
  - 가로(canvasW≥canvasH): `std::nullopt`(기존 씬 transform 유지).
- Consumes: `domain::VisualTransform::create(...)` (TimelineTypes.h:59, 13인자), `StudioSourceRole`.

- [ ] **Step 1: 실패 테스트** — 세로/가로 분기 좌표 단언:
```cpp
TEST(VerticalLayoutTest, ScreenTopCameraBottomOnPortrait) {
    auto s = verticalDefaultTransform(StudioSourceRole::Screen, 1080, 1920);
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->width(), 1.0);
    EXPECT_LT(s->y(), 0.5);                 // screen near top
    auto c = verticalDefaultTransform(StudioSourceRole::Camera, 1080, 1920);
    ASSERT_TRUE(c.has_value());
    EXPECT_GT(c->y(), 0.5);                 // camera lower half
    EXPECT_FALSE(verticalDefaultTransform(StudioSourceRole::Screen, 1920, 1080)
                     .has_value());          // landscape -> no override
}
```
- [ ] **Step 2: 실패 확인.**
- [ ] **Step 3: 구현** — named 상수 + 위 create 호출로 함수 구현. z-order 상수는 기존 관례(avatar 20 등) 참고해 named.
- [ ] **Step 4: 플래너 배선** — `stateInScene`(또는 호출부)이 canvas dims를 알 때, 세로면 role별로 `verticalDefaultTransform`을 우선 적용. 없으면(nullopt) 기존 `found->transform()`/avatar full-frame 유지. canvas dims를 planner로 전달하는 배선 추가.
- [ ] **Step 5: 통과 확인** (VerticalLayoutTest + 기존 RecordingImportPlannerTest 회귀 없음).
- [ ] **Step 6: 커밋** — `feat(vertical): default screen-top/camera-bottom layout on portrait import`

---

### Task 4: UI — 방향 선택 + 방향-매칭 프리셋

**Files:**
- Modify: `qml/HomePage.qml` (새 프로젝트 흐름에 가로/세로 선택; `createProject` 호출에 방향 전달)
- Modify: `qml/ExportPage.qml:114-117` (preset model에 세로 프리셋; 프로젝트 방향에 맞는 것 노출)
- Modify: 방향을 QML에 노출할 read-only 프로퍼티(EditorController/ProjectController에 `bool portrait`(canvasH>canvasW)).

**Interfaces:**
- Consumes: Task 2의 `createProject(..., portrait)`, Task 1의 preset id `"h264-1080x1920p30"`/`"h264-720x1280p30"`.
- Produces: QML `portrait` 프로퍼티(캔버스 종횡비에서).

- [ ] **Step 1: HomePage 방향 선택** — 새 프로젝트 다이얼로그/화면에 세그먼트("가로 16:9" / "세로 9:16 쇼츠"), 선택값을 createProject에 전달. 기본 가로.
- [ ] **Step 2: ExportPage 세로 프리셋** — preset ComboBox model에 세로 항목 추가; `root.portrait`면 세로 프리셋을 기본/우선 노출:
```qml
model: root.portrait
    ? [ { label: qsTr("1080x1920 · 30 fps (쇼츠)"), value: "h264-1080x1920p30" },
        { label: qsTr("720x1280 · 30 fps (경량)"), value: "h264-720x1280p30" } ]
    : [ { label: qsTr("1080p · 30 fps"), value: "h264-1080p30" },
        { label: qsTr("4K · 30 fps"), value: "h264-2160p30" } ]
```
- [ ] **Step 3: QML 스모크** — `cs_app_tests` QmlSmokeTest가 두 페이지 로드/컴파일 되는지(기존 QML smoke에 편입). 방향 프로퍼티 바인딩 오류 없음 확인.
- [ ] **Step 4: 커밋** — `feat(vertical): project-creation orientation picker + export presets in UI`

---

### Task 5: 헤드리스 세로 export 통합 검증

**Files:**
- Modify(optional): `tests/acceptance/RealScreenEditExportDriver.cpp` (세로 프리셋으로 export하는 env-gated 경로) 또는 새 통합 테스트.
- Test scaffold: scratchpad 스크립트로 세로 프로젝트 export → PyAV로 1080×1920 확인.

**Interfaces:**
- Consumes: Task 1 세로 preset, Task 2 세로 canvas, Task 3 기본 레이아웃.

- [ ] **Step 1: 세로 export 실측** — 세로 캔버스 프로젝트를 세로 프리셋으로 export한 MP4가 **1080×1920**(9:16)인지 PyAV로 측정. 화면이 상단 영역에, 카메라가 하단 영역에 합성됐는지(프레임의 상/하 밴드 픽셀 비교) 확인. 가로 프로젝트는 1920×1080 회귀 없음.
- [ ] **Step 2: 문서 갱신** — 스펙에 "구현 완료 + 실측(1080×1920, 상/하 배치)" 노트. build-verify 메모리에 세로 export 검증법 추가.
- [ ] **Step 3: 커밋** — `test(vertical): headless 9:16 export verifies portrait dims + layout`

---

## Self-Review
- **스펙 커버리지:** §4.1 방향선택→Task2+4; §4.2 자동적응→Task2(canvas)+3(배선)+5(검증); §4.3 기본레이아웃→Task3; §4.4 프리셋→Task1+4; §6 테스트→각 Task의 test + Task5 통합. 갭 없음.
- **플레이스홀더:** 좌표/치수는 구체값. "확인 후 배선"(Task2/3의 매니페스트·canvas dims 전달)은 코드 탐색 필요한 실제 배선 지점 — 구현 시 정확 경로 확정.
- **타입 일관성:** `verticalDefaultTransform(role, w, h)` Task3에서 정의·Task4/5에서 참조 일치; preset id 문자열 Task1↔4 일치(`h264-1080x1920p30`, `h264-720x1280p30`).
- **YAGNI:** 스튜디오 라이브 프리뷰의 세로화·가로↔세로 토글·모바일 UI는 제외(후속).
