# ADR-0004: Avatar Tracking Core (Stage A — Port, Fake, Telemetry)

## Status
Accepted (Stage A only; Stage B/C deferred — see below)

## Decision
Ship the avatar-tracking boundary before any real tracking engine: a Qt-free
`cs_avatar` module (`src/avatar/`) defining `ITrackingProvider`, provider-neutral
`ExpressionParameters`/`TrackingResult` value types, offset-removal
`CalibrationProfile`, `ExpressionNormalizer`, and an `avatar.motion` telemetry
pipeline (`AvatarMotionSample` → `AvatarMotionSerializer` →
`AvatarMotionNdjsonSink`, appending to `<package>/telemetry/avatar-motion.ndjson`).
No real tracking engine is implemented in this stage. The port is proven
end-to-end by `FakeTrackingProvider` (`src/fakes/`), a deterministic scripted
provider that drives a pipeline test producing schema-valid NDJSON with no ML,
camera, or GPU involved.

## Rationale

### 왜 포트 + fake부터인가
CLAUDE.md 10은 "acceptance criteria를 테스트로 먼저 표현"하고 작은 buildable
commit 단위로 진행할 것을 요구한다. 실제 추적 엔진(MediaPipe/OpenSeeFace)은
네이티브 의존성 감사, 모델 다운로드 해시 검증(ARCHITECTURE §11), 라이선스
정책 확인을 필요로 하는 무거운 작업이다. 이를 성급하게 붙이기 전에, 도메인이
기대하는 데이터 모양(`ExpressionParameters`의 아홉 필드, 정규화 범위,
`TrackingResult`, 캘리브레이션 의미론, telemetry 스키마)을 먼저 고정하면 이후
엔진 교체가 어댑터 경계 안에서만 일어난다.

### Fake가 픽셀을 무시하는 이유
`ITrackingProvider::process()`는 정직하게 `media::VideoFrame`을 받는다 — 실제
구현은 픽셀을 읽는다. 하지만 `FakeTrackingProvider`는 스크립트된 결과를
반환할 뿐, 프레임의 픽셀(`platformHandle`)은 전혀 들여다보지 않고 오직
`timestamp`만 읽는다. 이는 R0-01의 `FakeCaptureSource`가 클록을 읽지 않고
스레드도 스폰하지 않는 것과 같은 규율을 그대로 반복한 것이며, 덕분에 파이프
라인 전체(정규화 → 캘리브레이션 → 직렬화 → NDJSON append)를 결정론적으로
증명할 수 있었다 — 동일한 스크립트는 매 실행마다 바이트 단위로 동일한 출력을
낸다.

### 탐색으로 발견한 세 가지 장애물 (Stage C로 이연)
Stage A 작업 중 현재 capture 레이어를 조사한 결과, 실제 카메라 → 트래커
배선을 지금 붙이는 것을 막는 세 가지 독립적인 장애물을 발견했다:

1. `media::VideoFrame`은 불투명 GPU `platformHandle`만 노출하고, 이식 가능한
   CPU 픽셀 접근자가 없다 — 현재 실제 CPU readback은 macOS 전용이며
   `ffmpeg_adapter` 안에 있다.
2. Capture의 `VideoFrameFanoutSink`는 primary(프리뷰) + secondary(녹화) 슬롯
   두 개만 가진다 (`src/capture/CaptureFanoutSinks.h`) — 트래커는 세 번째
   소비자인데 꽂을 슬롯이 없다.
3. Windows에는 아직 실제 카메라 캡처 백엔드가 없다 (macOS AVFoundation만
   존재).

실카메라 → 트래커 배선, GPU CPU-readback, 세 번째 fanout 슬롯, Studio 소스로서의
Inochi2D 렌더, Editor 모션 재생은 모두 Stage C이며 R1의 capture/timeline 작업에
게이팅된다.

### Stage B는 OpenSeeFace-first
실제 엔진 작업은 `legal/OSS_BOM.csv`의 모델 가중치 상업적 사용 라이선싱
확인에 게이팅된다 (LICENSE_POLICY는 상업적 사용이 불분명한 AI 모델 가중치를
금지하고, ARCHITECTURE §11은 "AI 모델 다운로드 시 해시 검증"을 요구한다).
OpenSeeFace를 MediaPipe보다 먼저 하는 것을 권고한다: OpenSeeFace는 자체
카메라를 잡고 UDP로 추적 결과를 스트리밍하는 **별도 프로세스**로 동작하므로
위의 세 장애물을 모두 우회하고, ARCHITECTURE §10(프로세스 격리) 및
OSS_BOM의 "Separate worker preferred" 메모와도 맞아떨어진다. MediaPipe(인프로세스
C++)는 FFmpeg/MLT에 썼던 감사된-네이티브-의존성 절차(게이팅된 부트스트랩 →
해시 검증 → 정책 테스트) 전체에 더해 CPU 픽셀 경로까지 필요해 더 무거우므로,
OpenSeeFace로 파이프라인을 먼저 증명한 뒤 진행한다는 권고 자체는 유효하다.

**단, 이 프로세스 격리는 Stage A가 고정한 `ITrackingProvider::process(const
VideoFrame&)` 포트와 그대로 맞물리지 않는다.** 이 포트는 정직하게 **인프로세스,
프레임-소비(pull)** 계약이다: 호출자가 프레임을 건네고 타임라인을 소유하며,
provider는 그 프레임의 timestamp만 읽는다. MediaPipe는 이 모양에 그대로
들어맞는다. 반면 OpenSeeFace를 우회하게 만든 바로 그 특성 — 별도 프로세스,
자체 카메라, 비동기 UDP 스트리밍, 자체 timestamp — 은 동시에 이 포트가 요구하는
"앱이 프레임을 건넨다"는 전제와 맞지 않는다: OpenSeeFace 어댑터가
`ITrackingProvider`를 구현하려 하면 건네받은 프레임을 무시하고 timestamp를
지어내야 한다. 따라서 Stage B는 다음 중 하나를 선택해야 한다: (a) OpenSeeFace의
실제 UDP 와이어 프로토콜을 확인한 뒤, 그에 맞춰 별도의 **자체-구동(self-driven)
tracking-source 포트**를 새로 정의하고 OpenSeeFace 어댑터를 그 위에 붙이거나,
(b) 프로토콜이 아직 불확실하다면 이 pull 포트에 맞는 MediaPipe로 먼저
시작한다. 프로토콜을 모르는 채로 push/streaming 포트 모양을 지금 추측해서
만들어두는 것은 R0-01이 `ICaptureSource`의 프레임 전달 방식을 미룬 것과 같은
이유로 피한다 — 이는 Stage A의 범위가 아니며, 실제 포트 설계는 Stage B에서
프로토콜을 확인한 뒤 결정한다.

### Telemetry는 SQLite가 아니라 package telemetry/ NDJSON
새 DB 테이블은 forward-only migration을 필요로 하는데, 이는 R1이 진행 중인
timeline 스키마 작업과 충돌한다. 이미 만들어진 `telemetry/` 디렉터리로의
NDJSON append는 어떤 migration/DB도 건드리지 않으며, "캡처/트래킹 어댑터가
Project DB에 직접 쓰지 않는다"는 규칙(CLAUDE.md 6, ARCHITECTURE.md §14)을
그대로 지킨다. `AvatarMotionSerializer`/`AvatarMotionNdjsonSink`가 함께 그
경계를 이룬다: 직렬화기는 스키마 위반(음수 `tNs`)을 조용히 clamp하지 않고
그대로 내보내며, sink가 쓰기 경계에서 이를 거부한다 — CLAUDE.md 9("오류를
조용히 무시하지 않는다")를 두 계층 모두에서 지킨다.

## Consequences
- `cs_avatar`는 Qt/FFmpeg/MLT/MediaPipe 타입을 포함하지 않는 순수 도메인
  인접 모듈이다 (CLAUDE.md 4, 5).
- `TrackingResult.confidence`는 이번 Stage A에서 캡처만 되고 아무 곳에서도
  소비되지 않는다 (`ExpressionNormalizer`는 `faceFound`만 읽고, `avatar.motion`
  telemetry에는 confidence 슬롯이 없다). 의도적으로 죽은 필드가 아니라 Stage B
  예약 필드다 — 실제 provider가 붙으면 정규화 게이팅(낮은 confidence를
  `!faceFound`처럼 취급) 및/또는 telemetry 기록에 쓰일 수 있다. 이번 단계에서
  threshold 동작이나 스키마 슬롯을 추가하지는 않는다.
- 앱은 아직 실제 얼굴을 추적하지 않는다 — Stage A는 경계 뒤의 port + fake
  연구이며, 실제 트래킹 엔진 통합(Stage B)과 라이브 카메라/Studio/Editor
  연동(Stage C)은 이 ADR이 다루지 않는다.
- Stage B 전, `legal/OSS_BOM.csv`의 OpenSeeFace/MediaPipe 항목에 모델 가중치
  라이선스 재확인이 필요하다 (현재 노트: "recheck bundled third parties" /
  "Check model and third-party notices separately").
- `avatar.motion` NDJSON은 project.db 마이그레이션과 독립적이지만, 언젠가
  타임라인 재생/편집이 이 데이터를 필요로 하면 DB로의 인덱싱 또는 참조가
  추가로 필요할 수 있다 — 그 결정은 Stage C로 미룬다.

## 이번에 하지 않은 것 (Stage B/C)

- **실제 트래킹 엔진** (MediaPipe, OpenSeeFace 구현체) — 라이선스/모델 해시
  검증 게이트, Stage B.
- **자체-구동(self-driven) tracking-source 포트** (OpenSeeFace처럼 별도
  프로세스가 자체 카메라를 잡고 비동기로 결과를 스트리밍하는 provider를 위한
  포트) — 현재의 `ITrackingProvider` pull 포트로는 표현할 수 없지만, 실제
  와이어 프로토콜을 모르는 채로 지금 설계하지 않는다. Stage B에서 프로토콜
  확인 후 정의한다.
- **캘리브레이션 gain/rescale** — `CalibrationProfile`은 현재 offset-removal만
  한다; 실제 provider의 성취 가능한 동적 범위를 알아야 하는 rescale은 Stage B.
- **실카메라 → 트래커 배선** — `VideoFrame`의 CPU 픽셀 접근자 부재, Windows
  카메라 백엔드 부재에 게이팅, Stage C.
- **세 번째 fanout 슬롯** — `VideoFrameFanoutSink`를 프리뷰/녹화 외 트래커용
  으로 확장하는 것, Stage C, R1의 capture 작업에 게이팅.
- **Inochi2D 렌더 (Studio 소스로서)** — Stage C.
- **Editor에서 모션 재생/재배치** — Stage C, R1의 timeline 작업에 게이팅.
