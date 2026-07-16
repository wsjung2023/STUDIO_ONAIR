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

R0-01과 R0-02가 완료되었고 R0-03 macOS 화면 캡처와 R0-04 카메라·오디오 캡처
코드는 현재 브랜치에 구현되어,
애플리케이션(`creator_studio.exe`)에서 다음 흐름이 연결됩니다.

- Home에서 로컬 `.cstudio` 프로젝트 생성·열기와 최근 프로젝트 표시
- JSON Schema 검증을 거친 `manifest.json`과 SQLite `project.db` 생성
- 프로젝트가 열린 뒤에만 Studio로 이동하고 Record/Stop 허용
- 녹화 시작 전 세션 상태를 SQLite에 먼저 기록하고, 종료 저장이 성공한 뒤에만 `Stopped` 표시
- 비정상 종료로 남은 세션을 시작 시 스캔하고 Recovery 화면에서 복구 또는 나중에 처리
- 카메라·마이크 장치 열거, 독립 권한 요청, hotplug 격리, 시스템 오디오, 입력 레벨 표시
- Studio의 test pattern, fake 녹화 세션, 세그먼트 개수와 duration 표시

프로젝트 URL은 반드시 **로컬 파일시스템 경로**여야 합니다. 원격 URL이나 네트워크 API는 지원하지 않습니다. 저장 계층은 SQLite **3.53.3**과 pboettch JSON Schema Validator **2.4.0**을 고정 버전으로 사용합니다.

2026-07-16 이 Windows 머신에서 R0-04 최종 리뷰 수정 후 Debug 빌드는 경고를 오류로
처리해 성공했습니다. **292개 테스트가 발견되어 291개 통과, 1개 조건부 skip**이었습니다.
Release 빌드도 리뷰 수정 전 R0-04 전체에서 경고 없이 성공했지만, 이 Windows 머신의
Application Control이 새 Release 테스트 실행 파일과 crash-recovery 자식 fixture를 간헐적으로
차단했습니다. 동일한 실제 프로세스 사망 복구 테스트는 Debug에서 통과합니다. symlink skip은 일반 권한
Windows에서 fixture를 만들 수 없어 발생하며, 같은 외부 파일 연결 위험은 권한이 필요 없는
hard-link 테스트로 통과했습니다. CI 워크플로는 아직 원격에서 실행되지 않았고 macOS
빌드도 실제 머신에서 검증되지 않았습니다.

macOS ScreenCaptureKit 프레임 수신, Metal zero-copy 프리뷰, AVFoundation 카메라·마이크,
ScreenCaptureKit 시스템 오디오 경로는 구현했지만, 이 저장소를 작업한 Windows 머신에서는
Apple 전용 코드를 컴파일하거나 실기 측정할 수 없어 R0-03/R0-04 제품 완료 증거로 간주하지
않습니다. 실제 미디어 바이트 기록·인코딩과 비선형 편집은 아직 없으며,
Record/Stop 녹화 경로는 R0-05 전까지 결정론적 fake source/recorder를 사용합니다.

모듈은 CMake static library로 분리되어 있고, application 계층 아래(`cs_core`, `cs_domain`, `cs_media`, `cs_capture`, `cs_recorder`, `cs_project_store`, `cs_fakes`)는 Qt를 링크하지 않습니다. `cmake/CreatorStudioTargets.cmake`의 `cs_add_qtfree_library()`가 configure 단계에서 이를 강제합니다.

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
- Ninja
- C++20 컴파일러
  - Windows: Visual Studio 2022 Build Tools (MSVC v143)
  - macOS: Xcode 명령줄 도구
- Qt 6.8 이상: Quick, QuickControls2, Multimedia

Qt는 공식 온라인 설치 프로그램이나 [aqtinstall](https://github.com/miurahr/aqtinstall)로 설치합니다.

```bash
pip install aqtinstall
# Windows
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtmultimedia qtshadertools --outputdir C:\Qt
# macOS
python -m aqt install-qt mac desktop 6.8.3 clang_64 -m qtmultimedia qtshadertools --outputdir ~/Qt
```

## 빌드

Qt 경로를 `CMAKE_PREFIX_PATH`로 알려줍니다. 머신마다 다르므로 프리셋에 넣지 않았습니다.

**Windows** — MSVC 환경이 필요합니다. 아래 명령은 "x64 Native Tools Command Prompt for VS 2022"에서, 또는 일반 `cmd.exe`를 열어 그대로 실행하면 동작을 확인했습니다(Windows 11, MSVC 14.44, Qt 6.8.3, CMake 3.31.6, Ninja 1.12.1).

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set CMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%

cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

겪어보지 않으면 놓치기 쉬운 것이 두 가지 있습니다.

- `vcvars64.bat`는 반드시 `call`로 실행합니다. 이 블록을 `.bat` 스크립트 파일로 저장해 실행할 경우, `call` 없이 부르면 vcvars64.bat 실행 후 나머지 줄이 조용히 실행되지 않고 스크립트가 끝나버립니다(대화형 프롬프트에 한 줄씩 입력할 때는 `call` 유무가 문제되지 않지만, 스크립트로 저장하는 사람이 있으므로 항상 붙입니다).
- `ctest` 전에 Qt의 `bin`을 PATH에 추가해야 합니다. `cs_app_tests`와 `cs_qml_tests`가 Qt를 링크하기 때문입니다. 빠뜨리면 Qt 기반 테스트가 `Exit code 0xc0000135`(DLL을 찾을 수 없음)로 실패합니다 — 빌드는 성공하고 ctest만 실패하므로 원인을 오해하기 쉽습니다.

**PowerShell을 쓴다면**: 위 블록은 `cmd.exe` 문법(`set VAR=값`)이라 PowerShell에 그대로 붙여 넣으면 안 됩니다(`set`이 `Set-Variable`로 해석되어 환경 변수가 설정되지 않습니다). 그리고 `vcvars64.bat`를 PowerShell에서 직접 실행하면(`& $vcvars`) 배치 파일이 자식 `cmd.exe` 프로세스에서 실행되므로, 그 프로세스가 끝나는 순간 환경 변수 변경도 함께 사라져 PowerShell 세션에는 반영되지 않습니다. 두 가지 중 하나를 씁니다.

1. 시작 메뉴의 "Developer PowerShell for VS 2022"를 엽니다.
2. 일반 PowerShell이라면 VS가 제공하는 스크립트로 같은 세션에 환경을 불러옵니다(동작 확인함):

   ```powershell
   & "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64
   $env:CMAKE_PREFIX_PATH = "C:\Qt\6.8.3\msvc2022_64"
   $env:PATH = "C:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"

   cmake --preset windows-debug
   cmake --build --preset windows-debug
   ctest --preset windows-debug
   ```

Git Bash에서는 `cmd //c '...'` 형태로 감싸 한 줄에 몰아 넣지 않습니다. MSYS가 인용부호로 감싼 vcvars 경로를 다시 써버려(mangles argv) 실패합니다. Git Bash를 쓰고 싶다면 위 cmd.exe 또는 PowerShell 블록을 별도 창에서 실행하십시오.

**macOS** — 이 저장소를 만든 머신에는 macOS가 없어 아래 명령은 실행해 보지 못했습니다. 프리셋 정의와 Qt/CMake 문서로부터 구성한 것이며, CI의 `macos-*` 잡이 사실상 첫 실행입니다.

```bash
export CMAKE_PREFIX_PATH=~/Qt/6.8.3/macos

cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

프리셋은 `windows-debug`, `windows-release`, `macos-debug`, `macos-release` 네 가지입니다. 모두 경고를 오류로 취급합니다.

앱을 실행하려면 Qt의 `bin`이 PATH에 있어야 합니다(동작 확인함: Studio/Editor 화면 전환, Record/Stop 모두 정상).

```bat
set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%
build\windows-debug\creator_studio.exe
```

## 중요한 법적 주의

이 문서는 기술 설계이며 법률 자문이 아닙니다. Qt, MLT, FFmpeg와 각 플러그인의 실제 배포 구성은 출시 전 오픈소스 라이선스 전문가의 검토를 받아야 합니다. 특히 FFmpeg는 GPL 또는 nonfree 옵션이 포함되지 않은 고정 빌드를 사용해야 합니다.
