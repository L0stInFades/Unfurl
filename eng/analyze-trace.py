"""Summarize WPA exports from trace-ui.ps1 without counting grouped rows twice."""

import argparse
import csv
import json
import math
import statistics
from pathlib import Path


def number(value):
    return float(value.replace(",", ""))


def read_table(directory, prefix):
    paths = list(directory.glob(prefix + "*.csv"))
    if len(paths) != 1:
        raise ValueError(f"Expected one {prefix} CSV in {directory}")
    with paths[0].open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--refresh-hz", type=float, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.refresh_hz <= 0:
        parser.error("--refresh-hz must be positive")
    metadata = json.loads(args.metadata.read_text(encoding="utf-8-sig"))
    process = f"Unfurl.exe ({metadata['ProcessId']})"
    xaml = [
        row
        for row in read_table(args.csv, "Xaml_Frame_Analysis_")
        if row["Process"] == process and row["Count"] == "1" and row["Duration (ms)"]
    ]
    if not xaml:
        raise ValueError("No individual Unfurl XAML events; use the expanded Unfurl.wpaProfile")
    marks = {
        row["Mark"]: number(row["Time (s)"])
        for row in read_table(args.csv, "Marks_Summary_")
    }
    gpu = [
        row
        for row in read_table(args.csv, "GPU_Utilization_")
        if row["Process"] == process and row["Count"] == "1" and row["A/N/E"]
    ]
    phases = ["Startup", "Ready", "Resize", "OptionsMotion", "Navigation", "Idle", "Close"]
    result = {
        "ExecutableSHA256": metadata["SHA256"],
        "ProcessId": metadata["ProcessId"],
        "RefreshHz": args.refresh_hz,
        "FrameBudgetMs": 1000 / args.refresh_hz,
        "Method": "WPA XAML Frame Analysis and GPU exports; individual events only; nearest-rank P95.",
        "Phases": {},
    }
    for start_name, end_name in zip(phases, phases[1:]):
        start, end = marks["Unfurl." + start_name], marks["Unfurl." + end_name]
        rows = [row for row in xaml if start <= number(row["Start (s)"]) < end]
        durations = sorted(number(row["Duration (ms)"]) for row in rows if row["Type"] == "Frame")
        layouts = [number(row["Duration (ms)"]) for row in rows if row["Type"] == "UpdateLayout"]
        gpu_ms = sum(
            max(0, min(end, number(row["Finished (s)"])) - max(start, number(row["Start Execution (s)"]))) * 1000
            for row in gpu
        )
        result["Phases"][start_name] = {
            "Seconds": end - start,
            "Frames": len(durations),
            "MedianFrameMs": statistics.median(durations) if durations else None,
            "P95FrameMs": durations[math.ceil(len(durations) * 0.95) - 1] if durations else None,
            "MaxFrameMs": max(durations) if durations else None,
            "FramesOverRefreshInterval": sum(value > 1000 / args.refresh_hz for value in durations),
            "LayoutPasses": len(layouts),
            "LayoutTotalMs": sum(layouts),
            "GpuExecutionMs": gpu_ms,
        }
    cpu = [row for row in read_table(args.csv, "CPU_Usage_") if row["Process"] == process]
    if cpu:
        result["SampledCpuWeightMs"] = number(cpu[0]["Weight (in view) (ms)"])
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
