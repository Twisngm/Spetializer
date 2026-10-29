"""Analyze saved measurements using only Python's standard library.

Usage: python analyze_results.py Results/<run-id>
Never runs the subject or replaces raw CSV files.
"""
import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path


def rows(root, filename):
    with (root / filename).open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def maximum(data, column):
    return max(float(row[column]) for row in data)


def percentile95(values):
    values = sorted(values)
    return values[math.ceil(0.95 * len(values)) - 1]


def main(root):
    metadata = json.loads((root / "metadata.json").read_text(encoding="utf-8-sig"))
    checks = rows(root, "validations.csv")
    failed = [row for row in checks if row["status"] != "PASS"]
    if any(row["test"] == "fatal" for row in checks):
        raise RuntimeError("Native evaluation aborted; retain execution.txt and fix the fatal error before analysis.")
    geometry = rows(root, "geometry.csv")
    panning = rows(root, "panning.csv")
    motion = rows(root, "motion_summary.csv")
    partition = rows(root, "partition.csv")
    frequency = rows(root, "frequency_response.csv")
    attenuation = rows(root, "attenuation_callback.csv")
    benchmark = rows(root, "benchmark.csv")
    benchmark_raw = rows(root, "benchmark_blocks.csv")
    trace = rows(root, "motion_trace.csv")
    # A missing scenario must not silently produce a successful report.
    expected_counts = {"geometry": 70, "panning": 657, "motion": 24,
                       "partition": 24, "attenuation": 18,
                       "benchmark": 32 * metadata["benchmark_repeats"],
                       "benchmark_raw": 32 * metadata["benchmark_repeats"] * metadata["benchmark_blocks"]}
    actual_counts = {"geometry": len(geometry), "panning": len(panning), "motion": len(motion),
                     "partition": len(partition), "attenuation": len(attenuation),
                     "benchmark": len(benchmark), "benchmark_raw": len(benchmark_raw)}
    if actual_counts != expected_counts:
        raise RuntimeError(f"Incomplete measurements: {actual_counts!r}, expected {expected_counts!r}")
    for table, columns in [(geometry, ["position_max_error", "azimuth_error_deg", "elevation_error_deg"]),
                           (panning, ["actual_left", "actual_right", "max_coefficient_error"]),
                           (motion, ["max_coefficient_delta", "tracking_rmse", "max_power_error"]),
                           (partition, ["max_abs_output_error", "output_rmse"]),
                           (frequency, ["gain_db", "output_left_rms"]),
                           (benchmark_raw, ["group_wall_time_ms"])]:
        if any(not math.isfinite(float(row[column])) for row in table for column in columns):
            raise RuntimeError("Nonfinite numeric measurement; do not publish this run as successful.")

    # Independently recalculate the complete step traces rather than trusting C++ summaries.
    audits = []

    def audit(name, actual, expected, tolerance=1e-9):
        error = abs(actual - expected)
        passed = math.isfinite(error) and error <= tolerance
        audits.append(dict(test=name, status="PASS" if passed else "FAIL",
                           absolute_error=error, tolerance=tolerance))
        if not passed:
            raise RuntimeError(f"Data consistency failure: {name}: {actual} != {expected}")

    for motion_row in motion:
        if motion_row["sample_rate"] != "48000" or motion_row["trajectory"] != "step_left_to_right":
            continue
        step_trace = [r for r in trace if r["trajectory"] == "step_left_to_right"
                      and r["smoothing_ms"] == motion_row["smoothing_ms"]]
        audit("step_trace_count_" + motion_row["smoothing_ms"], len(step_trace), int(motion_row["frames"]), 0)
        previous = (1.0, 0.0)
        squared, largest_delta, power_error, reached = 0.0, 0.0, 0.0, -1
        for index, row in enumerate(step_trace):
            if int(row["sample_index"]) != index:
                raise RuntimeError("Step trace is not sample-contiguous.")
            current = (float(row["actual_left"]), float(row["actual_right"]))
            target = (float(row["target_left"]), float(row["target_right"]))
            largest_delta = max(largest_delta, *(abs(current[c] - previous[c]) for c in (0, 1)))
            squared += sum((current[c] - target[c]) ** 2 for c in (0, 1))
            power_error = max(power_error, abs(sum(v*v for v in current) - 1))
            if reached < 0 and abs(current[0]) < 1e-7 and abs(current[1] - 1) < 1e-7:
                reached = index + 1
            previous = current
        suffix = motion_row["smoothing_ms"]
        audit("step_max_delta_" + suffix, largest_delta, float(motion_row["max_coefficient_delta"]))
        audit("step_rmse_" + suffix, math.sqrt(squared/(2*len(step_trace))), float(motion_row["tracking_rmse"]))
        audit("step_power_" + suffix, power_error, float(motion_row["max_power_error"]))
        audit("step_reached_" + suffix, reached, int(motion_row["step_reached_after_samples"]), 0)

    groups = defaultdict(list)
    repeat_means = defaultdict(list)
    for row in benchmark_raw:
        key = tuple(int(row[k]) for k in ("multiband", "moving", "sources", "block_frames"))
        groups[key].append(float(row["group_wall_time_ms"]))
    for row in benchmark:
        key = tuple(int(row[k]) for k in ("multiband", "moving", "sources", "block_frames"))
        repeat_means[key].append(float(row["mean_group_ms"]))
        block_values = [float(r["group_wall_time_ms"]) for r in benchmark_raw
                        if all(r[k] == row[k] for k in ("multiband", "moving", "sources", "block_frames", "repeat"))]
        suffix = "_".join(row[k] for k in ("multiband", "moving", "sources", "block_frames", "repeat"))
        audit("benchmark_count_" + suffix, len(block_values), metadata["benchmark_blocks"], 0)
        audit("benchmark_mean_" + suffix, statistics.mean(block_values), float(row["mean_group_ms"]))
        audit("benchmark_p95_" + suffix, percentile95(block_values), float(row["p95_group_ms"]))
    with (root / "data_checks.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(audits[0]))
        writer.writeheader()
        writer.writerows(audits)
    benchmark_summary = []
    for key, samples in sorted(groups.items()):
        bands, moving, sources, frames = key
        mean = statistics.mean(samples)
        means = repeat_means[key]
        benchmark_summary.append(dict(multiband=bands, moving=moving, sources=sources,
            block_frames=frames, sample_rate=48000, repeats=len(means), measured_groups=len(samples),
            mean_group_ms=mean, p95_group_ms=percentile95(samples), max_group_ms=max(samples),
            repeat_mean_stdev_ms=statistics.stdev(means), repeat_mean_min_ms=min(means),
            repeat_mean_max_ms=max(means), ns_per_source_frame=mean * 1e6 / (sources * frames),
            serial_wall_time_to_audio_duration_ratio=mean / (1000 * frames / 48000)))
    with (root / "benchmark_summary.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(benchmark_summary[0]))
        writer.writeheader()
        writer.writerows(benchmark_summary)

    measured = {
        "geometry_position_max_error_unity_units": maximum(geometry, "position_max_error"),
        "panning_max_coefficient_error": maximum(panning, "max_coefficient_error"),
        "motion_max_equal_power_error": maximum(motion, "max_power_error"),
        "partition_max_output_error": maximum(partition, "max_abs_output_error"),
        "neutral_four_band_max_abs_gain_db": max(abs(float(r["gain_db"])) for r in frequency if r["mode"] == "neutral"),
        "attenuation_callback_max_error": maximum(attenuation, "absolute_error"),
    }
    summary = dict(schema_version=1, validation_passed=not failed, data_consistency_checks=len(audits),
        checks=len(checks), failures=failed, measured=measured, scenario_counts=actual_counts,
        subject=metadata["subject"], dll_sha256=metadata["dll_sha256"],
        scope=metadata["benchmark_scope"], unavailable=metadata["unavailable"])
    (root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    step = sorted([r for r in motion if r["sample_rate"] == "48000" and r["trajectory"] == "step_left_to_right"], key=lambda r: float(r["smoothing_ms"]))
    circle = sorted([r for r in motion if r["sample_rate"] == "48000" and r["trajectory"] == "circle_1rev_per_sec"], key=lambda r: float(r["smoothing_ms"]))
    korea_started = datetime.fromisoformat(metadata['started_utc'].replace('Z', '+00:00')).astimezone(timezone(timedelta(hours=9)))
    lines = ["# Spatializer 현재 상태 정량 평가", "",
        f"- 실행 시작(한국 시간): `{korea_started.isoformat()}`",
        f"- 실행 시작(UTC): `{metadata['started_utc']}`",
        f"- 코드 기준 commit: `{metadata['git_commit']}`",
        f"- 시험 DLL SHA-256: `{metadata['dll_sha256']}`",
        f"- 검사: **{len(checks) - len(failed)} PASS / {len(failed)} FAIL**",
        f"- 원자료 독립 재계산: **{len(audits)} PASS** (48kHz step 전체 trace 및 benchmark 반복별 집계)",
        "- 대상: 현재 Assets에 배포된 자체 Spatializer DLL. Unity 엔진과 오디오 장치를 실행하지 않은 합성 호스트 시험이다.",
        "- 소스와 DLL의 일치는 해시만으로 단정하지 않는다. parameters.csv에서 ABI 계약을 검사했고 실제 시험 대상은 위 DLL 해시로 식별한다.",
        "- 아래 수치는 이번 실행에서 수집했다. 비교 제품의 결과나 청취 품질 평가는 포함하지 않는다.", "",
        "## 구현 검증", "", "| 항목 | 측정값 |", "|---|---:|"]
    for label, key in [("좌표 최대 오차 (Unity 단위)", "geometry_position_max_error_unity_units"),
                       ("Panning 계수 최대 오차", "panning_max_coefficient_error"),
                       ("보간 중 Equal-Power 최대 오차", "motion_max_equal_power_error"),
                       ("버퍼 분할 최대 출력 차이", "partition_max_output_error"),
                       ("4밴드 중립 합산 최대 진폭 오차 (dB)", "neutral_four_band_max_abs_gain_db"),
                       ("거리 감쇠 콜백 최대 오차", "attenuation_callback_max_error")]:
        lines.append(f"| {label} | {measured[key]:.9g} |")
    lines += ["", "검사 한계값은 validations.csv에 기록한 공학적 회귀 검사 허용치이며 청취상 허용 오차나 제품 우위 기준이 아니다.", "",
        "## 48kHz 좌→우 step", "", "| 보간(ms) | 최대 계수 변화량 | 추종 RMSE | 목표 도달(samples) |", "|---:|---:|---:|---:|"]
    for r in step:
        lines.append(f"| {r['smoothing_ms']} | {float(r['max_coefficient_delta']):.9g} | {float(r['tracking_rmse']):.9g} | {r['step_reached_after_samples']} |")
    lines += ["", "step은 좌측에서 시작해 우측으로 변경한다. 목표를 처음 받은 뒤 100ms의 동일 구간에서 두 채널 계수 오차를 합산해 RMSE = sqrt(sum(eL²+eR²)/(2N))로 계산한다.",
        "시간은 변경 후 첫 렌더링 sample을 1로 센다. 0ms는 첫 sample에 즉시 적용되며 장치 지연을 의미하지 않는다.", "",
        "## 48kHz 원형 이동", "", "| 보간(ms) | 최대 계수 변화량 | 추종 RMSE |", "|---:|---:|---:|"]
    for r in circle:
        lines.append(f"| {r['smoothing_ms']} | {float(r['max_coefficient_delta']):.9g} | {float(r['tracking_rmse']):.9g} |")
    lines += ["", "원형 이동은 1회전/초로 2초간 실행하며 제어 목표는 10ms마다 갱신한다. RMSE의 기준은 각 갱신 시점에서 전달한 목표를 유지한 계수이며 연속 시간의 이상적인 위치가 아니다.",
        "출력 추적 파일은 48kHz에서 step은 매 sample, 원형은 48 samples마다 저장한다. 집계 지표는 저장 간격과 무관하게 모든 sample로 계산한다.", "",
        "## 오프라인 callback 실행 시간", "",
        "아래는 48kHz / 1024-frame / 32 sources의 대표 조건이다. 모든 조건은 benchmark_summary.csv와 benchmark_blocks.csv에 보관한다.", "",
        "| 4밴드 | 이동 | 평균 group(ms) | p95 group(ms) | ns/source-frame |", "|---|---|---:|---:|---:|"]
    for r in benchmark_summary:
        if r["sources"] == 32 and r["block_frames"] == 1024:
            lines.append(f"| {'ON' if r['multiband'] else 'OFF'} | {'이동' if r['moving'] else '정지'} | {r['mean_group_ms']:.6g} | {r['p95_group_ms']:.6g} | {r['ns_per_source_frame']:.6g} |")
    lines += ["", "- group은 N개 source를 한 thread에서 순차 처리한 callback과 최소한의 합성 host tick bookkeeping이다. scene 이동 계산과 파일 저장은 계측 구간 밖에 있다.",
        "- p95는 측정한 group wall time의 nearest-rank 95백분위이다. Unity 프레임 시간이나 오디오 장치 deadline 측정이 아니다.",
        "- serial_wall_time_to_audio_duration_ratio는 순차 처리 wall time/생성 오디오 길이이다. DSP CPU 사용률 또는 실시간 재생 보장을 뜻하지 않는다.",
        "- 각 조건 32 blocks 준비 후 반복 측정했다. CPU clock·다른 프로세스·온도 영향을 받으므로 다른 PC 또는 다른 실행의 값과 절대 우위를 단정하지 않는다.", "",
        "## 재현 조건 및 범위", "",
        "- 버퍼 분할 검사는 seed 0x12345678의 고정 입력과 partition_events.csv의 동일 sample 시점 이벤트를 사용한다. 이벤트 경계에서 block을 나누며 1-frame 기준과 37/256/512/1024-frame 처리를 비교한다.",
        "- 4밴드 응답은 0.125 진폭 sine, 0.5초 준비 후 0.5초 RMS를 계산한다. 중립 합산의 진폭을 평가하며 원본과의 sample 단위 위상 일치를 요구하지 않는다.",
        "- 감쇠 검사는 기존 콜백의 Unity 입력값 통과 및 수동 활성화한 inverse 항만 검사한다. 실제 Unity 출력의 거리 감쇠나 자체 Linear 모델의 완료를 의미하지 않는다.",
        "- Unity Native SDK 원본·Steam Audio·Unity DSP CPU·장치 지연·실제 underrun은 이번 측정에 포함되지 않는다.",
        "- 현재 수치는 즉시 적용과 현재 Pan 보간의 비교이다. 좌우 Gain 보간 대안은 아직 측정하지 않았다.", "",
        "## 원자료", "",
        "metadata.json, parameters.csv, validations.csv, data_checks.csv, geometry.csv, panning.csv, motion_summary.csv, motion_trace.csv, partition.csv, partition_events.csv, attenuation_callback.csv, frequency_response.csv, benchmark.csv, benchmark_blocks.csv, benchmark_summary.csv, build-output.txt, execution.txt", ""]
    if failed:
        lines += ["## 실패", ""] + [f"- {r['test']}: {r['actual_error_or_failure_count']} > {r['limit']}" for r in failed]
    (root / "report.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"Report: {root / 'report.md'}")
    print(f"Validation: {len(checks) - len(failed)} PASS / {len(failed)} FAIL")
    print(f"Data consistency: {len(audits)} PASS")


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
