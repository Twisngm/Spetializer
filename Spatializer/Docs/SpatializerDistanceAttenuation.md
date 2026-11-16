# 자유음장 거리 감쇠

자유음장에서 점음원의 직접음은 구면으로 퍼진다. 음향 세기 `I`는 거리 `r`의 제곱에 반비례하고, 음압 진폭 `p`는 세기의 제곱근에 비례하므로 `1/r`로 줄어든다. 오디오 진폭에 곱할 Gain은 음압 비율이며, 거리 두 배마다 음압 레벨 차이는 `20 log10(1/2) ≈ −6.02 dB`다. [OSHA Technical Manual, Section III, Chapter 5, Appendix B.6](https://www.osha.gov/otm/section-3-health-hazards/chapter-5)

ISO 9613-2는 옥외 전파를 예측할 때 기하학적 확산, 대기 흡수, 지면 효과, 표면 반사, 차폐를 각각 다룬다. 이 모드는 직접음의 기하학적 확산을 모델링한다. 아래 dB 값의 기준은 1m에서의 소스 레벨이다. [ISO 9613-2:2024](https://www.iso.org/standard/74047.html)

## 적용 곡선

Unity 1유닛을 1m로 해석한다. 유효한 3차원 거리 `d`에 대해 거리 Gain은 `1m / max(1m, d)`다. 1m 이내에서 Gain을 1로 제한해 소스와 리스너가 겹쳐도 진폭이 증폭되지 않게 한다. 이 제한은 1m에서 레벨을 정규화하기 위한 구현 선택이다. 거리가 1m를 넘으면 역거리에 따라 연속적으로 줄어든다.

| 거리 | 진폭 Gain | 1m 대비 |
| ---: | ---: | ---: |
| 0–1m | 1 | 0 dB |
| 2m | 0.5 | −6.02 dB |
| 4m | 0.25 | −12.04 dB |
| 8m | 0.125 | −18.06 dB |
| 16m | 0.0625 | −24.08 dB |

## 신호 처리 순서

Unity 2017.1 이후의 오디오 플러그인 API에서는 4밴드 처리, Panning, 2D/3D 혼합을 마친 좌우 출력에 거리 Gain을 적용한다. 새추레이션은 거리 감쇠 이전의 신호를 받아 거리별 진폭 비율을 유지한다. 거리 Gain은 기존 `Motion Smoothing Ms` 설정에 따라 보간한다.

Spatial Blend가 1이면 거리 Gain을 그대로 적용한다. 중간 Blend에서는 `(1 − blend) + blend × 거리 Gain`을 사용하고, Blend가 0이면 거리 Gain은 1이다. Unity의 거리 감쇠 콜백은 음원 가청도와 voice 우선순위 계산에 필요한 값을 전달한다. [Unity Audio Spatializer SDK](https://docs.unity3d.com/6000.0/Documentation/Manual/AudioSpatializerSDK.html)

## Unity에서 사용

1. Audio Project Settings에서 Spatializer Plugin을 `Demo Spatializer`로 설정한다.
2. AudioSource에서 **Spatialize**를 켜고 **Spatializer Distance Attenuation** 컴포넌트를 추가한다. `SampleScene`의 `TestSource`에는 설정이 적용되어 있다.
3. 같은 방향에서 리스너와 소스 사이를 1m, 2m, 4m로 바꿔 비교한다. 완전한 3D 음원의 감쇠를 비교하려면 Spatial Blend를 1로 둔다.

컴포넌트를 켜면 네이티브 파라미터 21이 활성화된다. 플러그인은 소스와 리스너 사이의 3차원 거리로 Gain을 계산한다. 기준 거리는 1m로 고정되며 AudioSource의 Max Distance를 지나도 `1/r`로 계속 줄어든다. 컴포넌트를 끄면 Unity가 전달한 AudioSource Rolloff와 기존 감쇠 파라미터로 계산한 Gain을 사용한다. 플러그인의 출력 Gain은 소스 레벨을 별도로 조정한다.
