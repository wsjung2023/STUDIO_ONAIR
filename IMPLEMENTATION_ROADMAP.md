# Creator Studio 구현 로드맵 v0.1

이 로드맵은 버릴 MVP를 만드는 방식이 아니다. 각 릴리스는 동일한 아키텍처 위에서 기능과 안정성을 축적한다.

## R0 — Engineering Baseline

목적: 제품의 가장 위험한 부분인 캡처·동기화·저장·복구를 먼저 증명한다.

### R0-01 저장소와 빌드

- Qt/QML 앱 실행
- CMake Presets
- macOS/Windows CI 빌드
- clang-format, clang-tidy
- unit test
- 로그와 오류 타입

완료 기준:

- 깨끗한 환경에서 문서대로 빌드
- CI에서 앱과 테스트가 통과

### R0-02 프로젝트 패키지

- `.cstudio` 폴더 생성
- manifest JSON 스키마 검증
- SQLite 초기 마이그레이션
- recording session 생성·종료
- 세그먼트 상태 저장

완료 기준:

- 앱 강제 종료 후 프로젝트 재오픈
- 미완료 세션을 감지하고 복구 화면 표시

### R0-03 macOS 화면 캡처

- 화면·창 목록
- 권한 안내
- ScreenCaptureKit 프레임 수신
- monotonic timestamp 변환
- 프리뷰 표시

완료 기준:

- 1080p60 화면 프리뷰
- 창 크기·해상도 변경 대응
- 캡처 대상 종료 시 안전한 오류 표시

### R0-04 카메라와 오디오

- 카메라 장치 열거
- 마이크 입력
- 시스템 오디오
- 장치 hotplug
- 오디오 레벨 미터

완료 기준:

- 화면+카메라+마이크+시스템 오디오 동시 동작
- 장치 분리 시 앱이 종료되지 않음

### R0-05 소스 분리 녹화

- FFmpeg encoder adapter
- 2초 세그먼트
- 화면·카메라·오디오 별도 저장
- 합성 프리뷰 선택 녹화
- 디스크 공간 감시

완료 기준:

- 30분 테스트에서 각 트랙 재생 가능
- 세그먼트 인덱스와 실제 파일 일치

### R0-06 동기화와 복구

- ClockCoordinator
- drift 측정
- video drop/duplicate 정책
- audio resampling 보정
- 강제 종료 복구

완료 기준:

- 2시간 후 A/V drift 40ms 이하 목표
- 강제 종료 후 손실 2초 이하 목표
- 미완료 `.part` 파일을 안전하게 격리

## R1 — Usable Recorder & Basic Editor

목적: 실제 튜토리얼 한 편을 촬영하고 편집해 출력할 수 있다.

현재 전달 상태:

- [x] R1-01 타임라인 도메인, migration 002, split/trim/ripple 명령,
  durable Undo/Redo·autosave 기반
- [x] R1-02 Qt-free 편집 엔진 port, 결정적 fake, 비동기 Editor controller,
  미디어 빈·멀티트랙 view model과 model-driven QML
- [x] R1-03 감사된 MLT 동적 adapter와 실제 preview/playback
  - MLT 7.40.0 core/avformat runtime-only staging and SHA-256 verification
  - real multitrack composite/audio-mix graph, seek, bounded async playback
  - Unicode package and physical tamper acceptance coverage
- [ ] Studio 장면/source 편집과 녹화 결과의 타임라인 연결
- [ ] 텍스트·기본 자막, preview, export와 R1 물리 검증

위 체크는 R1을 축소한 것이 아니다. R1 완료 기준은 아래 Studio, Editor,
Export 항목과 30분 실제 촬영·편집·출력 검증을 모두 만족할 때까지 유지한다.

### Studio

- scene/source 패널
- transform inspector
- PIP 프리셋
- 장면 전환
- 단축키
- 마커
- 녹화 상태 HUD

### Editor

- 미디어 빈
- 멀티트랙 타임라인
- cut/split/trim/ripple
- 화면·카메라 transform
- 오디오 볼륨과 fade
- 텍스트·기본 자막
- Undo/Redo

### Export

- H.264 MP4
- 1080p/4K 프리셋
- 하드웨어 인코더 탐지
- 소프트웨어 폴백
- 진행률과 취소

완료 기준:

- 30분짜리 화면 강의 촬영
- 실수 구간 삭제
- 카메라 위치 변경
- 제목·자막 추가
- MP4 출력

## R2 — Creator Intelligence

목적: 일반 편집기와 다른 화면 녹화 전용 생산성을 만든다.

### 커서

- 좌표·클릭 이벤트 기록
- 클릭 강조
- 커서 숨기기
- 자동 줌 후보 생성
- 줌 구간 편집

### 자막·텍스트 편집

- whisper.cpp worker
- 문장·단어 타임스탬프
- transcript 패널
- 텍스트 삭제 → 비파괴 컷
- 침묵·필러 후보

### 오디오

- RNNoise 실시간
- DeepFilterNet 후처리
- compressor/limiter
- loudness normalize

완료 기준:

- 자막 생성부터 문장 편집까지 로컬에서 완료
- 자동 제안을 사용자가 승인·취소 가능
- AI 기능 실패 시 원본 프로젝트에 영향 없음

## R3 — Avatar Studio

목적: 얼굴 공개 없이 촬영하고, 녹화 후 아바타를 교체할 수 있다.

### Tracking

- MediaPipe provider
- OpenSeeFace provider
- calibration
- smoothing
- tracking confidence

### Inochi2D

- 모델 로딩
- 파라미터 매핑
- 투명 렌더
- Studio source
- motion telemetry 저장
- Editor 재생

완료 기준:

- 아바타를 화면 위에 실시간 합성
- 아바타 모션과 음성 동기화
- 편집 단계에서 모델 교체

## R4 — Commercial Hardening

### 안정성

- 2시간·4시간 soak test
- 메모리·핸들 누수 검사
- 장치 조합 테스트
- 저용량 디스크
- 절전·화면 잠금·모니터 변경
- 외장 카메라 연결 해제

### 배포

- macOS notarization
- Windows code signing
- 자동 업데이트
- 버전 롤백
- crash dump
- 진단 리포트

### 법률·라이선스

- SBOM 자동 생성
- 금지 라이선스 CI 검사
- About/Open Source Notices
- FFmpeg build configuration 기록
- 모델 가중치 해시·라이선스 관리

### 사용자 경험

- 첫 실행 장치 진단
- 권한 온보딩
- 녹화 실패 전 사전 경고
- 복구 센터
- 기본 템플릿
- 키보드 접근성

## 첫 30개 작업 백로그

1. Qt Quick 앱 부트스트랩
2. 로깅 규격
3. Result/Error 타입
4. 프로젝트 경로 서비스
5. manifest schema validator
6. SQLite migration runner
7. ProjectStore 인터페이스
8. RecordingSession 도메인
9. Segment 메타데이터
10. RecoveryScanner
11. DeviceRegistry 포트
12. CaptureSource 포트
13. VideoFrame 중립 타입
14. AudioBlock 중립 타입
15. Timebase/ClockMapper
16. macOS permission service
17. macOS display/window enumeration
18. ScreenCaptureKit source
19. PreviewFrameProvider
20. 카메라 source
21. 마이크 source
22. 시스템 오디오 source
23. FFmpeg build script
24. Encoder capability probe
25. SegmentWriter
26. DiskSpaceMonitor
27. CaptureStats
28. Studio 상태 화면
29. 30분 soak test
30. force-kill recovery test

## 개발 중 절대 미루면 안 되는 테스트

- 녹화 중 앱 강제 종료
- 녹화 중 디스크 부족
- 녹화 중 카메라 분리
- 캡처하던 창 종료
- 모니터 해상도 변경
- 블루투스 마이크 지연
- 59.94Hz/60Hz 혼합
- 시스템 절전 진입
- 외장 모니터 분리
- 프로젝트 폴더 이동

## 현실적인 위험 순위

1. 장시간 A/V 동기화
2. GPU texture zero-copy와 다양한 하드웨어
3. 장치 hotplug 및 권한
4. 녹화 복구
5. 편집 프리뷰 성능
6. MLT 플러그인별 동작·라이선스
7. 아바타 렌더와 캡처 프레임 동기화
8. 설치 패키지에 포함되는 코덱·모델 라이선스

## 중단 기준

다음 기반이 통과하기 전에 화려한 AI·템플릿 기능을 늘리지 않는다.

- 2시간 녹화
- A/V drift 기준
- 강제 종료 복구
- 메모리 누수 기준
- 프로젝트 재오픈
- 기본 편집·출력
