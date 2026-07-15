# Claude Code 첫 작업지시서 — Repository Bootstrap

당신은 Creator Studio의 수석 데스크톱 미디어 엔지니어다.

## 목표

현재 저장소를 macOS와 Windows에서 빌드 가능한 Qt 6/QML + C++20 애플리케이션 기반으로 완성한다. 이번 작업에서는 실제 ScreenCaptureKit 또는 Windows.Graphics.Capture를 구현하지 않는다. 이후 네이티브 캡처가 안정적으로 들어갈 수 있도록 경계와 테스트를 확정한다.

## 반드시 먼저 읽을 문서

- `PRODUCT_BLUEPRINT.md`
- `ARCHITECTURE.md`
- `IMPLEMENTATION_ROADMAP.md`
- `CLAUDE.md`
- `schemas/project.schema.json`
- `schemas/event.schema.json`

## 구현 범위

1. CMake Presets 추가
   - macOS Debug/Release
   - Windows Debug/Release
2. Qt Quick 앱이 Home/Studio/Editor 3개 화면을 전환
3. 다음 도메인 value object 구현
   - ProjectId
   - SourceId
   - SessionId
   - TimestampNs
   - DurationNs
4. `Result<T>`와 `AppError` 구현
5. 인터페이스 구현
   - `IProjectStore`
   - `ICaptureSource`
   - `IRecorder`
6. in-memory fake 구현
   - FakeCaptureSource가 60fps timestamp만 발생
   - FakeRecorder가 segment metadata를 메모리에 기록
7. Project manifest 생성·읽기
8. unit test
   - 시간 단위
   - ID 직렬화
   - manifest round-trip
   - fake recording session start/stop
9. CI workflow 초안
10. README의 빌드 명령 검증

## UI 요구

Studio 화면은 아직 실제 영상 대신 test pattern을 표시한다.

- 좌측 Scenes/Sources
- 중앙 Preview
- 우측 Inspector
- 하단 Audio/Stats placeholder
- Record 버튼을 누르면 fake session 시작
- Stop 버튼을 누르면 segment 개수와 duration 표시

## 품질 기준

- QML은 application controller만 호출한다.
- raw pointer ownership 금지
- UI thread block 금지
- domain에 Qt Multimedia/FFmpeg/MLT include 금지
- warning을 error로 취급 가능한 상태
- 모든 테스트 통과

## 이번 작업에서 하지 말 것

- FFmpeg 다운로드 자동화
- MLT 연결
- ScreenCaptureKit
- Windows.Graphics.Capture
- MediaPipe
- whisper.cpp
- 임시로 Electron/Tauri 추가

## 결과 보고 형식

1. 변경 파일
2. 아키텍처 경계
3. 빌드 결과
4. 테스트 결과
5. 다음 작업 `R0-02 Project Package`에서 필요한 사항
