# Spatializer 현재 상태 정량 평가

- 실행 시작(UTC): `2026-10-28T18:27:54.3252299Z`
- 코드 기준 commit: `fe74a5bf161efbaa5f7df0698ad4a44842a46819`
- 시험 DLL SHA-256: `7EC0F2011F6D4FC90BE94C5B98FDE399FCCC3EA990725F88738A2A59FF72B5CF`
- 검사: **134 PASS / 0 FAIL**
- 대상: 현재 Assets에 배포된 자체 Spatializer DLL. Unity 엔진과 오디오 장치를 실행하지 않은 합성 호스트 시험이다.
- 소스와 DLL의 일치는 해시만으로 단정하지 않는다. parameters.csv에서 ABI 계약을 검사했고 실제 시험 대상은 위 DLL 해시로 식별한다.
- 아래 수치는 이번 실행에서 수집했다. 비교 제품의 결과나 청취 품질 평가는 포함하지 않는다.

## 구현 검증

| 항목 | 측정값 |
|---|---:|
| 좌표 최대 오차 (Unity 단위) | 4.76837158e-07 |
| Panning 계수 최대 오차 | 1.87742394e-07 |
| 보간 중 Equal-Power 최대 오차 | 3.04508826e-07 |
| 버퍼 분할 최대 출력 차이 | 0 |
| 4밴드 중립 합산 최대 진폭 오차 (dB) | 5.06417534e-07 |
| 거리 감쇠 콜백 최대 오차 | 9.49949026e-11 |

검사 한계값은 validations.csv에 기록한 공학적 회귀 검사 허용치이며 청취상 허용 오차나 제품 우위 기준이 아니다.

## 48kHz 좌→우 step

| 보간(ms) | 최대 계수 변화량 | 추종 RMSE | 목표 도달(samples) |
|---:|---:|---:|---:|
| 0 | 1 | 4.32978028e-17 | 1 |
| 5 | 0.00654564798 | 0.134405986 | 240 |
| 10 | 0.00327363214 | 0.190352146 | 480 |
| 20 | 0.00163649989 | 0.269390742 | 960 |

step은 좌측에서 시작해 우측으로 변경한다. 목표를 처음 받은 뒤 100ms의 동일 구간에서 두 채널 계수 오차를 합산해 RMSE = sqrt(sum(eL²+eR²)/(2N))로 계산한다.
시간은 변경 후 첫 렌더링 sample을 1로 센다. 0ms는 첫 sample에 즉시 적용되며 장치 지연을 의미하지 않는다.

## 48kHz 원형 이동

| 보간(ms) | 최대 계수 변화량 | 추종 RMSE |
|---:|---:|---:|
| 0 | 0.0402950346 | 7.51646524e-08 |
| 5 | 0.000170648098 | 0.0100395309 |
| 10 | 8.85128975e-05 | 0.0142201013 |
| 20 | 8.48770142e-05 | 0.0375032285 |

원형 이동은 1회전/초로 2초간 실행하며 제어 목표는 10ms마다 갱신한다. RMSE의 기준은 각 갱신 시점에서 전달한 목표를 유지한 계수이며 연속 시간의 이상적인 위치가 아니다.
출력 추적 파일은 48kHz에서 step은 매 sample, 원형은 48 samples마다 저장한다. 집계 지표는 저장 간격과 무관하게 모든 sample로 계산한다.

## 오프라인 callback 실행 시간

아래는 48kHz / 1024-frame / 32 sources의 대표 조건이다. 모든 조건은 benchmark_summary.csv와 benchmark_blocks.csv에 보관한다.

| 4밴드 | 이동 | 평균 group(ms) | p95 group(ms) | ns/source-frame |
|---|---|---:|---:|---:|
| OFF | 정지 | 0.519901 | 0.7465 | 15.8661 |
| OFF | 이동 | 0.570602 | 0.7904 | 17.4134 |
| ON | 정지 | 3.73664 | 4.5434 | 114.033 |
| ON | 이동 | 4.05071 | 5.2815 | 123.618 |

- group은 N개 source를 한 thread에서 순차 처리한 callback과 최소한의 합성 host tick bookkeeping이다. scene 이동 계산과 파일 저장은 계측 구간 밖에 있다.
- p95는 측정한 group wall time의 nearest-rank 95백분위이다. Unity 프레임 시간이나 오디오 장치 deadline 측정이 아니다.
- serial_wall_time_to_audio_duration_ratio는 순차 처리 wall time/생성 오디오 길이이다. DSP CPU 사용률 또는 실시간 재생 보장을 뜻하지 않는다.
- 각 조건 32 blocks 준비 후 반복 측정했다. CPU clock·다른 프로세스·온도 영향을 받으므로 다른 PC 또는 다른 실행의 값과 절대 우위를 단정하지 않는다.

## 재현 조건 및 범위

- 버퍼 분할 검사는 seed 0x12345678의 고정 입력과 partition_events.csv의 동일 sample 시점 이벤트를 사용한다. 이벤트 경계에서 block을 나누며 1-frame 기준과 37/256/512/1024-frame 처리를 비교한다.
- 4밴드 응답은 0.125 진폭 sine, 0.5초 준비 후 0.5초 RMS를 계산한다. 중립 합산의 진폭을 평가하며 원본과의 sample 단위 위상 일치를 요구하지 않는다.
- 감쇠 검사는 기존 콜백의 Unity 입력값 통과 및 수동 활성화한 inverse 항만 검사한다. 실제 Unity 출력의 거리 감쇠나 자체 Linear 모델의 완료를 의미하지 않는다.
- Unity Native SDK 원본·Steam Audio·Unity DSP CPU·장치 지연·실제 underrun은 이번 측정에 포함되지 않는다.
- 현재 수치는 즉시 적용과 현재 Pan 보간의 비교이다. 좌우 Gain 보간 대안은 아직 측정하지 않았다.

## 원자료

metadata.json, parameters.csv, validations.csv, geometry.csv, panning.csv, motion_summary.csv, motion_trace.csv, partition.csv, partition_events.csv, attenuation_callback.csv, frequency_response.csv, benchmark.csv, benchmark_blocks.csv, benchmark_summary.csv, build-output.txt, execution.txt
