#!/usr/bin/env python3
"""Validate one VolkEngine run summary against a named conservative scenario gate."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def finite_number(value: object, label: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
        raise ValueError(f"{label} must be a finite number")
    return float(value)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("summary", type=Path)
    parser.add_argument("--scenarios", type=Path, default=Path("benchmarks/scenarios.json"))
    parser.add_argument("--scenario", required=True)
    arguments = parser.parse_args()

    try:
        summary = load_json(arguments.summary)
        scenarios = load_json(arguments.scenarios)
        scenario = scenarios["scenarios"][arguments.scenario]
        failures: list[str] = []
        if summary.get("schema") != "volkengine.run-summary" or summary.get("schema_version") != 2:
            failures.append("run-summary schema/version mismatch")
        run = summary.get("run", {})
        if run.get("scenario") != arguments.scenario:
            failures.append(f"scenario mismatch: {run.get('scenario')!r}")
        if run.get("frame_count") != scenario["frames"]:
            failures.append(f"frame count {run.get('frame_count')} != {scenario['frames']}")
        if run.get("warmup_frames") != scenario["warmup_frames"]:
            failures.append(f"warmup {run.get('warmup_frames')} != {scenario['warmup_frames']}")
        if [run.get("resolution", {}).get("width"), run.get("resolution", {}).get("height")] != scenario["resolution"]:
            failures.append("resolution mismatch")

        limits = scenario["limits"]
        distributions = summary.get("distributions", {})
        for metric, limit_name in (("cpu_frame", "cpu_frame_p95_ms"), ("gpu_frame", "gpu_frame_p95_ms")):
            if limit_name not in limits:
                continue
            distribution = distributions.get(metric, {})
            expected_samples = scenario["frames"] - scenario["warmup_frames"]
            if not distribution.get("available") or distribution.get("sample_count") != expected_samples:
                failures.append(f"{metric} lacks {expected_samples} post-warmup samples")
                continue
            p95 = finite_number(distribution.get("p95"), f"{metric}.p95")
            if p95 > limits[limit_name]:
                failures.append(f"{metric} p95 {p95:.3f} ms > {limits[limit_name]:.3f} ms")
            if distribution.get("hitch_count", 0) > limits.get("hitches", sys.maxsize):
                failures.append(f"{metric} hitch count {distribution.get('hitch_count')} > {limits['hitches']}")

        graph = summary.get("frame_graph", {})
        if graph.get("transient_allocated_bytes", 0) > limits.get("graph_transient_bytes", sys.maxsize):
            failures.append("graph transient allocation exceeded scenario limit")
        if graph.get("recompile_count", 0) < limits.get("minimum_graph_recompiles", 0):
            failures.append("graph recompile count is below scenario minimum")
        if summary.get("validation", {}).get("required") and not summary.get("validation", {}).get("enabled"):
            failures.append("required Vulkan validation was not enabled")

        if failures:
            for failure in failures:
                print(f"REGRESSION: {failure}", file=sys.stderr)
            return 1
        print(f"PASS {arguments.scenario}: conservative regression gate satisfied")
        return 0
    except (KeyError, TypeError, ValueError) as error:
        print(f"INVALID EVIDENCE: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
