# Tests

```
ctest --preset windows-debug     # 또는 macos-debug
```

## 구성

테스트 실행 파일은 두 개다.

- `cs_tests` — Qt를 링크하지 않는다. 이것이 그 자체로 검사다: application 계층 아래가 정말 Qt 없이 빌드되는지 매 실행마다 확인된다.
- `cs_app_tests` — `StudioController`용. `QCoreApplication`이 필요해 `gtest_main`을 쓸 수 없고 `main()`을 직접 둔다.

## 현재 범위 (R0-01)

- Result/AppError — 값 전달, 오류 전파
- Timebase — 단위 변환, 프레임↔시간 왕복(432000 프레임까지), 59.94fps 정확도
- UuidV4 / Utc — 생성·검증·RFC 3339 왕복
- Identifier — 직렬화, 동등성, 타입 안전성(`static_assert`)
- ProjectManifest — 스키마 경계값 검증
- RecordingSession — 상태 전이와 오류 경로
- JsonProjectStore — manifest 왕복, 손상·절단·필드 누락·미래 버전
- FakeCaptureSource / FakeRecorder — 60fps 타임스탬프, 세그먼트 분할
- StudioController — Record/Stop, 세그먼트·duration 보고

## 규칙

`CLAUDE.md` §8에 따라 기능 PR에는 정상 경로만이 아니라 **오류 경로 테스트**가 함께 있어야 한다.

fake는 스레드를 만들지 않고 시계를 읽지 않는다. 프레임은 `tick()`에서만 나오고 타임스탬프는 프레임 번호에서 계산된다. `sleep` 기반 테스트는 느리고 간헐 실패하며 `CLAUDE.md` §9가 금지한다.

## 아직 없는 것

- 복구 스캐너 픽스처 (R0-02)
- 실제 장치 테스트 — 캡처 기능은 실장치와 mock 양쪽 테스트를 모두 제공해야 한다 (`CLAUDE.md` §8). 현재는 캡처 구현 자체가 없다.
