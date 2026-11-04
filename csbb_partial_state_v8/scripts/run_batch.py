#!/usr/bin/env python3
"""
Batch runner for the modernized Chu--Stuckey branch-and-bound for MOSP.

This script intentionally keeps campaign logic outside the solver. It runs one
instance after another, optionally generates an MCNh warm start for each file,
automatically chooses the number of physical CPU cores minus a reserve, stores
individual logs, and writes consolidated CSV/JSONL outputs.

Predefined configurations:
  original-modernized : modernized B&B backbone, close to the original method.
  low-risk            : B&B with low-risk model-based additions.
  close-stack         : low-risk configuration plus close-stack branching.

Use:
  python3 scripts/run_batch.py --describe-parameters
  python3 scripts/run_batch.py --list-presets
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import platform
import re
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


# ---------------------------------------------------------------------------
# Presets
# ---------------------------------------------------------------------------

@dataclass
class Preset:
    name: str
    description: str
    use_mcnh: bool
    mcnh_preprocess: int
    solver_args: Dict[str, Any] = field(default_factory=dict)


PRESETS: Dict[str, Preset] = {
    "original-modernized": Preset(
        name="original-modernized",
        description=(
            "Modernized Chu--Stuckey-style B&B backbone. It keeps customer "
            "branching, legacy lower-bound behavior, no imported sequence "
            "symmetry manipulation, and no root-merge probe. This is the "
            "closest preset to the original exact method after engineering "
            "modernization."
        ),
        use_mcnh=False,
        mcnh_preprocess=0,
        solver_args={
            "auto_lower_mode": "legacy",
            "sequence_evaluator": "native",
            "symmetry_break": "off",
            "anchor_pattern": "auto",
            "branching": "customer",
            "relaxation": "off",
            "canonical_closures": 1,
            "hash": 1,
            "sort_moves": 1,
            "group_moves": 1,
        },
    ),
    "low-risk": Preset(
        name="low-risk",
        description=(
            "Recommended enhanced configuration with low-risk model-based "
            "additions: MCNh warm start, graph/model lower bounds, independent "
            "sequence validation, reverse-anchor handling for warm starts, "
            "customer branching, and conservative root-merge probing."
        ),
        use_mcnh=True,
        mcnh_preprocess=1,
        solver_args={
            "auto_lower_mode": "graph",
            "sequence_evaluator": "both",
            "symmetry_break": "reverse-anchor",
            "anchor_pattern": "auto",
            "branching": "customer",
            "relaxation": "root-merge-probe",
            "canonical_closures": 1,
            "warm_start_policy": "value",
            "hash": 1,
            "sort_moves": 1,
            "group_moves": 1,
        },
    ),
    "close-stack": Preset(
        name="close-stack",
        description=(
            "Experimental full close-stack branching configuration. It uses "
            "the low-risk additions and activates branching by stack closure. "
            "The solver keeps fallback safeguards to the customer-search "
            "backbone for proof safety."
        ),
        use_mcnh=True,
        mcnh_preprocess=1,
        solver_args={
            "auto_lower_mode": "graph",
            "sequence_evaluator": "both",
            "symmetry_break": "reverse-anchor",
            "anchor_pattern": "auto",
            "branching": "close-stack",
            "relaxation": "root-merge-probe",
            "canonical_closures": 1,
            "warm_start_policy": "value",
            "hash": 1,
            "sort_moves": 1,
            "group_moves": 1,
        },
    ),
}


PARAMETER_HELP = r"""
Batch script parameters
=======================

Core campaign parameters
------------------------
--instances-dir DIR
    Folder containing instance files. Files are executed sequentially.

--file-glob PATTERN
    Glob pattern relative to --instances-dir. Can be repeated. Default: *.txt and *.dat.

--output-dir DIR
    Folder for consolidated outputs. The script creates logs/, warm_starts/, json/, and profiles/.

--preset original-modernized|low-risk|close-stack
    Selects one of the three predefined configurations.

--solver FILE
    Path to csbb_modern executable.

--mcnh FILE
    Path to mcnh_modern executable. Required when --use-mcnh 1.

--resume 0|1
    If 1, skip an instance when its per-instance JSON output already exists.

--dry-run
    Print commands without executing them.

Instance interpretation
-----------------------
--input-format cs|scoop
    cs: Frinhani--Carvalho--Soma / Chu--Stuckey matrix format, first line m n,
        followed by an m x n pattern-by-piece matrix.
    scoop: legacy MCNh/SCOOP format, first line nPieces nPatterns, followed by
        the matrix in the legacy order.

--presence legacy-one|positive
    legacy-one: a piece is present only when the entry is exactly 1. Recommended
        for binary CS instances.
    positive: any positive entry means presence. Recommended for SCOOP-like
        instances with multiplicities.

Parallel execution
------------------
--threads auto|N
    auto detects the number of physical CPU cores and uses cores - --reserve-cores.
    An integer N passes N directly to the solver.

--reserve-cores N
    Number of physical cores kept free when --threads auto. Default: 1.

--parallel-depth D
    Branch-and-bound parallel frontier depth. Recommended initial value: 1 for
    400x400; increase only after profiling.

--closure-threads N
    Workers for close-stack branching (default 0 = inherit --threads).
    Only used for --branching close-stack|auto; 1 forces serial closure search.

--closure-parallel-depth D
    Depth used to generate the close-stack parallel frontier.
    Default -1 = inherit --parallel-depth; 0 forces serial closure search.

--closure-frontier-cap N
    Maximum close-stack frontier tasks (default 8192, must be >= 1).
    If splitting would overflow the cap, the whole parent subtree is kept.
    The cap NEVER truncates the search or proves infeasibility on its own.

Stopping criteria
-----------------
--time-limit SECONDS
    Time limit passed to the B&B. The solver reports UB, LB and gap if stopped.

--node-limit N
    Optional node limit passed to the B&B.

Warm start / MCNh
-----------------
--use-mcnh 0|1
    If 1, run MCNh before B&B for each instance and pass the generated warm-start file.

--mcnh-preprocess 0|1
    Activates dominance preprocessing inside MCNh. Ignored when --use-mcnh 0.

--warm-start-policy value|sequence|strict
    value: require MCNh's declared value to equal two independently checked
        costs of a complete PRODUCT permutation for the original CS matrix.
    sequence: ignore the declared value and use the independently verified cost.
    strict: same verified cost requirement as value; rejects absent/mismatched value.
    All modes reject unlabelled, wrongly oriented, incomplete or duplicated sequences.
    The MCNh is invoked with --objective-orientation customer automatically.

B&B algorithm parameters
------------------------
--auto-lower-mode off|legacy|model|graph
    off: no automatic lower bound.
    legacy: preserves legacy lower-bound behavior.
    model: compatibility alias using proven legacy bound; model cardinality is logged as diagnostic.
    graph: max(legacy, CUSTOMER co-occurrence graph degeneracy+1), a proved LB.
    The old PRODUCT graph bounds remain logged as diagnostics only.

--residual-lb off|next|core
    off: unchanged previous search (default, backward-compatible).
    next: admissible minimum immediate peak over every possible first customer closure.
    core: next plus a target-specific k-core test of the induced remaining CUSTOMER graph.
    Both work in serial, parallel-frontier and exact contracted-graph proof.

--partial-state-eval off|counts
    off: mantém o estado customer original (padrão).
    counts: armazena |N(c) \ O| por cliente e atualiza/desfaz as contagens por subárvore DFS;
    usa cache para fechamento forçado, geração de movimentos e bound residual next.
    Aplica-se também às provas relaxadas e ao fallback do close-stack, sem compartilhar cache entre workers.
--partial-state-verify 0|1
    Verificação diagnóstica do cache em todos os nós (lenta); requer --partial-state-eval counts.

--residual-core-max-vertices N
    Skip the costlier k-core test when more than N customers remain; still apply next.
    Default 192; 0 disables the size cap (useful for small tests, may be expensive).

--witness-evaluator legacy|standalone|dual
    Mode of product-sequence certification: original interval+streaming; standalone+sparse streaming;
    or full three-way audit. Standalone never accepts value-only or malformed MCNh certificates.
--witness-decode legacy|sparse|dual
    Customer-order decoding. Sparse uses incident products only; dual compares cost AND exact
    reconstructed product order, aborting on any disagreement. This is different from importing
    a PRODUCT permutation and never changes a bound's mathematical meaning.
--verify-customer-witness 0|1
    Independently verify each accepted customer-order witness against the original instance;
    requires a reconstructible full customer permutation. May add time in frequent-feasibility searches.
--refine-incumbent 0|1
    Optional incremental search after a VERIFIED MCNh PRODUCT certificate.
    Requires --use-mcnh 1 in batch mode. Default 0. Never affects B&B proof rules.
--refine-moves insert|swap|reverse|mixed
    Neighborhood for local refinement, default mixed.
--refine-max-evals N
    Maximum number of candidate move evaluations (default 2500).
--refine-max-seconds S
    Wall-clock refinement budget included in the B&B time limit (default 0.5 s).
--refine-seed N
    Reproducible PRNG seed (default 1).
--refine-mmosp-ties 0|1
    Accept equal-MOSP moves only when total stack-time decreases (default 1).
    MMOSP is a secondary search criterion, never the certified MOSP objective.
--refine-output FILE
    Set to 1 to emit one verified PRODUCT certificate per instance under output-dir/refined_starts/. Requires refinement and MCNh.

--profile-evaluator 0|1
    Collect call counts and evaluation/decode time, including full-witness checks.
--sequence-evaluator native|faggioli|both
    Legacy evaluator selector for internally represented customer orders.
    Imported MCNh product sequences are ALWAYS checked using two independent
    original-instance MOSP evaluators, regardless of this selector.

--symmetry-break off|reverse-anchor
    reverse-anchor can reverse an imported pattern sequence so that the selected
    anchor pattern is positioned in the first half. It is intended for warm-start
    pattern sequences, not as a proof rule in the customer-search tree.

--anchor-pattern auto|ID
    Anchor pattern for reverse-anchor symmetry handling. ID is 1-based. Use only
    with --symmetry-break reverse-anchor; otherwise it has no effect.

--branching customer|close-stack|auto
    customer: modernized Chu--Stuckey customer-search backbone.
    close-stack: branching by stack closure with safety fallback.
    auto: reserved for adaptive selection.

--relaxation off|root-merge-probe|full
    off: no relaxation/probe.
    root-merge-probe: conservative root-level merge-inspired probe for incumbents.
    full: reversible graph contractions and exact proof of infeasibility on contracted graphs;
        on inconclusive cases, selective unmerge and certified original-graph fallback.

--relax-merge-policy original
    Chu--Stuckey (2009) danger-based root-level adjacent-customer merge selection.

--relax-unmerge-policy original|lifo
    original: preferentially undo a merge whose removal invalidates the lifted
        relaxed witness, following the 2009 findunmerge selection principle.
    lifo: undo the most recent merge (deterministic lower overhead).

--relax-max-merges N
    Maximum initial root-level contractions. 0: until active customers <= target.

--relax-max-time-share P
    Fraction P in [0,1] of the total positive --time-limit spent on full relaxation;
    after that, continue original-graph search, subject to remaining time.

--relax-proof-threads N
    Threads allocated to exact proofs on contracted graphs. 0 (default) inherits --threads.
    1 forces serial contracted proofs even if the original-graph fallback is parallel.
--relax-proof-depth D
    Frontier expansion depth within each contracted graph. -1 (default) inherits --parallel-depth;
    0 forces serial contracted proofs. Used only when --relaxation full.

--relax-proof-frontier-cap N
    Maximum frontier states for each contracted-graph proof (default 8192).
    Subtrees that would overflow the cap remain intact as leaves; no search is lost.

--canonical-closures 0|1
    Canonicalizes simultaneous stack closures. Default/recommended: 1.

--emit-stack-profile 0|1
    If 1, creates one stack-profile CSV per instance and passes --emit-stack-profile.

--progress-interval SECONDS
    Registra séries temporais de nós por fase, UB, LB, gap e provas relaxadas
    concluídas em `output-dir/progress/INSTANCIA.csv`. Padrão 30 s; 0 desativa.
    Junto a --time-limit, permite AUC-gap e curvas de convergência.

--profile 0|1
    If 1, requests solver component timings and stores them in the output.

Low-level B&B parameters
------------------------
--hash 0|1
    Enables dominance/hash memoization. Disabling it is for diagnostics only.

--sort-moves 0|1
    Enables move ordering. Recommended: 1.

--group-moves 0|1
    Enables move grouping. Recommended: 1. Normally aligned with canonical closures.

Important incompatibilities and cautions
========================================
1. --warm-start-policy sequence or strict requires a warm-start file. In this
   script that means either --use-mcnh 1 or an externally supplied warm-start
   mechanism added by the user.

2. --symmetry-break reverse-anchor does not transform imported PRODUCT
   sequences used as MCNh certificates. It is retained only for older
   internal customer-close-order functionality; strict is safe with it.

3. --anchor-pattern ID is meaningful only with --symmetry-break reverse-anchor.
   With --symmetry-break off, it is ignored.

4. --mcnh-preprocess is meaningful only with --use-mcnh 1.

5. --branching close-stack|auto activates optional parallel closure search when
   --closure-threads > 1 and --closure-parallel-depth > 0 (both inherit by default).
   The closure search remains an experimental FEASIBILITY search, with exact
   customer-search fallback for all unproved failures. --branching customer
   ignores the --closure-* settings. Never treat closure-only failure as an
   infeasibility certificate.

6. --relaxation root-merge-probe is a heuristic probe; --relaxation full enables
   exact minor-graph infeasibility proof and selective unmerge. ALL modes verify
   imported MCNh PRODUCT sequences on the ORIGINAL matrix; unverified numeric
   --upper hints are ignored. Full mode also retains its verified greedy
   customer-order fallback. It also
   reverts initial LB to the proven legacy product-simultaneity bound; previous
   model/graph lower bounds require separate mathematical validation.

7. --input-format and --presence must match the dataset. For the binary random
   CS instances, use --input-format cs --presence legacy-one.

8. --threads auto is handled by this script, not by the solver. The solver always
   receives an explicit integer thread count.

9. Running several instances in parallel externally while also using --threads > 1
   inside the solver may oversubscribe the machine. This script intentionally runs
   instances sequentially.
"""


def detect_physical_cores() -> int:
    """Best-effort physical-core detection, with portable fallbacks."""
    try:
        import psutil  # type: ignore
        n = psutil.cpu_count(logical=False)
        if isinstance(n, int) and n > 0:
            return n
    except Exception:
        pass

    system = platform.system().lower()
    if system == "linux":
        try:
            out = subprocess.check_output(["lscpu", "-p=Core,Socket"], text=True, stderr=subprocess.DEVNULL)
            pairs = set()
            for line in out.splitlines():
                if not line or line.startswith("#"):
                    continue
                parts = line.split(",")
                if len(parts) >= 2:
                    pairs.add((parts[0].strip(), parts[1].strip()))
            if pairs:
                return len(pairs)
        except Exception:
            pass
    elif system == "darwin":
        try:
            out = subprocess.check_output(["sysctl", "-n", "hw.physicalcpu"], text=True, stderr=subprocess.DEVNULL)
            n = int(out.strip())
            if n > 0:
                return n
        except Exception:
            pass

    logical = os.cpu_count() or 1
    return max(1, logical)


def resolve_threads(value: str, reserve_cores: int) -> int:
    if value == "auto":
        physical = detect_physical_cores()
        return max(1, physical - max(0, reserve_cores))
    n = int(value)
    if n < 1:
        raise ValueError("--threads must be auto or a positive integer")
    return n


def shell_join(cmd: List[str]) -> str:
    return " ".join(shlex.quote(str(x)) for x in cmd)


def bool_int(value: Any) -> str:
    return "1" if str(value) not in {"0", "false", "False", "off", "no"} else "0"


def option(cmd: List[str], name: str, value: Optional[Any]) -> None:
    if value is None:
        return
    cmd.extend([f"--{name.replace('_', '-')}", str(value)])


def collect_instances(instances_dir: Path, globs: List[str]) -> List[Path]:
    files: List[Path] = []
    for pattern in globs:
        files.extend(instances_dir.glob(pattern))
    unique = sorted({p.resolve() for p in files if p.is_file()})
    return unique


def extract_json_object(stdout: str) -> Dict[str, Any]:
    text = stdout.strip()
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass
    # Last resort: find last JSON object in mixed stdout.
    matches = list(re.finditer(r"\{[\s\S]*?\}", text))
    for m in reversed(matches):
        try:
            return json.loads(m.group(0))
        except json.JSONDecodeError:
            continue
    # Parse key-value text output if --json was not used.
    result: Dict[str, Any] = {}
    for line in text.splitlines():
        parts = line.strip().split(maxsplit=1)
        if len(parts) == 2:
            key, value = parts
            if key.startswith("best_sequence"):
                continue
            result[key] = coerce_value(value)
    return result


def coerce_value(value: str) -> Any:
    if value in {"true", "false"}:
        return value == "true"
    try:
        if any(c in value for c in [".", "e", "E"]):
            return float(value)
        return int(value)
    except ValueError:
        return value


def run_command(cmd: List[str], stdout_file: Path, stderr_file: Path, dry_run: bool, timeout: Optional[float] = None) -> Tuple[int, str, str, float]:
    t0 = time.time()
    if dry_run:
        line = shell_join(cmd)
        stdout_file.write_text(line + "\n", encoding="utf-8")
        stderr_file.write_text("", encoding="utf-8")
        return 0, line + "\n", "", 0.0
    proc = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    elapsed = time.time() - t0
    stdout_file.write_text(proc.stdout, encoding="utf-8")
    stderr_file.write_text(proc.stderr, encoding="utf-8")
    return proc.returncode, proc.stdout, proc.stderr, elapsed


def build_solver_cmd(args: argparse.Namespace, instance: Path, warm_file: Optional[Path], stack_profile_file: Optional[Path], threads: int, preset: Preset) -> List[str]:
    p = dict(preset.solver_args)

    # CLI overrides.
    for attr in [
        "refine_incumbent", "refine_moves", "refine_max_evals", "refine_max_seconds", "refine_seed", "refine_mmosp_ties", "refine_output",
        "auto_lower_mode", "residual_lb", "residual_core_max_vertices", "partial_state_eval", "partial_state_verify", "witness_evaluator", "witness_decode", "verify_customer_witness", "profile_evaluator", "sequence_evaluator", "symmetry_break", "anchor_pattern",
        "branching", "closure_threads", "closure_parallel_depth", "closure_frontier_cap", "relaxation", "relax_merge_policy", "relax_unmerge_policy", "relax_max_merges", "relax_max_time_share", "relax_proof_threads", "relax_proof_depth", "relax_proof_frontier_cap", "canonical_closures", "warm_start_policy",
        "hash", "sort_moves", "group_moves",
    ]:
        v = getattr(args, attr)
        if v is not None:
            p[attr] = v

    cmd: List[str] = [str(args.solver), "--input", str(instance), "--input-format", args.input_format, "--presence", args.presence]
    option(cmd, "auto-lower-mode", p.get("auto_lower_mode"))
    option(cmd, "residual-lb", p.get("residual_lb"))
    option(cmd, "residual-core-max-vertices", p.get("residual_core_max_vertices"))
    option(cmd, "partial-state-eval", p.get("partial_state_eval"))
    option(cmd, "partial-state-verify", p.get("partial_state_verify"))
    option(cmd, "witness-evaluator", p.get("witness_evaluator"))
    option(cmd, "witness-decode", p.get("witness_decode"))
    option(cmd, "verify-customer-witness", p.get("verify_customer_witness"))
    option(cmd, "profile-evaluator", p.get("profile_evaluator"))
    option(cmd, "refine-incumbent", p.get("refine_incumbent"))
    option(cmd, "refine-moves", p.get("refine_moves"))
    option(cmd, "refine-max-evals", p.get("refine_max_evals"))
    option(cmd, "refine-max-seconds", p.get("refine_max_seconds"))
    option(cmd, "refine-seed", p.get("refine_seed"))
    option(cmd, "refine-mmosp-ties", p.get("refine_mmosp_ties"))
    option(cmd, "refine-output", str(args.output_dir / "refined_starts" / (instance.stem + ".warm")) if p.get("refine_output") and warm_file is not None else None)
    option(cmd, "sequence-evaluator", p.get("sequence_evaluator"))
    option(cmd, "symmetry-break", p.get("symmetry_break"))
    option(cmd, "anchor-pattern", p.get("anchor_pattern"))
    option(cmd, "branching", p.get("branching"))
    option(cmd, "closure-threads", p.get("closure_threads"))
    option(cmd, "closure-parallel-depth", p.get("closure_parallel_depth"))
    option(cmd, "closure-frontier-cap", p.get("closure_frontier_cap"))
    option(cmd, "relaxation", p.get("relaxation"))
    option(cmd, "relax-merge-policy", p.get("relax_merge_policy"))
    option(cmd, "relax-unmerge-policy", p.get("relax_unmerge_policy"))
    option(cmd, "relax-max-merges", p.get("relax_max_merges"))
    option(cmd, "relax-max-time-share", p.get("relax_max_time_share"))
    option(cmd, "relax-proof-threads", p.get("relax_proof_threads"))
    option(cmd, "relax-proof-depth", p.get("relax_proof_depth"))
    option(cmd, "relax-proof-frontier-cap", p.get("relax_proof_frontier_cap"))
    option(cmd, "canonical-closures", p.get("canonical_closures"))
    option(cmd, "hash", p.get("hash"))
    option(cmd, "sort-moves", p.get("sort_moves"))
    option(cmd, "group-moves", p.get("group_moves"))
    option(cmd, "threads", threads)
    option(cmd, "parallel-depth", args.parallel_depth)
    option(cmd, "time-limit", args.time_limit)
    option(cmd, "node-limit", args.node_limit)
    option(cmd, "profile", args.profile)
    if args.progress_interval > 0:
        option(cmd,"progress-interval",args.progress_interval)
        option(cmd,"progress-file",args.output_dir / "progress" / f"{instance.stem}.csv")
    if warm_file is not None:
        option(cmd, "warm-start-file", warm_file)
        option(cmd, "warm-start-policy", p.get("warm_start_policy", "value"))
    if stack_profile_file is not None:
        option(cmd, "emit-stack-profile", stack_profile_file)
    cmd.append("--json")
    if args.quiet:
        cmd.append("--quiet")
    if args.extra_solver_arg:
        cmd.extend(args.extra_solver_arg)
    return cmd


def build_mcnh_cmd(args: argparse.Namespace, instance: Path, warm_file: Path, preset: Preset) -> List[str]:
    preprocess = args.mcnh_preprocess if args.mcnh_preprocess is not None else preset.mcnh_preprocess
    cmd: List[str] = [
        str(args.mcnh), "--input", str(instance),
        "--input-format", args.input_format,
        "--presence", args.presence,
        "--preprocess", str(preprocess),
        "--objective-orientation", "customer",
        "--warm-start-output", str(warm_file),
    ]
    if args.extra_mcnh_arg:
        cmd.extend(args.extra_mcnh_arg)
    return cmd


def validate_args(args: argparse.Namespace, preset: Preset) -> None:
    use_mcnh = args.use_mcnh if args.use_mcnh is not None else preset.use_mcnh
    warm_policy = args.warm_start_policy or preset.solver_args.get("warm_start_policy")
    sym = args.symmetry_break or preset.solver_args.get("symmetry_break", "off")
    anchor = args.anchor_pattern or preset.solver_args.get("anchor_pattern", "auto")

    if args.closure_threads is not None and args.closure_threads < 0:
        raise SystemExit("--closure-threads must be >= 0 (0 inherits --threads).")
    if args.closure_parallel_depth is not None and args.closure_parallel_depth < -1:
        raise SystemExit("--closure-parallel-depth must be >= -1 (-1 inherits --parallel-depth).")
    if args.closure_frontier_cap is not None and args.closure_frontier_cap < 1:
        raise SystemExit("--closure-frontier-cap must be >= 1.")
    if (args.refine_incumbent == 1 or args.refine_output is not None) and not use_mcnh:
        raise SystemExit("Conflict: --refine-incumbent/--refine-output requires --use-mcnh 1.")
    if args.refine_output and args.refine_incumbent != 1:
        raise SystemExit("Conflict: --refine-output requires --refine-incumbent 1.")
    if args.mcnh_preprocess is not None and not use_mcnh:
        raise SystemExit("Conflict: --mcnh-preprocess is meaningful only with --use-mcnh 1.")
    if warm_policy in {"sequence", "strict"} and not use_mcnh:
        raise SystemExit("Conflict: --warm-start-policy sequence|strict requires --use-mcnh 1 in this batch script.")
    if sym == "off" and anchor != "auto":
        raise SystemExit("Conflict: --anchor-pattern ID is meaningful only with --symmetry-break reverse-anchor.")
    if args.input_format not in {"cs", "scoop"}:
        raise SystemExit("Invalid --input-format. Use cs or scoop.")
    if args.presence not in {"legacy-one", "positive"}:
        raise SystemExit("Invalid --presence. Use legacy-one or positive.")
    if args.branching is not None and args.branching not in {"customer", "close-stack", "auto"}:
        raise SystemExit("Invalid --branching.")
    if args.relaxation is not None and args.relaxation not in {"off", "root-merge-probe", "full"}:
        raise SystemExit("Invalid --relaxation.")


def write_run_manifest(out_dir: Path, args: argparse.Namespace, preset: Preset, threads: int, instances: List[Path]) -> None:
    manifest = {
        "preset": preset.name,
        "preset_description": preset.description,
        "threads_resolved": threads,
        "physical_cores_detected": detect_physical_cores(),
        "reserve_cores": args.reserve_cores,
        "instances_dir": str(args.instances_dir),
        "n_instances": len(instances),
        "instances": [str(p) for p in instances],
        "input_format": args.input_format,
        "presence": args.presence,
        "time_limit": args.time_limit,
        "parallel_depth": args.parallel_depth,
        "use_mcnh": args.use_mcnh if args.use_mcnh is not None else preset.use_mcnh,
        "mcnh_preprocess": args.mcnh_preprocess if args.mcnh_preprocess is not None else preset.mcnh_preprocess,
        "residual_lb_mode": args.residual_lb if args.residual_lb is not None else preset.solver_args.get("residual_lb", "off"),
        "residual_core_max_vertices": args.residual_core_max_vertices if args.residual_core_max_vertices is not None else preset.solver_args.get("residual_core_max_vertices", 192),
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Sequential batch runner for csbb_modern.")
    parser.add_argument("--instances-dir", type=Path, default=Path("instances"))
    parser.add_argument("--file-glob", action="append", default=None, help="Pattern relative to instances-dir; can be repeated.")
    parser.add_argument("--output-dir", type=Path, default=Path("results/batch"))
    parser.add_argument("--solver", type=Path, default=Path("build/csbb_modern"))
    parser.add_argument("--mcnh", type=Path, default=Path("build/mcnh_modern"))
    parser.add_argument("--preset", choices=sorted(PRESETS), default="low-risk")
    parser.add_argument("--list-presets", action="store_true")
    parser.add_argument("--describe-parameters", action="store_true")
    parser.add_argument("--input-format", choices=["cs", "scoop"], default="cs")
    parser.add_argument("--presence", choices=["legacy-one", "positive"], default="legacy-one")
    parser.add_argument("--threads", default="auto")
    parser.add_argument("--reserve-cores", type=int, default=1)
    parser.add_argument("--parallel-depth", type=int, default=1)
    parser.add_argument("--time-limit", type=float, default=3600.0)
    parser.add_argument("--node-limit", default=None)
    parser.add_argument("--use-mcnh", type=int, choices=[0, 1], default=None)
    parser.add_argument("--mcnh-preprocess", type=int, choices=[0, 1], default=None)
    parser.add_argument("--profile", type=int, choices=[0, 1], default=1)
    parser.add_argument("--progress-interval",type=float,default=30.0,help="Periodic UB/LB/gap and nodes by phase, seconds; 0 disables")
    parser.add_argument("--emit-stack-profile", type=int, choices=[0, 1], default=0)
    parser.add_argument("--resume", type=int, choices=[0, 1], default=1)
    parser.add_argument("--quiet", action="store_true", default=True)
    parser.add_argument("--dry-run", action="store_true")

    # Optional overrides to preset values.
    parser.add_argument("--residual-lb", choices=["off", "next", "core"], default=None)
    parser.add_argument("--residual-core-max-vertices", type=int, default=None)
    parser.add_argument("--partial-state-eval", choices=["off", "counts"], default=None)
    parser.add_argument("--partial-state-verify", type=int, choices=[0,1], default=None)
    parser.add_argument("--auto-lower-mode", choices=["off", "legacy", "model", "graph"], default=None)
    parser.add_argument("--witness-evaluator", choices=["legacy", "standalone", "dual"], default=None)
    parser.add_argument("--witness-decode", choices=["legacy", "sparse", "dual"], default=None)
    parser.add_argument("--verify-customer-witness", type=int, choices=[0,1], default=None)
    parser.add_argument("--profile-evaluator", type=int, choices=[0,1], default=None)
    parser.add_argument("--refine-incumbent", type=int, choices=[0,1], default=None)
    parser.add_argument("--refine-moves", choices=["insert","swap","reverse","mixed"],default=None)
    parser.add_argument("--refine-max-evals",type=int,default=None)
    parser.add_argument("--refine-max-seconds",type=float,default=None)
    parser.add_argument("--refine-seed",type=int,default=None)
    parser.add_argument("--refine-mmosp-ties",type=int,choices=[0,1],default=None)
    parser.add_argument("--refine-output",type=int, choices=[0,1],default=None)
    parser.add_argument("--sequence-evaluator", choices=["native", "faggioli", "both"], default=None)
    parser.add_argument("--symmetry-break", choices=["off", "reverse-anchor"], default=None)
    parser.add_argument("--anchor-pattern", default=None)
    parser.add_argument("--branching", choices=["customer", "close-stack", "auto"], default=None)
    parser.add_argument("--closure-threads", type=int, default=None)
    parser.add_argument("--closure-parallel-depth", type=int, default=None)
    parser.add_argument("--closure-frontier-cap", type=int, default=None)
    parser.add_argument("--relaxation", choices=["off", "root-merge-probe", "full"], default=None)
    parser.add_argument("--relax-merge-policy", choices=["original"], default=None)
    parser.add_argument("--relax-unmerge-policy", choices=["original", "lifo"], default=None)
    parser.add_argument("--relax-max-merges", type=int, default=None)
    parser.add_argument("--relax-max-time-share", type=float, default=None)
    parser.add_argument("--relax-proof-threads", type=int, default=None)
    parser.add_argument("--relax-proof-depth", type=int, default=None)
    parser.add_argument("--relax-proof-frontier-cap", type=int, default=None)
    parser.add_argument("--canonical-closures", type=int, choices=[0, 1], default=None)
    parser.add_argument("--warm-start-policy", choices=["value", "sequence", "strict"], default=None)
    parser.add_argument("--hash", type=int, choices=[0, 1], default=None)
    parser.add_argument("--sort-moves", type=int, choices=[0, 1], default=None)
    parser.add_argument("--group-moves", type=int, choices=[0, 1], default=None)
    parser.add_argument("--extra-solver-arg", action="append", default=[])
    parser.add_argument("--extra-mcnh-arg", action="append", default=[])

    args = parser.parse_args(argv)

    if args.list_presets:
        for name, preset in PRESETS.items():
            print(f"{name}\n  {preset.description}\n  use_mcnh={int(preset.use_mcnh)} mcnh_preprocess={preset.mcnh_preprocess}\n  solver_args={preset.solver_args}\n")
        return 0
    if args.describe_parameters:
        print(PARAMETER_HELP.strip())
        return 0

    # pathlib normalizes "./binary" into "binary"; subprocess would then
    # search PATH instead of executing the executable in the working directory.
    # Resolve both tools explicitly before building per-instance commands.
    args.solver = args.solver.resolve()
    args.mcnh = args.mcnh.resolve()
    preset = PRESETS[args.preset]
    validate_args(args, preset)
    threads = resolve_threads(str(args.threads), args.reserve_cores)

    globs = args.file_glob or ["*.txt", "*.dat"]
    instances = collect_instances(args.instances_dir, globs)
    if not instances:
        raise SystemExit(f"No instance files found in {args.instances_dir} with patterns {globs}")

    out_dir: Path = args.output_dir
    logs_dir = out_dir / "logs"
    warm_dir = out_dir / "warm_starts"
    json_dir = out_dir / "json"
    profiles_dir = out_dir / "profiles"
    progress_dir = out_dir / "progress"
    refine_dir = out_dir / "refined_starts"
    for d in [out_dir, logs_dir, warm_dir, json_dir, profiles_dir, progress_dir, refine_dir]:
        d.mkdir(parents=True, exist_ok=True)

    write_run_manifest(out_dir, args, preset, threads, instances)

    use_mcnh = args.use_mcnh if args.use_mcnh is not None else preset.use_mcnh
    results_csv = out_dir / "results.csv"
    results_jsonl = out_dir / "results.jsonl"
    fieldnames = [
        "instance", "preset", "input_format", "presence", "return_code", "mcnh_return_code",
        "status", "upper_bound", "lower_bound", "gap_percent", "time_seconds", "elapsed_seconds",
        "nodes", "nodes_per_second", "nodes_customer", "nodes_relaxed_proofs", "nodes_close_stack", "nodes_frontier",
        "cpu_seconds", "peak_rss_mb", "progress_file", "hash_hits", "hash_entries", "dominance_keys", "frontier_states",
        "warm_start_value", "warm_start_sequence_value", "warm_start_faggioli_value",
        "warm_start_evaluators_agree", "warm_start_accepted", "warm_start_sequence_accepted",
        "warm_start_verified", "warm_start_validation_status", "warm_start_declared_value",
        "warm_start_verified_value", "warm_start_sequence_domain",
        "refine_enabled", "refine_status", "refine_initial_ub", "refine_final_ub", "refine_evaluations", "refine_mosp_improvements", "refine_tie_acceptances", "refine_seconds",
        "witness_evaluator", "witness_decode", "evaluator_bridge_setup_seconds", "evaluator_incidences",
        "evaluator_legacy_calls", "evaluator_legacy_stream_calls", "evaluator_standalone_calls", "evaluator_sparse_stream_calls",
        "witness_decode_legacy_calls", "witness_decode_sparse_calls", "customer_witness_checks",
        "evaluator_legacy_seconds", "evaluator_legacy_stream_seconds", "evaluator_standalone_seconds", "evaluator_sparse_stream_seconds",
        "witness_decode_seconds", "customer_witness_seconds",
        "applied_initial_lower_bound", "legacy_lower_bound", "model_cardinality_lower_bound",
        "graph_min_degree_lower_bound", "graph_degeneracy_lower_bound", "customer_graph_core_lower_bound",
        "residual_lb_mode", "residual_lb_calls", "residual_lb_core_tests", "residual_lb_prunes",
        "partial_state_eval", "partial_cache_builds", "partial_neighbor_updates", "partial_candidate_checks", "partial_force_checks", "profile_partial_build_seconds", "profile_partial_updates_seconds",
        "profile_residual_lb_seconds", "relaxed_proof_parallel_calls", "relaxed_proof_frontier_states", "relaxed_proof_max_workers", "relax_proof_threads", "relax_proof_depth", "relax_proof_frontier_cap", "branching_mode", "relaxation_mode", "threads", "parallel_depth",
        "closure_branch_calls", "closure_branch_nodes", "closure_branch_dead_states",
        "closure_branch_successes", "closure_branch_fallbacks",
        "closure_parallel_calls", "closure_frontier_states", "closure_max_workers",
        "closure_threads", "closure_parallel_depth", "closure_frontier_cap",
        "active_target", "last_completed_target", "command", "mcnh_command",
    ]

    csv_exists = results_csv.exists() and args.resume == 1
    csv_file = results_csv.open("a", newline="", encoding="utf-8")
    writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
    if not csv_exists:
        writer.writeheader()
    jsonl_file = results_jsonl.open("a", encoding="utf-8")

    try:
        for idx, instance in enumerate(instances, start=1):
            stem = instance.stem
            per_json = json_dir / f"{stem}.json"
            if args.resume == 1 and per_json.exists():
                print(f"[{idx}/{len(instances)}] skipping {instance.name} (resume)")
                continue

            print(f"[{idx}/{len(instances)}] running {instance.name} with preset={preset.name}, threads={threads}")
            warm_file: Optional[Path] = None
            mcnh_rc: Optional[int] = None
            mcnh_cmd: List[str] = []
            if use_mcnh:
                warm_file = warm_dir / f"{stem}.warm.txt"
                mcnh_cmd = build_mcnh_cmd(args, instance, warm_file, preset)
                mcnh_rc, _, _, _ = run_command(
                    mcnh_cmd,
                    logs_dir / f"{stem}.mcnh.stdout.txt",
                    logs_dir / f"{stem}.mcnh.stderr.txt",
                    args.dry_run,
                )
                if mcnh_rc != 0:
                    print(f"  warning: MCNh returned {mcnh_rc}; running B&B without warm start for this instance", file=sys.stderr)
                    warm_file = None

            stack_profile_file = profiles_dir / f"{stem}.stack_profile.csv" if args.emit_stack_profile else None
            solver_cmd = build_solver_cmd(args, instance, warm_file, stack_profile_file, threads, preset)
            timeout = None
            if args.time_limit and args.time_limit > 0:
                timeout = float(args.time_limit) + 120.0
            rc, stdout, stderr, wall = run_command(
                solver_cmd,
                logs_dir / f"{stem}.solver.stdout.txt",
                logs_dir / f"{stem}.solver.stderr.txt",
                args.dry_run,
                timeout=timeout,
            )
            result = extract_json_object(stdout)
            result.update({
                "instance": instance.name,
                "instance_path": str(instance),
                "preset": preset.name,
                "input_format": args.input_format,
                "presence": args.presence,
                "return_code": rc,
                "mcnh_return_code": mcnh_rc,
                "command": shell_join(solver_cmd),
                "mcnh_command": shell_join(mcnh_cmd) if mcnh_cmd else "",
                "batch_wall_seconds": wall,
                "progress_file": str(progress_dir / f"{stem}.csv") if args.progress_interval > 0 else "",
            })
            if "elapsed_seconds" in result and "time_seconds" not in result:
                result["time_seconds"] = result["elapsed_seconds"]
            if "relaxation_mode" not in result:
                # The current solver reports counters, not the relaxation mode as a field.
                result["relaxation_mode"] = args.relaxation or preset.solver_args.get("relaxation")
            per_json.write_text(json.dumps(result, indent=2), encoding="utf-8")
            jsonl_file.write(json.dumps(result, ensure_ascii=False) + "\n")
            jsonl_file.flush()
            row = {k: result.get(k, "") for k in fieldnames}
            writer.writerow(row)
            csv_file.flush()
    finally:
        csv_file.close()
        jsonl_file.close()

    print(f"Done. Consolidated outputs written to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
