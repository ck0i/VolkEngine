#!/usr/bin/env python3
"""Compare equal-sized P6 screenshots in linear light and emit a P6 diff."""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys


def read_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    cursor = 0

    def token() -> bytes:
        nonlocal cursor
        while cursor < len(data):
            if data[cursor] == 35:
                while cursor < len(data) and data[cursor] not in (10, 13):
                    cursor += 1
            elif chr(data[cursor]).isspace():
                cursor += 1
            else:
                break
        begin = cursor
        while cursor < len(data) and not chr(data[cursor]).isspace():
            cursor += 1
        if begin == cursor:
            raise ValueError(f"{path}: truncated PPM header")
        return data[begin:cursor]

    if token() != b"P6":
        raise ValueError(f"{path}: expected binary P6 PPM")
    width, height, maximum = int(token()), int(token()), int(token())
    if width <= 0 or height <= 0 or maximum != 255:
        raise ValueError(f"{path}: unsupported dimensions or max value")
    if cursor >= len(data) or not chr(data[cursor]).isspace():
        raise ValueError(f"{path}: missing PPM header separator")
    if data[cursor:cursor + 2] == b"\r\n":
        cursor += 2
    else:
        cursor += 1
    pixels = data[cursor:]
    expected = width * height * 3
    if len(pixels) != expected:
        raise ValueError(f"{path}: expected {expected} pixel bytes, got {len(pixels)}")
    return width, height, pixels


def linear(channel: int) -> float:
    value = channel / 255.0
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--diff", type=Path, required=True)
    parser.add_argument("--absolute-tolerance", type=float, default=0.012)
    parser.add_argument("--rms-tolerance", type=float, default=0.002)
    parser.add_argument("--differing-pixel-ratio", type=float, default=0.001)
    args = parser.parse_args()
    try:
        width, height, baseline = read_ppm(args.baseline)
        candidate_width, candidate_height, candidate = read_ppm(args.candidate)
        if (width, height) != (candidate_width, candidate_height):
            raise ValueError("screenshot dimensions differ")
        if min(args.absolute_tolerance, args.rms_tolerance, args.differing_pixel_ratio) < 0.0:
            raise ValueError("tolerances must be non-negative")

        squared_error = 0.0
        differing_pixels = 0
        maximum_error = 0.0
        diff = bytearray(len(baseline))
        for pixel in range(width * height):
            pixel_error = 0.0
            for channel in range(3):
                index = pixel * 3 + channel
                error = abs(linear(baseline[index]) - linear(candidate[index]))
                squared_error += error * error
                pixel_error = max(pixel_error, error)
                diff[index] = min(255, int(round(error * 1020.0)))
            maximum_error = max(maximum_error, pixel_error)
            if pixel_error > args.absolute_tolerance:
                differing_pixels += 1
        rms = math.sqrt(squared_error / len(baseline))
        ratio = differing_pixels / (width * height)
        args.diff.parent.mkdir(parents=True, exist_ok=True)
        args.diff.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + diff)
        print(f"pixels={width * height} differing={differing_pixels} ratio={ratio:.8f} "
              f"rms_linear={rms:.8f} max_linear={maximum_error:.8f} diff={args.diff}")
        if rms > args.rms_tolerance or ratio > args.differing_pixel_ratio:
            return 1
        return 0
    except (OSError, ValueError) as error:
        print(f"VISUAL COMPARISON ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
