#!/usr/bin/env python3
"""Run the pilot sequentially, retain raw CSV and compute medians."""
import csv
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess

root = Path(__file__).resolve().parents[1]
os.chdir(root)
subprocess.run(["make", "build/benchmark"], check=True)
results = root / "results"
results.mkdir(exist_ok=True)
with (results / "benchmark_raw.csv").open("w") as output:
    subprocess.run([str(root / "build/benchmark")], check=True, stdout=output)
with (results / "benchmark_raw.csv").open() as source:
    rows = list(csv.DictReader(source))
keys = ["phase", "patterns", "items", "density", "move", "distance", "accept_fraction", "moves", "method"]
groups = {}
for row in rows:
    key = tuple(row[k] for k in keys)
    groups.setdefault(key, []).append(row)
with (results / "benchmark_medians.csv").open("w", newline="") as output:
    writer = csv.DictWriter(output, fieldnames=keys + ["repetitions", "median_seconds", "ns_per_proposal", "checksum"])
    writer.writeheader()
    for key, samples in groups.items():
        checksums = {s["checksum"] for s in samples}
        if len(checksums) != 1:
            raise RuntimeError("checksum varies across repetitions")
        median = statistics.median(float(s["seconds"]) for s in samples)
        values = dict(zip(keys, key))
        writer.writerow(values | {"repetitions": len(samples), "median_seconds": median,
                                   "ns_per_proposal": median * 1e9 / int(values["moves"]),
                                   "checksum": checksums.pop()})
cpu = "unknown"
if Path("/proc/cpuinfo").exists():
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            cpu = line.split(":", 1)[1].strip()
            break
environment = {
    "platform": platform.platform(), "cpu": cpu,
    "compiler": subprocess.check_output([os.environ.get("CXX", "g++"), "--version"], text=True).splitlines()[0],
    "flags": "-O3 -std=c++17 -Wall -Wextra -Wpedantic; no -march=native, no LTO",
    "runtime_threads_per_measurement": 1, "repetitions": 3, "seed": 20261029,
    "matrix_source": "synthetic", "raw_rows": len(rows), "median_rows": len(groups),
}
(results / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
print(json.dumps({"benchmark_status": "PASS", "raw_rows": len(rows), "median_rows": len(groups)}))
