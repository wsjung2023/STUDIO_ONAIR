# CLAUDE.md — Creator Studio 개발 에이전트 규칙

## 1. 제품 미션

화면·카메라·마이크·시스템 오디오·아바타 모션을 분리 저장하고, 촬영 후 다시 배치·편집할 수 있는 데스크톱 크리에이터 스튜디오를 만든다.

## 2. 가장 중요한 규칙

1. 데모 전용 임시 구조를 만들지 않는다.
2. 원본 미디어를 직접 수정하지 않는다.
3. 모든 캡처 데이터에는 공통 프로젝트 timebase의 timestamp가 있어야 한다.
4. QML에서 FFmpeg, MLT, MediaPipe를 직접 호출하지 않는다.
5. Domain 계층은 Qt Multimedia, FFmpeg, MLT의 타입을 포함하지 않는다.
6. 캡처 소스가 DB에 직접 쓰지 않는다. Application service를 통해 기록한다.
7. GPL 의존성을 추가하지 않는다. 추가 전 `legal/OSS_BOM.csv`를 갱신한다.
8. FFmpeg 빌드에 `--enable-gpl` 또는 `--enable-nonfree`를 사용하지 않는다.
9. 녹화 실패를 숨기지 않는다. 프레임 드롭, 디스크 지연, 장치 오류를 UI와 로그에 노출한다.
10. 기능 구현은 정상 흐름과 복구 흐름을 함께 포함해야 완료다.

## 3. 아키텍처 경계

```text
qml → application → domain
application → ports
adapters → ports
platform code → capture/media adapters
```

의존 방향을 역전시키지 않는다.

## 4. 코드 규칙

- C++20
- RAII 필수
- raw owning pointer 금지
- `std::chrono` 사용, 단위 없는 정수 시간 금지
- 파일 write는 임시 파일 + flush + atomic rename 우선
- 스레드 종료는 cancellation token과 join 보장
- callback에서 예외가 외부 라이브러리 경계를 넘어가지 않게 한다.
- 모든 public class에 책임을 설명하는 주석을 둔다.
- 매직 문자열 대신 typed ID/value object 사용

## 5. 미디어 규칙

- 프리뷰와 녹화 queue를 분리한다.
- 프리뷰는 latest-frame 전략을 사용할 수 있다.
- 오디오 손실은 조용히 무시하지 않는다.
- 프레임 포맷 변환은 adapter 경계에서만 한다.
- 색공간, sample rate, channel layout을 메타데이터에 포함한다.
- 하드웨어 경로 실패 시 CPU 폴백과 이유를 기록한다.

## 6. 프로젝트 저장 규칙

- schema version 필수
- DB migration은 forward-only이며 테스트를 둔다.
- 작업 중 프로젝트는 폴더 구조다.
- cache와 proxy는 삭제 후 재생성 가능해야 한다.
- source media와 telemetry는 사용자 명령 없이 삭제하지 않는다.
- autosave는 명시적 project save와 분리한다.

## 7. Undo/Redo

모든 편집 동작은 Command로 구현한다.

```cpp
class ICommand {
public:
    virtual Result<void> execute(Project&) = 0;
    virtual Result<void> undo(Project&) = 0;
    virtual ~ICommand() = default;
};
```

UI에서 도메인 객체를 직접 변경하지 않는다.

## 8. 테스트 완료 기준

각 기능 PR에는 최소한 다음이 있어야 한다.

- unit test
- 오류 경로 test
- 로그 또는 metric 검증
- 리소스 정리 검증
- 문서 갱신

캡처 기능은 실제 장치 테스트와 mock source 테스트를 모두 제공한다.

## 9. 금지 사항

- 라이선스 확인 없이 GitHub 코드를 복사
- OBS/libobs 링크
- GPL MLT 플러그인 자동 로딩
- 임의의 FFmpeg 배포 바이너리 번들
- UI thread에서 인코딩·파일 I/O
- 무한 queue
- `sleep()` 기반 동기화
- wall clock 기반 A/V sync
- 오류를 `qDebug()`만 찍고 무시
- 원본 MP4에 destructive edit

## 10. 작업 방식

작업 시작 전:

1. 관련 문서를 읽는다.
2. 변경할 모듈과 경계를 적는다.
3. acceptance criteria를 테스트로 먼저 표현한다.
4. 작은 buildable commit 단위로 구현한다.

작업 종료 전:

1. 전체 빌드
2. 테스트
3. 포맷·정적 분석
4. 라이선스 BOM 확인
5. 문서와 실제 구현이 일치하는지 검토

## 11. 현재 최우선 작업

`prompts/01-bootstrap.md`를 실행한다. 실제 캡처를 성급하게 구현하기 전에 프로젝트 저장·시간 타입·포트 인터페이스와 빌드 기반을 먼저 고정한다.
