# Creator Studio Starter Pack v0.1

화면 녹화, 카메라, 오디오, 버튜버, 비선형 편집을 하나의 프로젝트로 묶는 데스크톱 크리에이터 스튜디오의 개발 착수 패키지입니다.

이 저장소는 데모용 일회성 MVP가 아니라, 상용 제품까지 그대로 확장하는 것을 전제로 한 **초기 제품 골격**입니다.

## 이번 버전에서 고정한 핵심 결정

- 데스크톱: **Qt 6 / QML + C++20**
- 화면 캡처: 운영체제 네이티브 API
  - macOS: ScreenCaptureKit
  - Windows: Windows.Graphics.Capture
- 실시간 합성: Qt Quick Scene Graph + 네이티브 GPU 텍스처 어댑터
- 녹화·코덱: LGPL 구성의 FFmpeg 라이브러리
- 편집 백엔드: MLT Framework를 Adapter 뒤에 격리
- 프로젝트 저장: 소스 분리 녹화 + SQLite/JSON/NDJSON 기반 프로젝트 패키지
- 얼굴 추적: MediaPipe 기본, OpenSeeFace 선택
- 2D 아바타: Inochi2D
- 음성 인식: whisper.cpp
- 노이즈 제거: RNNoise 기본, DeepFilterNet 고품질 옵션

## 먼저 읽을 문서

1. `PRODUCT_BLUEPRINT.md` — 제품 정의, 기능, 사용자 흐름
2. `ARCHITECTURE.md` — 기술 구조와 데이터 흐름
3. `IMPLEMENTATION_ROADMAP.md` — 구현 순서와 완료 기준
4. `CLAUDE.md` — Claude Code/Codex가 반드시 지켜야 할 개발 규칙
5. `legal/OSS_BOM.csv` — 오픈소스 도입 및 라이선스 관리표
6. `prompts/01-bootstrap.md` — 첫 개발 작업지시서

## 현재 코드 골격

현재 코드는 Qt Quick 애플리케이션이 빌드될 수 있는 최소 골격과 도메인 인터페이스를 제공합니다. 아직 실제 화면 캡처·녹화 기능은 구현하지 않았습니다. 첫 구현 대상은 `prompts/01-bootstrap.md`와 `IMPLEMENTATION_ROADMAP.md`의 R0 항목입니다.

## 프로젝트 패키지 예시

```text
MyTutorial.cstudio/
├─ manifest.json
├─ project.db
├─ media/
│  ├─ screen/segment_000001.mkv
│  ├─ camera/segment_000001.mkv
│  └─ preview/segment_000001.mkv
├─ audio/
│  ├─ microphone/segment_000001.mka
│  └─ system/segment_000001.mka
├─ telemetry/
│  ├─ cursor.ndjson
│  ├─ keyboard.ndjson
│  ├─ scene.ndjson
│  └─ avatar-motion.ndjson
├─ proxies/
├─ thumbnails/
├─ autosave/
├─ renders/
└─ logs/
```

## 빌드 전제

- CMake 3.25 이상
- C++20 컴파일러
- Qt 6.8 이상 권장: Quick, QuickControls2, Multimedia

```bash
cmake -S . -B build
cmake --build build
```

Qt 경로를 찾지 못하면 `CMAKE_PREFIX_PATH`를 지정해야 합니다.

## 중요한 법적 주의

이 문서는 기술 설계이며 법률 자문이 아닙니다. Qt, MLT, FFmpeg와 각 플러그인의 실제 배포 구성은 출시 전 오픈소스 라이선스 전문가의 검토를 받아야 합니다. 특히 FFmpeg는 GPL 또는 nonfree 옵션이 포함되지 않은 고정 빌드를 사용해야 합니다.
