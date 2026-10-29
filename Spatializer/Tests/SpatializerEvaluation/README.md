# Spatializer 정량 평가

현재 배포된 `Assets/Plugins/x86_64/AudioPluginDemo.dll`을 합성 host에서 로드해 실제 callback을 호출한다. DSP 식을 테스트 코드에 복사해 측정값으로 사용하는 방식이 아니다. 생산 코드, 씬, DLL은 변경하지 않는다. 별도 테스트용 오디오만 사용한다.

## 실행

필수 환경: Windows x64, Visual Studio C++ x64 build tools, Python 3. Python 분석은 표준 라이브러리만 사용한다. 이 PC에서는 번들 Python을 자동으로 찾는다.

프로젝트 폴더의 PowerShell에서:

```powershell
& ./Tests/SpatializerEvaluation/Run-Evaluation.ps1
```

선택 인자:

```powershell
& ./Tests/SpatializerEvaluation/Run-Evaluation.ps1 -BenchmarkRepeats 5 -BenchmarkBlocks 96
```

`-DllPath`는 동일한 자체 ABI를 가진 새 빌드의 검사에 사용한다. Unity SDK 원본이나 Steam Audio에 이 runner를 그대로 적용할 수는 없다. 대상 DLL에 `Demo Spatializer`, 21개 자체 파라미터 및 snapshot export가 있어야 한다.

결과는 `Results/<UTC 실행 시각>/`에 새로 생성하며 이전 실행을 덮어쓰지 않는다. 실행 코드·source·DLL의 SHA-256, commit 및 작업 상태를 `metadata.json`에 보관한다. 실제 DLL이 source와 동일한 빌드인지 해시만으로 증명할 수는 없으므로 해당 제한도 기록한다. 빌드용 exe/obj는 `.build/`에 두고 Git에서 제외한다. CSV·검사 코드·실행 기록은 보존한다.

저장된 결과를 다시 분석하려면:

```powershell
python ./Tests/SpatializerEvaluation/analyze_results.py ./Tests/SpatializerEvaluation/Results/<run-id>
```

성공 시 native 검사가 0을 반환하고, Python이 scenario 개수·유한성·원자료와 집계의 일치를 추가 확인한다. 실패 시 0이 아닌 종료 상태와 CSV/실행 기록을 보존한다. 분석은 raw CSV를 변경하지 않고 파생 summary/report만 다시 만든다.

## 현재 확보하는 지표

| 파일 | 내용 | 범위 |
|---|---|---|
| `validations.csv` | 검사별 PASS/FAIL, 실제 오차, 허용치 | 공학적 회귀 검사 |
| `data_checks.csv` | Python의 독립 재계산 결과 | raw trace/benchmark와 native 집계의 일치 |
| `geometry.csv` | 위치·방위각·고도각 오차 | 70개 합성 listener/source 조건 |
| `panning.csv` | 좌우 계수·이론값·오차 | 각도 73개 × 고도 3개 × blend 3개 |
| `motion_summary.csv` | 최대 계수 변화량·추종 RMSE·전력 오차·도달 sample | 44.1/48/96kHz × 0/5/10/20ms × step/원형 |
| `motion_trace.csv` | 목표/실제 좌우 계수의 시간 이력 | 48kHz step 전체 sample, 원형 1ms 간격 |
| `partition.csv` | block 분할별 최대 출력 오차·RMSE | 1-frame 기준, 37/256/512/1024-frame 비교 |
| `partition_events.csv` | sample 시점별 Pan·Gain 변경 | 고정 이벤트 입력 원본 |
| `attenuation_callback.csv` | 기존 감쇠 callback 반환값과 오차 | 기본 입력 통과 / 수동 inverse 활성화 |
| `frequency_response.csv` | 4밴드 응답·중립 합산 진폭 | 여러 sample rate의 중립 및 48kHz 대역별 출력 |
| `benchmark_blocks.csv` | N개 source 처리 group의 개별 wall time | 원자료 |
| `benchmark.csv` | 각 반복의 평균·p95·최대·ns/source-frame | 32조건 × 기본 5회 |
| `benchmark_summary.csv` | 반복 통합 집계와 반복 평균의 편차 | 비교 그래프용 파생 데이터 |
| `metadata.json`, `parameters.csv` | 환경·해시·설정·ABI | 재현 조건 |
| `summary.json`, `report.md` | 기계 처리용 요약 및 한국어 보고서 | 발표 자료 작성용 |
| `build-output.txt`, `execution.txt` | 빌드·실행 기록 | 오류 조사 |

## 시험 입력과 지표 정의

- **계수 probe:** 4밴드 OFF, blend=1, output Gain=1, stereo 입력 `(1,1)`. 출력이 좌우 Pan 계수이므로 계수 변화를 직접 관측한다. 청취용 음원·청취 품질 측정이 아니다.
- **정적 Panning oracle:** 저장된 float 위치에서 double 정밀도로 analytic Equal-Power 식을 계산한다. blend=0/0.5/1의 출력도 검사한다. 전력 1 조건은 blend=1에서만 판정한다.
- **Step:** 좌측에서 한 sample을 처리한 후 우측으로 목표를 변경하고 100ms 측정한다. 시작 계수 `(1,0)`에서 첫 출력까지의 차이도 최대 변화량에 포함한다. 도달 시간은 변경 후 첫 출력 sample을 1로 센다.
- **원형:** 좌측 시작, 1회전/초, 2초 길이, 10ms 간격으로 목표 갱신. 추종 기준은 마지막으로 전달한 목표를 유지한 좌우 계수이며 연속적인 이상 위치가 아니다. 보간 20ms는 다음 목표 도착 시 아직 진행 중일 수 있다.
- **RMSE:** `sqrt(sum((gL-targetL)^2 + (gR-targetR)^2)/(2*N))`. 동일 경로·동일 구간에서 비교한다. 각도 오차나 인지 성능 지표가 아니다.
- **분할 재현성:** 48kHz, 12000 frames, xorshift32 seed `0x12345678`의 고정 입력. 이벤트 sample 0/311/919/1801/2713/5003/7901에서 목표를 바꾸고 그 경계에서 callback을 분할한다. 동일 제어 입력을 주었을 때 block 분할만 바꾼 검사다.
- **4밴드:** 0.125 진폭 sine, 준비 0.5초, 측정 0.5초. 8/16/44.1/48/96/192kHz에서 Nyquist에 근접하지 않는 40~16000Hz의 고정 주파수를 사용한다. 중립은 진폭 평탄성을 검사하며 위상·원본 파형 일치를 주장하지 않는다.
- **허용치:** 위치 `1e-5`, 각도 `1e-3°`, 계수/단위전력/분할 차이 `2e-6`, 중립 합산 `0.005dB` 등. 수치 회귀 검사용으로 정한 값이며 청취상 허용 오차나 제품 비교 우위 기준이 아니다. 세부 값은 `validations.csv`에 있다.

## 실행 시간 측정

48kHz, 256/1024 frames, 1/8/16/32 sources, 4밴드 OFF/ON, 정지/이동의 32조건이다. 보간 시간은 5ms다. 기본 32개 block의 준비 처리 후 조건마다 96개 group을 5회 측정한다. 새 instance 생성과 입력 생성은 계측 구간 밖이다. 같은 조건 내 반복은 상태를 이어 사용하며 첫 32개 block은 조건 시작에만 준비한다.

한 group은 N개 source의 실제 callback과 최소한의 합성 host tick 갱신을 한 thread에서 순차 실행한 구간이다. 시계는 `steady_clock`을 사용한다. 위치 갱신·CSV 기록·정렬·메모리 할당은 계측 구간에서 제외한다. p95는 개별 group 시간의 nearest-rank 방식이다. `ns_per_source_frame`은 group 시간 / (source 수 × frame 수)이다.

이 값은 **Unity의 DSP CPU, OS의 CPU 사용률, 장치 전체 지연, 실시간 재생 성공률이 아니다.** `serial_wall_time_to_audio_duration_ratio` 역시 처리 시간/오디오 길이의 참고 비율일 뿐 CPU %로 표기하지 않는다. CPU 온도·clock·백그라운드 작업 영향을 받으며 조건 순서도 고정되어 있어, 미세한 성능 차이는 별도 반복·순서 교차 실험으로 확인해야 한다. 동일 host overhead가 포함되므로 매우 작은 처리 시간의 해석에 주의한다.

## 아직 측정하지 않는 항목

- Unity Native SDK 원본 및 Steam Audio와의 비교
- Unity Audio Profiler의 DSP CPU와 실제 처리 voice 수
- 실제 scene 입력부터 출력까지의 반응 시간, 장치 latency, underrun
- 자체 Linear 감쇠 모델 및 엔진이 적용한 감쇠의 최종 출력
- Gain 자체를 보간하는 대안과의 비교
- 청취/방향 인지 성능

감쇠 callback은 기본값에서 host의 `attenuationIn`을 통과시킨다. 기존 inverse 항을 켜서 식을 검증하지만 실제 Unity 출력까지 해당 값이 적용됐다는 검사는 아니다. 이후 Unity 측정 코드는 공간화 이후의 최종 출력 수집 위치를 먼저 검증하고, 처리 부하 측정은 파일 저장·상세 로깅과 분리해야 한다.
