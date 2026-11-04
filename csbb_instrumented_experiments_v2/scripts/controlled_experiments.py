#!/usr/bin/env python3
"""Reproducible, sequential, paired ablation benchmark for MOSP Chu--Stuckey v5.

Uses 1 real FCS instance of each of 400, 600, 800, 1000. Each comparison
changes exactly one specified factor; common MCNh certificates are generated
once and reused. No asynchronous/background tasks are launched. Python 3.9+.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
import os
import platform
import random
import re
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, replace
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT = SCRIPT_DIR.parent
DEFAULT_INPUTS = [PROJECT / f"instances/frinhani_carvalho_soma/{n}/Random-{n}-{n}-2-1.txt" for n in [400, 600, 800, 1000]]

# All parameters not being varied are explicit and stable across treatment pairs.
BASE = {
    "auto-lower-mode": "legacy", "residual-lb": "off", "residual-core-max-vertices": 192,
    "sequence-evaluator": "both", "symmetry-break": "off", "anchor-pattern": "auto",
    "branching": "customer", "canonical-closures": 1,
    "relaxation": "off", "relax-merge-policy": "original", "relax-unmerge-policy": "original",
    "relax-max-time-share": 0.25,
    "relax-proof-depth": 1, "relax-proof-frontier-cap": 8192,
    "closure-parallel-depth": 1, "closure-frontier-cap": 8192,
    "hash": 1, "sort-moves": 1, "group-moves": 1,
    "parallel-depth": 1, "profile": 1,
}

@dataclass(frozen=True)
class Variant:
    key: str
    description: str
    overrides: dict
    warm: str = "on"     # on, off, none (precomputed MCNh variants)

@dataclass(frozen=True)
class Comparison:
    key: str
    label: str
    control: str
    treatment: str
    rationale: str
    optional: bool = False


def variants_and_pairs(workers: int):
    V = [
        Variant("baseline_no_warm", "Customer B&B serial, legacy LB, sem warm start", {"threads": 1}, "none"),
        Variant("baseline_warm", "Mesmo B&B, MCNh com preprocessing", {"threads": 1}),
        Variant("warm_without_preprocess", "MCNh sem preprocessing", {"threads": 1}, "off"),
        Variant("graph_lb", "Lower bound inicial no grafo de clientes", {"threads": 1, "auto-lower-mode": "graph"}),
        Variant("residual_next", "Bound residual de próxima decisão", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next"}),
        Variant("residual_core", "Teste residual k-core", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "core"}),
        Variant("root_probe", "Root-merge probe (sem prova contraída)", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next", "relaxation": "root-merge-probe"}),
        Variant("full_relax_serial", "Relaxação completa com prova serial", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next", "relaxation": "full", "relax-proof-threads": 1}),
        Variant("customer_parallel", "Customer search paralelo, sem relaxação", {"threads": workers, "auto-lower-mode": "graph", "residual-lb": "next"}),
        Variant("full_relax_parallel_control", "Busca customer paralela, provas relaxadas seriais", {"threads": workers, "auto-lower-mode": "graph", "residual-lb": "next", "relaxation": "full", "relax-proof-threads": 1}),
        Variant("full_relax_parallel", "Mesma config., provas relaxadas paralelas", {"threads": workers, "auto-lower-mode": "graph", "residual-lb": "next", "relaxation": "full", "relax-proof-threads": workers}),
        Variant("close_stack_serial", "Fechamento de pilhas serial, fallback paralelo", {"threads": workers, "auto-lower-mode": "graph", "residual-lb": "next", "branching": "close-stack", "closure-threads": 1}),
        Variant("close_stack_parallel", "Fechamento de pilhas paralelo, fallback paralelo", {"threads": workers, "auto-lower-mode": "graph", "residual-lb": "next", "branching": "close-stack", "closure-threads": workers}),
        Variant("hash_disabled", "Hash/dominância desativada", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next", "hash": 0}),
        Variant("sort_disabled", "Ordenação de movimentos desativada", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next", "sort-moves": 0}),
        Variant("group_disabled", "Agrupamento de movimentos desativado", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next", "group-moves": 0}),
        Variant("symmetry_reverse", "Reversão de padrão âncora no warm start", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next", "symmetry-break": "reverse-anchor"}),
        Variant("unmerge_lifo", "Política LIFO de desfusão", {"threads": 1, "auto-lower-mode": "graph", "residual-lb": "next", "relaxation": "full", "relax-proof-threads": 1, "relax-unmerge-policy": "lifo"}),
    ]
    P = [
        Comparison("warm_start", "MCNh como warm start", "baseline_no_warm", "baseline_warm", "Única mudança: presença do certificado verificado do MCNh"),
        Comparison("graph_lb", "Lower bound inicial por grafo", "baseline_warm", "graph_lb", "Única mudança: --auto-lower-mode legacy→graph"),
        Comparison("residual_next", "LB residual next", "graph_lb", "residual_next", "Única mudança: residual-lb off→next"),
        Comparison("residual_core", "LB residual core", "residual_next", "residual_core", "Única mudança: residual-lb next→core"),
        Comparison("root_probe", "Relaxação root-merge-probe", "residual_next", "root_probe", "Única mudança: relaxation off→root-merge-probe"),
        Comparison("full_relax", "Relaxação full serial", "residual_next", "full_relax_serial", "Única mudança: relaxation off→full (prova serial)"),
        Comparison("customer_parallel", "Paralelismo customer", "residual_next", "customer_parallel", "Única mudança: threads 1→N (N físico−1); mesmo branching e bounds"),
        Comparison("relaxed_proof_parallel", "Paralelismo da prova relaxada", "full_relax_parallel_control", "full_relax_parallel", "Única mudança: relax-proof-threads 1→N; customer fallback N em ambos"),
        Comparison("close_stack_branching", "Branching close-stack vs customer", "customer_parallel", "close_stack_parallel", "Única mudança: branching customer→close-stack (closure usa N); fallback inalterado"),
        Comparison("close_stack_parallel", "Paralelismo de close-stack", "close_stack_serial", "close_stack_parallel", "Única mudança: closure-threads 1→N, mantendo customer fallback N"),
        Comparison("mcnh_preprocess", "Pré-processamento do MCNh", "warm_without_preprocess", "baseline_warm", "Única mudança: pré-processamento do MCNh 0→1", True),
        Comparison("hash", "Tabela de dominância", "hash_disabled", "residual_next", "Única mudança: hash 0→1", True),
        Comparison("sort", "Ordenação de movimentos", "sort_disabled", "residual_next", "Única mudança: sort-moves 0→1", True),
        Comparison("group", "Agrupamento de movimentos", "group_disabled", "residual_next", "Única mudança: group-moves 0→1", True),
        Comparison("reverse_anchor", "Simetria de reversão", "residual_next", "symmetry_reverse", "Única mudança: symmetry-break off→reverse-anchor no warm start", True),
        Comparison("unmerge_policy", "Política de desfusão", "full_relax_serial", "unmerge_lifo", "Única mudança: relax-unmerge-policy original→lifo", True),
    ]
    return {v.key: v for v in V}, {p.key: p for p in P}


def sha(path: Path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def physical_cores():
    """Count distinct (socket, physical core) from CPU IDs allowed by affinity."""
    affinity = set(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    if sys.platform.startswith("linux") and shutil.which("lscpu"):
        try:
            t = subprocess.check_output(["lscpu", "--parse=CPU,CORE,SOCKET"], text=True, timeout=4)
            cpus = set()
            for s in t.splitlines():
                if s.startswith("#") or not s.strip():
                    continue
                cpu, core, socket = map(int, s.split(",")[:3])
                if affinity is None or cpu in affinity:
                    cpus.add((socket, core))
            if cpus:
                return len(cpus), "lscpu / available CPU affinity"
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
    if sys.platform == "darwin":
        try:
            n = int(subprocess.check_output(["sysctl", "-n", "hw.physicalcpu"], text=True, timeout=4).strip())
            return max(1, n), "sysctl hw.physicalcpu"
        except (OSError, ValueError, subprocess.SubprocessError):
            pass
    try:
        import psutil
        n = psutil.cpu_count(logical=False)
        if n:
            return n, "psutil physical (may not reflect affinity)"
    except ImportError:
        pass
    return len(affinity) if affinity else (os.cpu_count() or 1), "logical fallback (WARNING: may overestimate physical cores)"


def read_instance_shape(p: Path):
    with p.open(encoding="utf-8") as f:
        parts = f.readline().split()
    if len(parts) < 2:
        raise ValueError(f"Missing CS dimensions: {p}")
    return tuple(map(int, parts[:2]))


def check_binary(p: Path, exe: Path):
    if not exe.is_file():
        raise FileNotFoundError(f"Missing executable {exe}: run ./scripts/build.sh")


def atomic_json(path: Path, obj):
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, ensure_ascii=False, indent=2, default=str), encoding="utf-8")
    tmp.replace(path)


def run_process(cmd, stdout_path: Path, stderr_path: Path, timeout: float):
    t0 = time.perf_counter()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
        rc, out, err, issue = p.returncode, p.stdout, p.stderr, None
    except subprocess.TimeoutExpired as e:
        rc, out, err, issue = 124, e.stdout or b"", e.stderr or b"", "EXTERNAL_TIMEOUT"
        if isinstance(out, bytes): out = out.decode("utf-8", "replace")
        if isinstance(err, bytes): err = err.decode("utf-8", "replace")
    except OSError as e:
        rc, out, err, issue = 127, "", str(e), "EXEC_ERROR"
    elapsed = time.perf_counter() - t0
    stdout_path.write_text(out, encoding="utf-8")
    stderr_path.write_text(err, encoding="utf-8")
    return rc, out, err, elapsed, issue


def parse_solver_json(stdout: str):
    try:
        result = json.loads(stdout.strip())
        if not isinstance(result, dict): raise ValueError("expected object")
        return result
    except (ValueError, json.JSONDecodeError) as e:
        raise ValueError(f"Solver output is not a single valid JSON object: {e}")


def solver_command(solver, instance, variant: Variant, warm, limit, node_limit, progress_file=None, progress_interval=0.0):
    d = dict(BASE)
    d.update(variant.overrides)
    cmd = [str(solver), "--input", str(instance), "--input-format", "cs", "--presence", "legacy-one"]
    for k, v in d.items():
        cmd.extend(["--" + k, str(v)])
    if warm is not None:
        cmd += ["--warm-start-file", str(warm), "--warm-start-policy", "strict"]
    cmd += ["--time-limit", str(limit), "--json", "--quiet"]
    if progress_interval > 0 and progress_file is not None:
        cmd += ["--progress-interval", str(progress_interval), "--progress-file", str(progress_file)]
    if node_limit is not None:
        cmd += ["--node-limit", str(node_limit)]
    return cmd


def validated_output(data, warm_expected):
    if not isinstance(data, dict) or data.get("status") not in {"OPTIMAL", "TIME_LIMIT", "NODE_LIMIT", "INFEASIBLE"}:
        return "INVALID_STATUS"
    if warm_expected and (data.get("warm_start_verified") is not True or data.get("warm_start_validation_status") != "VERIFIED"):
        return "INVALID_WARM_START"
    u, l = data.get("upper_bound"), data.get("lower_bound")
    if not isinstance(u, (int, float)) or not isinstance(l, (int, float)) or not math.isfinite(u) or not math.isfinite(l) or u < l:
        return "INVALID_BOUNDS"
    if data.get("status") == "OPTIMAL" and u != l:
        return "INVALID_OPTIMALITY"
    return None


def ensure_warm(mcnh, inst, dim, prep, root, timeout, dry_run):
    target = root / "warm_starts" / f"{dim}_preprocess{prep}.warm"
    log = root / "logs" / f"MCNh_{dim}_preprocess{prep}"
    cmd = [str(mcnh), "--input", str(inst), "--input-format", "cs", "--presence", "legacy-one", "--objective-orientation", "customer", "--preprocess", str(prep), "--warm-start-output", str(target)]
    if dry_run:
        print("WARM", " ".join(map(str, cmd)))
        return target, {"command": cmd, "generated": False}
    if target.exists():
        # Warm starts cached in a campaign directory are immutable under resume;
        # the manifest contains the exact instance and MCNh executable hashes.
        return target, {"command": cmd, "reused": True, "sha256": sha(target)}
    rc, out, err, elapsed, issue = run_process(cmd, Path(str(log) + ".stdout.log"), Path(str(log) + ".stderr.log"), timeout)
    if rc != 0 or not target.is_file():
        raise RuntimeError(f"MCNh failed for dimension={dim}, preprocess={prep}: rc={rc} {issue}, stderr={err[-400:]}")
    return target, {"command": cmd, "elapsed_seconds": elapsed, "sha256": sha(target)}


def selected_variants(pairs, definitions):
    return {key: definitions[key] for p in pairs for key in (p.control, p.treatment)}


def cli(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    p.add_argument("--solver", type=Path, default=PROJECT / "csbb_modern")
    p.add_argument("--mcnh", type=Path, default=PROJECT / "mcnh_modern")
    p.add_argument("--instances", nargs=4, type=Path, metavar=("I400", "I600", "I800", "I1000"), default=DEFAULT_INPUTS,
                   help="Exactly one CS instance in each dimension; order 400,600,800,1000")
    p.add_argument("--output-dir", type=Path, default=PROJECT / "results" / "controlled_experiments")
    p.add_argument("--suite", choices=["core", "extended"], default="core")
    p.add_argument("--comparisons", help="Comma-separated subset of comparison IDs (use --list-comparisons)")
    p.add_argument("--list-comparisons", action="store_true")
    p.add_argument("--time-limit", type=float, default=3600, help="Per B&B execution, seconds; MCNh precomputation reported separately")
    p.add_argument("--warm-timeout", type=float, default=300)
    p.add_argument("--external-grace", type=float, default=45)
    p.add_argument("--progress-interval", type=float, default=30.0,
                   help="Seconds between progress snapshots (0 disables CSV monitoring); reports also calculate gap area")
    p.add_argument("--node-limit", type=int)
    p.add_argument("--threads", default="auto", help="auto: available physical cores minus reserved; or a positive integer")
    p.add_argument("--reserve-cores", type=int, default=1)
    p.add_argument("--repeats", type=int, default=1, help="Use >=3 for repeatability studies of parallel variants")
    p.add_argument("--resume", action="store_true", help="Reuse completed result JSONs only when manifest signature is identical")
    p.add_argument("--dry-run", action="store_true", help="Print full planned commands without launching MCNh or B&B")
    p.add_argument("--report-only", action="store_true", help="Regenerate reports from an existing campaign manifest/results")
    p.add_argument("--seed", type=int, default=20261028, help="Deterministic rotation of variant execution order, not solver RNG")
    return p.parse_args(argv)


def main(argv=None):
    a = cli(argv)
    count, source = physical_cores()
    if a.threads == "auto":
        workers = max(1, count - max(0, a.reserve_cores))
    else:
        workers = int(a.threads)
    if workers < 1 or a.repeats < 1 or a.time_limit <= 0 or a.external_grace < 0 or a.warm_timeout <= 0 or a.progress_interval < 0:
        raise SystemExit("Invalid threads/repeats/time-limit/grace/warm-timeout")
    variants, comps = variants_and_pairs(workers)
    if a.list_comparisons:
        for p in comps.values():
            print(f"{p.key:27s}  {'EXTENDED' if p.optional else 'CORE':8s} {p.control} -> {p.treatment}: {p.rationale}")
        return 0
    if a.comparisons:
        keys = [x.strip() for x in a.comparisons.split(",") if x.strip()]
        bad = set(keys) - set(comps)
        if bad: raise SystemExit(f"Unknown comparisons: {sorted(bad)}")
        chosen = [comps[k] for k in keys]
    else:
        chosen = [p for p in comps.values() if a.suite == "extended" or not p.optional]
    if not chosen: raise SystemExit("No comparison selected")
    used = selected_variants(chosen, variants)
    if workers == 1:
        print("WARNING: workers=1, parallel ablations cannot exhibit speedup; they remain correctness/control tests", file=sys.stderr)
    paths = [p.resolve() for p in a.instances]
    if not a.report_only:
        for p, expected in zip(paths, [400, 600, 800, 1000]):
            if not p.is_file(): raise SystemExit(f"Instance missing: {p}")
            shape = read_instance_shape(p)
            if shape != (expected, expected): raise SystemExit(f"Wrong shape {shape} for {p}, expected {expected}x{expected}")
        a.solver = a.solver.resolve(); a.mcnh = a.mcnh.resolve()
        if not a.dry_run:
            check_binary(paths[0], a.solver)
            if any(v.warm != "none" for v in used.values()): check_binary(paths[0], a.mcnh)
    out = a.output_dir.resolve()
    config = {
        "protocol_version": "controlled-v2-telemetry", "date_created": datetime.now(timezone.utc).isoformat(),
        "cpu": platform.processor(), "platform": platform.platform(), "physical_cores": count, "physical_core_source": source,
        "threads": workers, "progress_interval_seconds": a.progress_interval, "repeats": a.repeats, "time_limit_seconds": a.time_limit, "node_limit": a.node_limit,
        "seed": a.seed, "suite": a.suite, "comparison_ids": [p.key for p in chosen],
        "comparisons": [p.__dict__ for p in chosen], "variants": {k: v.__dict__ for k, v in used.items()},
        "instances": [{"dimension": size, "path": str(p), "sha256": sha(p) if p.exists() else ""} for size, p in zip([400,600,800,1000],paths)],
        "solver": str(a.solver), "solver_sha256": sha(a.solver) if a.solver.exists() else "",
        "mcnh": str(a.mcnh), "mcnh_sha256": sha(a.mcnh) if a.mcnh.exists() else "",
    }
    # Stable signature deliberately excludes date and system-only info.
    signature_keys = ["threads", "repeats", "time_limit_seconds", "node_limit", "progress_interval_seconds", "seed", "comparison_ids", "variants", "instances", "solver_sha256", "mcnh_sha256"]
    config["signature"] = hashlib.sha256(json.dumps({k: config[k] for k in signature_keys}, sort_keys=True).encode()).hexdigest()
    if a.dry_run:
        print(f"HOST physical cores={count} ({source}), B&B workers={workers}; runs={4*len(used)*a.repeats}; comparisons={len(chosen)}")
        for inst, size in zip(paths, [400,600,800,1000]):
            for w in sorted({v.warm for v in used.values()} - {"none"}):
                ensure_warm(a.mcnh, inst, size, 1 if w == "on" else 0, out, a.warm_timeout, True)
            for k, v in used.items():
                warm = None if v.warm == "none" else out / "warm_starts" / f"{size}_preprocess{int(v.warm == 'on')}.warm"
                print(size, k, subprocess.list2cmdline(solver_command(a.solver, inst, v, warm, a.time_limit, a.node_limit,
                    out/"progress"/f"{size}__{k}__r01.csv" if a.progress_interval>0 else None,a.progress_interval)))
        return 0
    if a.report_only:
        from controlled_report import generate
        generate(out)
        return 0
    for d in [out, out/"logs", out/"warm_starts", out/"runs", out/"reports", out/"progress"]:
        d.mkdir(parents=True, exist_ok=True)
    manifest = out / "manifest.json"
    if manifest.exists():
        old = json.loads(manifest.read_text(encoding="utf-8"))
        if old.get("signature") != config["signature"]:
            raise SystemExit("Campaign manifest differs from requested settings or binaries. Choose a new output directory; do NOT mix results.")
        if not a.resume:
            raise SystemExit("Output folder already contains a campaign. Pass --resume or use a new output directory.")
    else:
        atomic_json(manifest, config)
    old_prep_file = out / "warm_preparation.json"
    old_prep = json.loads(old_prep_file.read_text(encoding="utf-8")) if old_prep_file.exists() else {}
    warm_files = {}; prep = {}
    for size, inst in zip([400,600,800,1000],paths):
        for mode in sorted({v.warm for v in used.values()} - {"none"}):
            key = (size, mode)
            wf, meta = ensure_warm(a.mcnh, inst, size, int(mode=="on"), out, a.warm_timeout, False)
            warm_files[key] = wf
            prev = old_prep.get(str(key), {})
            if prev.get("sha256") and prev["sha256"] != sha(wf):
                raise SystemExit(f"Cached warm-start file changed: {wf}; do not mix certificates within a campaign")
            # Preserve the original generation cost when a certificate is reused.
            if prev.get("sha256"):
                meta = {**prev, "reused": True}
            prep[str(key)] = meta
    atomic_json(old_prep_file, prep)
    # A shared verified certificate is reused for all on/off comparisons. Never modify it in a run.
    results = out / "runs"
    total = 4 * len(used) * a.repeats
    done = 0
    for r in range(1, a.repeats+1):
        for size, inst in zip([400,600,800,1000],paths):
            # Deterministic per-dimension/repetition rotation to reduce systematic execution-order bias.
            order = list(used)
            random.Random(a.seed + r*100000 + size).shuffle(order)
            for key in order:
                done += 1
                variant = used[key]
                path = results / f"{size}__{key}__r{r:02d}.json"
                if a.resume and path.exists():
                    print(f"[{done}/{total}] RESUME {size}: {key}, repetition={r}", flush=True)
                    continue
                warm = None if variant.warm == "none" else warm_files[(size, variant.warm)]
                progress_file = out / "progress" / f"{path.stem}.csv" if a.progress_interval > 0 else None
                cmd = solver_command(a.solver, inst, variant, warm, a.time_limit, a.node_limit,
                                     progress_file, a.progress_interval)
                logbase = out / "logs" / path.stem
                print(f"[{done}/{total}] {size}x{size}: {key}, repetition={r}", flush=True)
                rc, stdout, stderr, elapsed, issue = run_process(cmd, Path(str(logbase) + ".out"), Path(str(logbase) + ".err"), a.time_limit + a.external_grace)
                parsed = {}
                if rc == 0 and not issue:
                    try: parsed = parse_solver_json(stdout)
                    except ValueError as e: issue = str(e)
                error = issue or (f"return_code={rc}: {stderr[-500:]}" if rc != 0 else validated_output(parsed, warm is not None))
                record = {"dimension": size, "instance": inst.name, "repetition": r, "variant": key,
                          "warm_mode": variant.warm, "command": cmd, "return_code": rc,
                          "wall_seconds": elapsed, "error": error, "result": parsed,
                          "log_stdout": str(logbase) + ".out", "log_stderr": str(logbase) + ".err",
                          "progress_csv": str(progress_file) if progress_file else None}
                atomic_json(path, record)
                if error: print(f"  WARNING: {error}", file=sys.stderr, flush=True)
            from controlled_report import generate
            generate(out)   # incremental checkpoint at each dimension, including summary/report
    print(f"Done: {out / 'reports' / 'report.md'}", flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
