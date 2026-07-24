# 세로 9:16 쇼츠 프로젝트 모드 — 설계

- 상태: 사용자 승인 완료
- 승인일: 2026-07-25
- 대상 제품: Creator Studio (feat/ux-mobile)
- 선행: 기본기(녹화·편집·오디오·아바타) 완료. 이 스펙은 "모바일 폴리시"의 토대.

## 1. 목표

가로(16:9)와 **세로(9:16, 쇼츠)** 를 프로젝트 단위로 선택할 수 있게 한다. 세로를
고르면 캔버스·프리뷰·편집·내보내기 전체가 온전히 9:16으로 동작하고, 가로로 촬영한
소스(화면·카메라·아바타)가 세로 캔버스에 **화면 위 + 카메라/아바타 아래** 기본
레이아웃으로 합성된다. 각 클립은 편집기에서 드래그·리사이즈로 조정 가능하다.

목업이나 데모 전용 구조가 아니라, `새 프로젝트(세로) → 녹화 → 세로 합성 → 세로
MP4 내보내기`가 실제 제품 코드로 동작해야 완료다.

## 2. 범위

### 2.1 포함
- 새 프로젝트 생성 시 방향(가로/세로) 선택 UI + 배선.
- `CanvasSettings`를 방향에 따라 1920×1080(가로) 또는 1080×1920(세로)로 설정·영속.
- 세로 캔버스에서 편집기 프리뷰·MLT 프로파일·export가 9:16로 동작(자동 적응 확인).
- 세로 프로젝트로 소스 임포트 시 **역할 기반 기본 VisualTransform**(화면 위, 카메라/아바타 아래).
- 세로 내보내기 프리셋(1080×1920, 720×1280).
- 내보내기 UI가 프로젝트 방향에 맞는 프리셋을 노출.

### 2.2 제외
- 녹화 캡처 자체는 바뀌지 않는다(소스는 네이티브 해상도로 캡처; 세로는 합성/캔버스 속성).
- 기존 프로젝트의 가로↔세로 실시간 토글(생성 시 선택만; 토글은 클립 재배치·버그 여지 커서 후속).
- 모바일 전용 UI 폴리시(별도 후속 스펙).
- 세로용 자동 얼굴/피사체 추적 크롭.

## 3. 아키텍처 & 데이터 흐름

방향은 프로젝트 매니페스트의 `CanvasSettings`에 **1급 상태**로 저장되고, 이미
존재하는 canvas-dims 소비 경로들이 그것을 읽어 자동으로 세로가 된다.

```text
새 프로젝트(방향 선택)
   → ProjectController.createProject(orientation)
       → ProjectManifest.canvas = {1080×1920} (세로) | {1920×1080} (가로)
           → (저장) manifest.json
   녹화 → 세그먼트(네이티브 해상도, 변화 없음)
   임포트/reconcile → 클립 생성
       → 세로 캔버스면 역할별 기본 VisualTransform 적용
   편집기/프리뷰 (EditorController: aspect = canvasW/canvasH)  ← 자동 적응
   MLT 프로파일 (display_aspect = canvas 또는 preset dims)      ← 자동 적응
   내보내기 → 세로 RenderPreset(1080×1920) → 9:16 MP4
```

## 4. 컴포넌트 상세

### 4.1 방향 선택 (생성 시)
- `src/domain/ProjectManifest.h` `CanvasSettings{width,height,...}`: 구조 변경 없음.
  세로는 `width=1080, height=1920`.
- `ProjectController::createProject`에 방향 인자 추가(enum `ProjectOrientation{Landscape, Portrait}`
  또는 canvas dims 직접). 매니페스트 생성 시 canvas dims를 그에 맞게 설정.
- `qml/HomePage.qml` 새 프로젝트 화면에 방향 선택(세그먼트/라디오): **가로 16:9 /
  세로 9:16 (쇼츠)**. 기본값 가로(기존 동작 보존).
- typed value로: 매직 dims 대신 orientation → dims 매핑 헬퍼 한 곳.

### 4.2 캔버스 전파 (자동 적응 — 확인·배선)
- 편집기 프리뷰 aspect는 이미 `snapshot.canvasWidth/canvasHeight`에서 계산됨
  (`EditorController.cpp:88,302,633`). 세로면 세로 프리뷰가 나온다 — 확인 필요.
- MLT export 프로파일 display_aspect는 `ExportEncoderProbe.cpp:57`에서 **preset dims**
  로 설정됨. 세로 export가 preset(1080×1920)을 쓰면 자동으로 9:16. preview 엔진의
  캔버스도 `EditorSessionWorker.cpp:288` 에서 canvas dims를 넘김 — 세로 dims가 흐르는지 확인.
- 확인 포인트: 프리뷰/오버레이(자막·타이틀)가 canvas dims로 렌더되는지
  (`GeneratedOverlayCache.cpp` 은 canvasW/H를 받음 — 세로에서 좌표 정상인지 테스트).

### 4.3 세로 기본 레이아웃 (역할별 기본 VisualTransform)
VisualTransform 좌표계(FrameEffects.cpp): `x,y`=정규화 좌상단[0,1], `width,height`=
캔버스 대비 박스 비율. 세로 캔버스(1080×1920)에서 역할별 기본:

- **화면(Screen)**: 폭 맞춤 상단.
  `width=1.0`, `height = (1080 * 9/16) / 1920 ≈ 0.316`, `x=0`, `y≈0.04`(상단 여백).
  → 1080×608 16:9 화면이 위쪽 띠.
- **카메라(Camera)**: 아래쪽 띠, 폭 맞춤.
  `width=1.0`, `height≈0.316`, `x=0`, `y≈0.60`(화면 아래).
- **아바타(Avatar)**: 기존 코너 오버레이 배치 유지(작은 PiP). 세로에선 아래쪽
  띠 안 코너(예: 우하단)로 기본 배치되도록 오버레이 좌표만 세로 기준으로.

적용 지점: 소스가 타임라인에 임포트/reconcile될 때(예: `RecordingTimelineReconciler`
/ `RecordingImportPlanner`), 캔버스가 세로면 클립 role에 따라 위 기본 transform을
설정. 가로면 기존(identity/full-canvas) 유지. 상수는 한 곳(named)에서 관리, 매직 넘버 금지.
사용자가 이후 편집기에서 각 클립 transform을 덮어쓸 수 있다.

### 4.4 세로 내보내기 프리셋
- `src/edit_engine/EditEngineTypes.cpp` `RenderPreset`에 추가:
  - `h264-1080x1920p30`: 1080×1920, 30fps, video 12Mbps, audio 192kbps.
  - `h264-720x1280p30`: 720×1280, 30fps, video 8Mbps, audio 192kbps(경량/모바일).
- `qml/ExportPage.qml`: 프로젝트 방향(캔버스 종횡비)에 맞는 프리셋만/우선 노출.
  세로 프로젝트 → 세로 프리셋. (구현 시 방향 판별은 canvas dims에서.)

## 5. 오류 처리
- 잘못된 캔버스 치수(0, 최대 초과)는 `RenderPreset::create`/canvas 검증에서 거부(기존 경로 재사용).
- 방향 미지정 시 기본 가로(기존 동작 보존).
- 세로 프리셋으로 가로 프로젝트를 export하거나 그 반대여도 크래시 없이 동작(레터박스/필러)
  — 다만 UI가 방향-매칭 프리셋을 기본 제시해 실수 예방.

## 6. 테스트 (CLAUDE.md 8)
### 6.1 단위
- `CanvasSettings` 세로(1080×1920) 직렬화·역직렬화 라운드트립.
- `RenderPreset::h264_1080x1920p30()`/`h264_720x1280p30()` create + dims/bitrate 검증;
  잘못된 dims 거부.
- 역할별 기본 transform 계산: 세로 캔버스에서 화면 box가 상단·폭맞춤, 카메라 box가
  하단인지(좌표 단언). 가로에서는 기본(identity) 유지.
### 6.2 통합(헤드리스)
- 세로 프로젝트 생성 → 소스 임포트 → 기본 레이아웃 적용 → export.
  드라이버로 export한 MP4가 **1080×1920**(9:16)로 나오는지, 화면이 상단·카메라가
  하단에 합성됐는지(프레임 샘플 좌표/영역) 실측.
- 가로 프로젝트는 기존대로 1920×1080 export(회귀 없음).
### 6.3 로그/리소스
- 방향 선택·canvas dims가 로그/매니페스트에 정확히 기록되는지.
- 기존 export/편집 테스트 그린 유지(회귀 없음).

## 7. 구현 순서(계획 예고)
1. `RenderPreset` 세로 프리셋 + 단위 테스트(가장 격리·저위험).
2. `ProjectOrientation` → canvas dims + `ProjectController.createProject` 배선 + 매니페스트 라운드트립 테스트.
3. 역할별 기본 transform(named 상수 + 계산 함수) + 단위 테스트, 임포트 경로 배선.
4. HomePage 방향 선택 UI + ExportPage 방향-매칭 프리셋.
5. 헤드리스 세로 export 통합 검증(1080×1920, 상/하 배치).

## 8. 설계 결정 요약
- 방향은 **프로젝트 생성 시 선택**, `CanvasSettings`에 1급 저장(토글은 후속).
- 세로 기본 레이아웃 = **화면 위 + 카메라/아바타 아래**(편집으로 조정 가능한 기본값).
- 편집기·MLT·오버레이는 canvas dims 소비 경로를 **재사용**(신규 렌더 경로 없음).
- 녹화 캡처는 불변(세로는 합성/캔버스 속성).
- 모바일 폴리시는 이 위에 얹는 별도 후속.
