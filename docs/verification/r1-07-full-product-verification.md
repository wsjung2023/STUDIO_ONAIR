# R1-07 전체 제품 검증 완료

검증일: 2026-07-20
환경: Windows x64, 실제 화면·카메라·마이크·WASAPI 시스템 오디오, 감사된 FFmpeg 8.1.2 및 MLT 7.40.0 Debug 빌드.

## 최종 결과

R1-07의 실제 30분 촬영 → 일시정지/재개 → 재조립 → 편집 → 저장/재열기 → export 취소 → 재시도 → MP4 probe 경로가 통과했다.

- 실제 녹화 30분 생존, 장기 세션 3,588개와 일시정지 전 세션을 합쳐 READY 세그먼트 3,604개.
- 화면·카메라·마이크·시스템 오디오 4개 소스, 드롭 프레임 0.
- 두 녹화 세션을 8개 concat asset과 8개 녹화 트랙으로 재조립. 3,604개 세그먼트를 개별 MLT producer로 만들지 않는다.
- 4개 소스의 보정 후 세그먼트 시작 경계 최대 차이 0ms(허용 상한 40ms).
- 캡처·재조립·재열기 최대 프로세스 핸들 17,690(상한 20,000).
- export 포함 최대 프로세스 핸들 44,861(상한 60,000), 시간에 따른 핸들 누적 없음.
- export 취소 산출물 미게시, 재시도 성공, 최종 H.264/AAC MP4 probe 성공.
- 최종 MP4 56,548,748 bytes, render job `COMPLETED`, SHA-256
  `6c4a8d204bbf36bd107b1b13bc9d97bc39cfbf27979da5de1a7e87e6a4e74b5e`.
- 최종 테스트 실행 시간 2,933,149ms, GoogleTest 1/1 통과.

보존 증거:

`%TEMP%\creator-studio-r1-07-물리검증\35220\30분 강의.cstudio`

`%TEMP%\creator-studio-r1-07-물리검증\35220\최종 강의.mp4`

## 재발 방지 변경

- 실제 Windows 화면 캡처에서 관측된 +679ms gap과 -173ms overlap을 같은 연속 소스로 취급하되, ±1초를 넘는 실제 손실은 별도 concat run으로 분리한다.
- concat manifest는 짧은 sibling temporary path에 durable write 후 원자적으로 교체한다. 긴 Windows 경로 회귀 테스트가 이를 고정한다.
- FFmpeg concat 입력은 검증된 Matroska cache로 한 번만 remux하고 MLT가 개별 세그먼트 수만큼 producer와 파일 핸들을 만들지 않게 한다.
- cache key는 manifest 내용 기반이며 partial 파일명은 Windows MAX_PATH를 넘지 않는 고정 길이를 사용한다.
- 물리 테스트 자체가 보정 후 A/V 경계와 단계별 핸들 상한을 검사하므로 이전 14만~190만 핸들 회귀는 자동 실패한다.

## 자동 검증

변경된 녹화·가져오기·FFmpeg·MLT·QML 범위 90/90 테스트가 통과했다. 실제 30분 패키지 복제본의 cold re-import/replay도 8 assets, 8 tracks, 10 clips, 최대 9,566 handles로 통과했다.

## 범위 한계

이번 완료 증거는 Windows 실기 기준이다. macOS/Android 실기기 및 서명·notarization·스토어 제출은 각 플랫폼/R4 외부 승인 게이트이며 이 보고서에서 성공으로 주장하지 않는다. 30분 1080p30 소프트웨어 export 중 private memory는 최대 약 2.47GB였고 프로세스 종료로 회수됐다. 장시간 export 메모리 최적화는 상용 성능 추적 항목으로 남기되 R1의 촬영·편집·출력 정확성과 완료를 막지는 않는다.
