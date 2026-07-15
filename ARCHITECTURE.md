# Creator Studio 기술 아키텍처 v0.1

## 1. 최종 기술 결정

### 1.1 UI와 애플리케이션

- Qt 6 / QML
- C++20
- CMake
- Qt Quick Controls
- Qt Multimedia는 장치 열거·일반 미디어 기능에 활용하되, 핵심 화면 캡처는 OS 네이티브 구현 사용

### 1.2 캡처

- macOS: ScreenCaptureKit + CoreMedia/CoreAudio + Metal texture
- Windows: Windows.Graphics.Capture + WASAPI + D3D11 texture
- Linux: 첫 상용 범위에서 제외, 이후 PipeWire 구현

### 1.3 녹화와 코덱

- FFmpeg `libavformat`, `libavcodec`, `libavutil`, `libswresample`, `libswscale`
- LGPL 전용 빌드
- 하드웨어 인코더 Adapter
  - macOS VideoToolbox
  - Windows Media Foundation 또는 FFmpeg 하드웨어 래퍼
  - NVIDIA/Intel/AMD는 기능 탐지 후 사용

### 1.4 편집

- 제품 도메인 타임라인이 기준 모델
- MLT Framework는 `EditEngineAdapter` 뒤에 배치
- MLT XML은 내부 진실 원본이 아니라 실행·렌더용 파생물
- OpenTimelineIO는 외부 교환 기능으로만 사용

### 1.5 AI·아바타

- MediaPipe: 기본 얼굴·손·자세 추적
- OpenSeeFace: 저사양 CPU 선택 엔진
- Inochi2D: 2D 아바타
- whisper.cpp: 로컬 자막
- RNNoise: 실시간 저비용 노이즈 억제
- DeepFilterNet: 고품질 후처리 또는 고성능 장치 옵션

## 2. 왜 GStreamer를 핵심에서 제외했는가

GStreamer는 훌륭하지만 현재 선택한 구성에서 FFmpeg와 MLT가 이미 코덱·입출력·편집 기능을 담당한다. 세 프레임워크를 동시에 핵심 엔진으로 사용하면 다음 문제가 생긴다.

- 프레임 포맷 변환 중복
- 플러그인 배포와 라이선스 추적 복잡성
- 시간 기준과 버퍼 모델의 중복
- 크래시 원인 분석 어려움
- 바이너리 크기 증가

따라서 1차 제품은 `Native Capture + Compositor + FFmpeg + MLT`로 고정한다. GStreamer는 향후 스트리밍, 가상 카메라, 외부 장치 파이프라인에서 이점이 명확할 때 플러그인 프로세스로 검토한다.

## 3. 전체 구조

```text
┌──────────────────────────────────────────────────────┐
│                    Qt/QML Desktop                    │
│ Home | Studio | Editor | Avatar | Export | Settings │
└───────────────────────┬──────────────────────────────┘
                        │ Commands / Queries
┌───────────────────────▼──────────────────────────────┐
│                  Application Layer                   │
│ ProjectController | StudioController | EditController│
│ UndoStack | JobQueue | Settings | Diagnostics        │
└────────────┬──────────────┬───────────────┬──────────┘
             │              │               │
┌────────────▼─────┐ ┌──────▼──────┐ ┌──────▼──────────┐
│ Capture Service │ │ Avatar       │ │ AI Services     │
│ Devices/Frames  │ │ Tracking     │ │ STT/Denoise     │
└────────────┬─────┘ └──────┬──────┘ └──────┬──────────┘
             │              │               │
┌────────────▼──────────────▼───────────────▼──────────┐
│                   Frame / Audio Bus                   │
│ Timebase | Buffer ownership | Backpressure | Metrics │
└────────────┬───────────────────────────┬──────────────┘
             │                           │
┌────────────▼───────────┐   ┌───────────▼─────────────┐
│ GPU Scene Compositor   │   │ Source Track Recorder   │
│ Canvas preview         │   │ FFmpeg + segment writer │
└────────────┬───────────┘   └───────────┬─────────────┘
             │                           │
             └────────────┬──────────────┘
                          │
┌─────────────────────────▼────────────────────────────┐
│                    Project Store                      │
│ SQLite | JSON manifest | Media segments | Telemetry  │
└─────────────────────────┬────────────────────────────┘
                          │
┌─────────────────────────▼────────────────────────────┐
│                    Edit Engine                        │
│ Domain Timeline → MLT Adapter → Preview / Export     │
└──────────────────────────────────────────────────────┘
```

## 4. 모듈 책임

### 4.1 `core`

- 시간 기준
- 오류 타입
- 로깅
- 작업 큐
- 취소 토큰
- 성능 메트릭

### 4.2 `domain`

- Project
- Scene
- Source
- Track
- Clip
- Effect
- Keyframe
- Transcript
- AvatarMotion
- ExportPreset

도메인 계층은 FFmpeg, Qt Multimedia, MLT 타입을 노출하지 않는다.

### 4.3 `capture`

```cpp
class ICaptureSource {
public:
    virtual SourceId id() const = 0;
    virtual Result<void> start(const CaptureConfig&) = 0;
    virtual Result<void> stop() = 0;
    virtual CaptureStats stats() const = 0;
    virtual ~ICaptureSource() = default;
};
```

세부 구현:

- `MacScreenCaptureSource`
- `WindowsScreenCaptureSource`
- `CameraCaptureSource`
- `MicrophoneCaptureSource`
- `SystemAudioCaptureSource`

### 4.4 `media`

- VideoFrame, AudioBlock의 내부 중립 표현
- 하드웨어 텍스처 핸들
- CPU 폴백 프레임
- 색공간 메타데이터
- 오디오 샘플 포맷
- 소유권과 수명 관리

### 4.5 `compositor`

- SourceNode 그래프
- transform, crop, mask, opacity, blend
- 캔버스 출력
- 프리뷰와 합성 녹화 출력
- 화면 렌더와 인코더 입력이 동일한 장면 모델을 사용

### 4.6 `recorder`

- 소스별 인코더
- 세그먼트 파일 작성
- 인덱스 체크포인트
- 저장 공간 감시
- 오류 발생 시 안전 종료
- 녹화 중 프록시 생성은 성능 조건을 통과한 장치에서만 활성화

### 4.7 `project-store`

- SQLite WAL 모드
- 명령 이벤트와 Undo 정보
- 원본 미디어 참조
- 자동 저장
- 프로젝트 마이그레이션
- 복구 스캔

### 4.8 `edit-engine`

- 도메인 타임라인을 MLT 구성으로 변환
- 프레임 요청
- 오디오 믹스
- 렌더 작업
- MLT 오류를 제품 오류로 변환
- MLT 플러그인 화이트리스트

### 4.9 `avatar`

- TrackingProvider 인터페이스
- MediaPipe/OpenSeeFace 구현
- 표정 파라미터 정규화
- Inochi2D 파라미터 매핑
- 캘리브레이션 프로필
- 모션 이벤트 기록·재생

### 4.10 `transcription`

- 오디오 추출
- whisper.cpp 실행
- 단어·문장 타임스탬프
- 언어 감지
- 자막 캐시
- 편집 구간과 자막의 양방향 연결

## 5. 시간과 동기화

### 5.1 공통 시간 기준

모든 미디어와 이벤트는 프로젝트의 monotonic timebase에 매핑한다.

```text
project_time_ns = normalized_source_time + source_offset_ns
```

벽시계 시간은 로그와 사용자 표시용으로만 사용한다. 시스템 시간이 바뀌어도 녹화 동기화에 영향을 주면 안 된다.

### 5.2 동기화 정책

- 오디오는 연속성이 가장 중요하므로 master stream으로 취급
- 비디오는 필요 시 frame duplicate/drop으로 목표 타임라인에 정렬
- 장시간 장치 clock drift는 오디오 resampling ratio로 미세 조정
- 모든 조정량은 진단 로그에 기록

### 5.3 목표 품질

- 2시간 후 A/V drift 절대값 40ms 이하
- 정상 장치에서 dropped video frame 0.1% 미만
- 프리뷰 P95 지연 120ms 이하
- 강제 종료 시 손실 최대 2초 목표
- 메모리 증가 기울기 5MB/시간 이하 목표

이 수치는 제품 승인 목표이며 라이브러리의 보장 수치가 아니다.

## 6. 녹화 파일 전략

### 6.1 왜 단일 MP4에 바로 쓰지 않는가

장시간 녹화 중 앱이나 시스템이 종료되면 단일 파일의 인덱스가 완성되지 않을 수 있다. 복구 가능성을 높이기 위해 짧은 세그먼트와 프로젝트 인덱스를 사용한다.

### 6.2 세그먼트 정책

- 기본 세그먼트 길이: 2초
- 컨테이너: MKV/MKA
- 세그먼트 완료 후 원자적 rename
- SQLite에 완료 상태 기록
- 정상 종료 시 선택적으로 단일 파일로 remux
- 편집기는 세그먼트를 하나의 연속 소스로 취급

```text
.tmp/segment_000421.mkv.part
        ↓ close + fsync
media/screen/segment_000421.mkv
        ↓ transaction
project.db: segment status = READY
```

### 6.3 백프레셔

인코더가 따라오지 못할 때 메모리를 무한히 쌓지 않는다.

- 프리뷰 프레임은 최신 프레임 우선으로 폐기 가능
- 녹화 화면 프레임은 정책에 따라 drop하고 카운트
- 오디오는 ring buffer와 짧은 대기 후 오류 처리
- 디스크 쓰기 지연이 임계치를 넘으면 사용자에게 즉시 경고

## 7. 프로젝트 저장 포맷

### 7.1 폴더 패키지

작업 중 프로젝트는 ZIP이 아니라 폴더로 유지한다. 그래야 대용량 파일을 매번 다시 압축하지 않고 충돌 복구가 가능하다. 공유 시에만 `.cstudiozip`으로 패킹한다.

### 7.2 manifest.json

- 포맷 버전
- 프로젝트 ID
- 생성·수정 시각
- 캔버스 설정
- 기본 프레임레이트
- 데이터베이스 파일
- 요구 기능

### 7.3 project.db

주요 테이블 초안:

```sql
projects
scenes
sources
recording_sessions
media_assets
segments
tracks
clips
effects
keyframes
transcripts
transcript_words
commands
markers
jobs
migrations
```

### 7.4 telemetry

대량 이벤트는 NDJSON으로 순차 기록한다.

```json
{"t_ns":1023400000,"type":"cursor.move","x":0.421,"y":0.337,"source_id":"screen-1"}
{"t_ns":1291100000,"type":"cursor.click","button":"left","x":0.421,"y":0.337}
{"t_ns":1820000000,"type":"scene.changed","scene_id":"scene-b"}
```

좌표는 원본 픽셀과 정규화 좌표를 함께 저장할 수 있다.

## 8. GPU와 프레임 경로

### 8.1 목표 경로

```text
OS Capture Texture
 → Native Texture Adapter
 → Compositor GPU Node
 → Preview Surface
 → Encoder Hardware Frame
```

CPU 메모리로 내려갔다 다시 GPU로 올리는 round trip을 기본 경로로 삼지 않는다.

### 8.2 폴백

모든 GPU 조합을 처음부터 완벽히 지원할 수 없으므로 CPU 폴백을 둔다. 다만 폴백 상태를 숨기지 않고 진단 화면에 표시한다.

### 8.3 색 관리

첫 버전은 SDR Rec.709를 기준으로 한다. HDR은 캡처·프리뷰·편집·출력 전 구간의 색공간 관리가 필요하므로 별도 릴리스로 둔다.

## 9. 편집 엔진 경계

### 9.1 Domain Timeline

제품 내부의 기준 모델이다.

```text
Timeline
 ├─ VideoTrack
 │   └─ Clip(asset, sourceRange, timelineRange, transform, effects)
 ├─ AudioTrack
 └─ CaptionTrack
```

### 9.2 MLT Adapter

- Domain Timeline을 MLT producer/playlist/tractor/filter로 변환
- MLT XML 생성은 캐시일 뿐 원본이 아님
- 제품의 Undo, 협업, 마이그레이션은 MLT XML에 의존하지 않음

### 9.3 FFmpeg Adapter

FFmpeg 타입은 `ffmpeg-adapter` 내부에서만 사용한다.

- Decoder
- Encoder
- Muxer
- Remuxer
- AudioResampler
- PixelConverter
- HardwareDevice

## 10. 프로세스 격리

장기적으로 다음 작업은 별도 worker process로 격리한다.

- 최종 렌더
- whisper.cpp 대형 모델
- DeepFilterNet 후처리
- 썸네일·프록시 대량 생성
- 불안정한 외부 플러그인

메인 UI 프로세스가 worker 오류로 종료되지 않도록 한다.

## 11. 보안과 개인정보

- 녹화 데이터 기본 저장 위치를 명확히 표시
- 클라우드 업로드 기본 OFF
- 진단 로그에 파일 내용·키 입력·음성 내용 저장 금지
- 비밀번호 입력 필드 감지를 보장할 수 없으므로 키 문자 기록 기능은 기본 제공하지 않음
- 프로젝트 암호화는 기업 버전 확장
- AI 모델 다운로드 시 해시 검증

## 12. 플랫폼 출시 순서

### 12.1 macOS 첫 구현

개발 장비에서 바로 테스트할 수 있고 ScreenCaptureKit의 화면·오디오 캡처를 활용할 수 있다. Apple Silicon에서 GPU·하드웨어 인코딩 경로를 먼저 안정화한다.

### 12.2 Windows 두 번째 구현

동일한 도메인·recorder·project-store를 유지하고 capture 및 GPU adapter만 교체한다. Windows.Graphics.Capture, WASAPI, D3D11 경로를 추가한다.

### 12.3 Linux

상용 v1 이후 수요가 확인되면 PipeWire 기반으로 추가한다.

## 13. 관측성과 진단

녹화 제품은 재현이 어려운 장치 문제가 많으므로 다음을 내장한다.

- 장치·드라이버·인코더 정보
- 소스별 FPS, queue depth, dropped frames
- 오디오 underrun/overrun
- 디스크 write latency
- GPU/CPU 폴백 여부
- clock drift 보정량
- 마지막 5분 ring log
- 사용자가 내보낼 수 있는 진단 ZIP

## 14. 의존성 규칙

```text
qml/ui → application → domain
application → capture/media/edit/avatar ports
platform adapters → ports
ffmpeg/mlt/mediapipe implementation → adapters only
```

금지:

```text
QML → FFmpeg 직접 호출
Domain → Qt/MLT 타입 참조
Capture adapter → Project DB 직접 쓰기
Editor → 녹화 세그먼트 파일 직접 수정
```
