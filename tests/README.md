# Tests

```powershell
ctest --preset windows-debug
ctest --preset windows-release
```

Windows에서는 실행 전에 `C:\Qt\6.8.3\msvc2022_64\bin`이 `PATH`에 있어야 한다. 2026-07-16 이 머신에서 Debug와 Release 각각 202개 테스트가 발견되어 201개가 통과했고, symlink 생성 권한이 필요한 1개가 조건부 skip됐다. 외부 DB alias 거부는 별도의 hard-link 테스트가 실행되어 통과했다. macOS는 아직 실제 머신에서 검증하지 않았다.

## 실행 파일

- `cs_tests` — Qt를 링크하지 않는 core/domain/capture/recorder/project-store 테스트. SQLite 연결·마이그레이션·세션/세그먼트 영속화·복구·패키지 경계를 포함한다.
- `cs_app_tests` — `QCoreApplication` 기반의 `StudioController`와 `ProjectController` 테스트. 비동기 작업 스레드 왕복과 녹화 저장 순서를 검증한다.
- `cs_qml_tests` — `QGuiApplication`을 offscreen으로 실행해 Recovery QML이 C++ 컨트롤러 계약으로 생성되는지 검증한다.
- `cs_crash_recovery_fixture` — 테스트 전용 보조 프로세스. DB와 미디어 fixture를 만든 뒤 `std::_Exit(73)`로 소멸자를 건너뛰어 실제 프로세스 사망 상태를 만든다.

## R0-02 주요 범위

- JSON Schema 경계값, 손상·누락·미래 버전 거부
- SQLite PRAGMA와 오류 변환, 전진 전용 migration 및 checksum
- 프로젝트/녹화 세션/세그먼트 트랜잭션
- READY/WRITING 세그먼트 복구와 미디어 파일 불변성
- Unicode 프로젝트 이름·경로와 내구성 있는 패키지 생성
- 최근 프로젝트 레지스트리 손상 백업
- UI 스레드를 막지 않는 프로젝트 작업과 완료 콜백 1회 보장
- DB begin 전 캡처 금지, DB complete 전 `Stopped` 표시 금지
- 앱 재시작 뒤 같은 프로젝트의 두 번째 녹화와 UUID 세션 ID
- 누락 DB 비변경, 외부 hard-link DB 거부, symlink/reparse 방어
- 완료 시 남은 WRITING 행의 transaction rollback
- 컨트롤러 종료 시 pending persistence callback 정확히 1회 완료
- RecoveryPage 계약과 시작 시 복구 경합을 다루는 QML smoke

## 규칙

`CLAUDE.md` §8에 따라 정상 경로뿐 아니라 오류 경로를 함께 검증한다. fake는 스레드를 만들거나 시계를 읽지 않는다. 시간 기반 `sleep` 동기화 대신 Qt signal, callback, promise 또는 자식 프로세스 종료를 관찰한다.

실제 장치 캡처 테스트는 아직 없다. R0-03부터 네이티브 캡처 구현과 함께 mock·실장치 양쪽 검증이 필요하다.
