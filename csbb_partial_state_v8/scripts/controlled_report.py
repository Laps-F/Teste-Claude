#!/usr/bin/env python3
"""Regenerate deterministic, cautious pairwise MOSP B&B comparison report.

External process exit/error, invalid certificate and incomplete experiments are
kept visibly separate from valid results. Speedup is shown only if both sides
prove OPTIMAL, otherwise comparisons are in UB/LB/gap at equal time budgets.
"""
from __future__ import annotations
import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


def num(x):
    return float(x) if isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x) else None


def fmt(x, places=2):
    return "—" if x is None else f"{x:,.{places}f}"


def med(vals):
    values = [x for x in vals if x is not None]
    return statistics.median(values) if values else None


def progress_stats(record):
    path = record.get("progress_csv")
    if not path or not Path(path).is_file():
        return {"gap_auc": None, "progress_samples": 0, "first_lb_improvement_seconds":None}
    try:
        with open(path, newline="", encoding="utf-8") as f:
            samples = list(csv.DictReader(f))
        points = [(float(x["time_seconds"]),float(x["gap_percent"])) for x in samples]
        if not points or any(not (math.isfinite(t) and math.isfinite(g)) for t,g in points):
            return {"gap_auc": None,"progress_samples": 0, "first_lb_improvement_seconds":None}
        if any(t2 < t1 for (t1,_),(t2,_) in zip(points,points[1:])):
            return {"gap_auc":None,"progress_samples":0,"first_lb_improvement_seconds":None}
        if points[-1][0] <= 0: return {"gap_auc":None,"progress_samples":len(points),"first_lb_improvement_seconds":None}
        area = sum((t2-t1)*(g1+g2)*0.5 for (t1,g1),(t2,g2) in zip(points,points[1:]))
        area += points[0][0]*points[0][1]
        first_lb = float(samples[0]["lower_bound"])
        first_improvement = next((float(x["time_seconds"]) for x in samples if float(x["lower_bound"]) > first_lb), None)
        return {"gap_auc":area/points[-1][0],"progress_samples":len(points),
                "first_lb_improvement_seconds":first_improvement}
    except (OSError, ValueError, KeyError):
        return {"gap_auc":None,"progress_samples":0,"first_lb_improvement_seconds":None}


def worker_stats(data):
    out={}
    for phase in ("customer","relaxed","closure"):
        ws=data.get("workers_"+phase) or []
        active=sum((num(w.get("active_seconds")) or 0.0) for w in ws)
        available=sum((num(w.get("available_seconds")) or 0.0) for w in ws)
        nodes=[w.get("nodes",0) for w in ws if w.get("tasks",0)>0]
        out["worker_utilization_"+phase] = min(1.0,active/available) if available>0 else None
        out["worker_imbalance_"+phase] = max(nodes)/(sum(nodes)/len(nodes)) if nodes and sum(nodes)>0 else None
        out["worker_tasks_"+phase] = sum(w.get("tasks",0) for w in ws)
    return out


def score(entries):
    valid = [row for row in entries if not row["error"] and row["result"]]
    for row in valid:
        row["progress_metrics"] = progress_stats(row)
        row["worker_metrics"] = worker_stats(row["result"])
    result = {
        "executed": len(entries), "valid": len(valid), "failures": len(entries) - len(valid),
        "optimal": sum(1 for a in valid if a["result"].get("status") == "OPTIMAL"),
        "timeouts": sum(1 for a in valid if a["result"].get("status") == "TIME_LIMIT"),
        "nodes_limited": sum(1 for a in valid if a["result"].get("status") == "NODE_LIMIT"),
        "ub": med([num(a["result"].get("upper_bound")) for a in valid]),
        "lb": med([num(a["result"].get("lower_bound")) for a in valid]),
        "gap": med([num(a["result"].get("gap_percent")) for a in valid]),
        "time": med([num(a["result"].get("elapsed_seconds")) for a in valid]),
        "wall": med([num(a.get("wall_seconds")) for a in valid]),
        "nodes": med([num(a["result"].get("nodes")) for a in valid]),
        "nodes_per_second": med([num(a["result"].get("nodes_per_second")) for a in valid]),
        "cpu_seconds": med([num(a["result"].get("cpu_seconds")) for a in valid]),
        "peak_rss_mb": med([num(a["result"].get("peak_rss_mb")) for a in valid]),
        "gap_auc": med([num(a["progress_metrics"].get("gap_auc")) for a in valid]),
        "first_lb_improvement_seconds": med([num(a["progress_metrics"].get("first_lb_improvement_seconds")) for a in valid]),
        "nodes_customer": med([num(a["result"].get("nodes_customer")) for a in valid]),
        "nodes_relaxed_proofs": med([num(a["result"].get("nodes_relaxed_proofs")) for a in valid]),
        "nodes_close_stack": med([num(a["result"].get("nodes_close_stack")) for a in valid]),
        "nodes_frontier": med([num(a["result"].get("nodes_frontier")) for a in valid]),
        "worker_utilization_customer": med([num(a["worker_metrics"].get("worker_utilization_customer")) for a in valid]),
        "worker_utilization_relaxed": med([num(a["worker_metrics"].get("worker_utilization_relaxed")) for a in valid]),
        "worker_utilization_closure": med([num(a["worker_metrics"].get("worker_utilization_closure")) for a in valid]),
        "worker_imbalance_customer": med([num(a["worker_metrics"].get("worker_imbalance_customer")) for a in valid]),
        "worker_imbalance_relaxed": med([num(a["worker_metrics"].get("worker_imbalance_relaxed")) for a in valid]),
        "worker_imbalance_closure": med([num(a["worker_metrics"].get("worker_imbalance_closure")) for a in valid]),
        "relaxation_full_seconds": med([num(a["result"].get("relaxation_full_seconds")) for a in valid]),
        "relaxed_proof_seconds": med([num(a["result"].get("relaxed_proof_seconds")) for a in valid]),
        "relaxed_proofs_attempted": med([num(a["result"].get("relaxed_proofs_attempted")) for a in valid]),
        "relaxed_proof_infeasibilities": med([num(a["result"].get("relaxed_proof_infeasibilities")) for a in valid]),
        "hash_hits": med([num(a["result"].get("hash_hits")) for a in valid]),
        "refine_initial_ub": med([num(a["result"].get("refine_initial_ub")) for a in valid if a["result"].get("refine_enabled")]),
        "refine_final_ub": med([num(a["result"].get("refine_final_ub")) for a in valid if a["result"].get("refine_enabled")]),
        "refine_evaluations": med([num(a["result"].get("refine_evaluations")) for a in valid if a["result"].get("refine_enabled")]),
        "refine_mosp_improvements": med([num(a["result"].get("refine_mosp_improvements")) for a in valid if a["result"].get("refine_enabled")]),
        "refine_seconds": med([num(a["result"].get("refine_seconds")) for a in valid if a["result"].get("refine_enabled")]),
        "warm_verified": sum(1 for a in valid if a["result"].get("warm_start_verified") is True),
        "residual_prunes": med([num(a["result"].get("residual_lb_prunes")) for a in valid]),
        "partial_cache_builds": med([num(a["result"].get("partial_cache_builds")) for a in valid]),
        "partial_neighbor_updates": med([num(a["result"].get("partial_neighbor_updates")) for a in valid]),
        "partial_candidate_checks": med([num(a["result"].get("partial_candidate_checks")) for a in valid]),
        "partial_force_checks": med([num(a["result"].get("partial_force_checks")) for a in valid]),
        "relaxed_parallel_calls": med([num(a["result"].get("relaxed_proof_parallel_calls")) for a in valid]),
        "closure_parallel_calls": med([num(a["result"].get("closure_parallel_calls")) for a in valid]),
        "evaluator_standalone_calls": med([num(a["result"].get("evaluator_standalone_calls")) for a in valid]),
        "evaluator_standalone_seconds": med([num(a["result"].get("evaluator_standalone_seconds")) for a in valid]),
        "evaluator_legacy_calls": med([num(a["result"].get("evaluator_legacy_calls")) for a in valid]),
        "evaluator_legacy_seconds": med([num(a["result"].get("evaluator_legacy_seconds")) for a in valid]),
        "witness_decode_seconds": med([num(a["result"].get("witness_decode_seconds")) for a in valid]),
        "customer_witness_checks": med([num(a["result"].get("customer_witness_checks")) for a in valid]),
        "close_stack_fallbacks": med([num(a["result"].get("closure_branch_fallbacks")) for a in valid]),
    }
    result["all_optimal"] = (len(valid) > 0 and len(valid) == len(entries) and result["optimal"] == len(valid))
    result["status"] = (
        "NÃO EXECUTADO" if not entries else
        "ERRO" if not valid else
        "OPTIMAL" if result["all_optimal"] else
        "MISTO" if len({a["result"].get("status") for a in valid}) > 1 or result["failures"] else
        valid[0]["result"].get("status", "UNKNOWN")
    )
    return result


def aggregate(raw):
    result = defaultdict(list)
    for a in raw:
        result[(int(a["dimension"]), a["variant"])].append(a)
    return {k: score(v) for k, v in result.items()}


def compare_runs(a, b, comparison_id=None, num_parallel_workers=1):
    if not a or not b or not a["valid"] or not b["valid"]:
        return {"d_ub":None, "d_lb":None, "d_gap":None, "proof_speedup":None,
                "throughput_gain":None,"work_inflation":None,"parallel_efficiency":None,"d_gap_auc":None,
                "assessment":"SEM DADOS VÁLIDOS"}
    # Control minus treatment: positive delta UB/gap is an improvement.
    d_ub = a["ub"] - b["ub"] if a["ub"] is not None and b["ub"] is not None else None
    d_lb = b["lb"] - a["lb"] if a["lb"] is not None and b["lb"] is not None else None
    d_gap = a["gap"] - b["gap"] if a["gap"] is not None and b["gap"] is not None else None
    speedup = (a["time"] / b["time"] if a["all_optimal"] and b["all_optimal"] and a["time"] and b["time"] and b["time"] > 0 else None)
    if speedup is not None:
        tag = ("GANHO DE TEMPO DE PROVA" if speedup > 1.05 else
               "PERDA DE TEMPO DE PROVA" if speedup < 0.95 else "TEMPO DE PROVA SEMELHANTE")
    elif d_ub is not None and d_lb is not None:
        if d_ub > 0 and d_lb >= 0: tag = "MELHOR UB; SEM PROVA DE SPEEDUP"
        elif d_lb > 0 and d_ub >= 0: tag = "MELHOR LB; SEM PROVA DE SPEEDUP"
        elif d_gap is not None and d_gap > 0: tag = "MENOR GAP; SEM PROVA DE SPEEDUP"
        elif d_ub < 0 or d_lb < 0: tag = "REGRESSÃO EM QUALIDADE/BOUND"
        else: tag = "EMPATE NOS BOUNDS; SPEEDUP NÃO DETERMINADO"
    else: tag = "DADOS INSUFICIENTES"
    if a["failures"] or b["failures"]:
        tag += " [ERROS PRESENTES]"
    throughput_gain = (b["nodes_per_second"] / a["nodes_per_second"]
                       if a.get("nodes_per_second") and b.get("nodes_per_second") else None)
    # Work inflation refers to the same exact proof and SAME search structure,
    # not to changes in branching or relaxation. Do not use time-censored counts.
    same_search = comparison_id in {"customer_parallel", "relaxed_proof_parallel", "close_stack_parallel", "partial_state", "partial_state_parallel", "partial_state_relax"}
    work_inflation = (b["nodes"] / a["nodes"] if same_search and speedup is not None and a.get("nodes") and b.get("nodes") else None)
    efficiency = speedup/num_parallel_workers if speedup is not None and comparison_id == "customer_parallel" and num_parallel_workers>0 else None
    ga = a.get("gap_auc"); gb = b.get("gap_auc")
    d_gap_auc = ga-gb if ga is not None and gb is not None else None
    return {"d_ub":d_ub, "d_lb":d_lb, "d_gap":d_gap, "proof_speedup":speedup,
            "throughput_gain":throughput_gain,"work_inflation":work_inflation,
            "parallel_efficiency":efficiency,"d_gap_auc":d_gap_auc,"assessment":tag}


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for row in rows: writer.writerow(row)


def generate(out: Path):
    out = Path(out)
    manifest = json.loads((out/"manifest.json").read_text(encoding="utf-8"))
    resultsdir = out / "runs"
    records = [json.loads(x.read_text(encoding="utf-8")) for x in sorted(resultsdir.glob("*.json"))]
    by_id = aggregate(records)
    raw = []
    for row in records:
        data = row.get("result") or {}
        flat = {k:row.get(k) for k in ("dimension", "instance", "repetition", "variant", "warm_mode", "wall_seconds", "return_code", "error")}
        flat.update({k:data.get(k) for k in ("cpu_seconds","peak_rss_mb","nodes_per_second", "nodes_customer",
            "nodes_relaxed_proofs","nodes_close_stack","nodes_frontier","status", "upper_bound", "lower_bound", "gap_percent", "elapsed_seconds", "nodes", "warm_start_verified", "warm_start_verified_value", "relaxation_mode", "branching_mode", "residual_lb_prunes", "partial_state_eval", "partial_cache_builds", "partial_neighbor_updates", "partial_candidate_checks", "partial_force_checks", "profile_partial_build_seconds", "profile_partial_updates_seconds", "relaxed_proof_parallel_calls", "closure_parallel_calls", "closure_branch_fallbacks", "relaxation_full_seconds", "relaxed_proof_seconds", "witness_evaluator", "witness_decode", "evaluator_bridge_setup_seconds", "evaluator_incidences", "evaluator_legacy_calls", "evaluator_legacy_stream_calls", "evaluator_standalone_calls", "evaluator_sparse_stream_calls", "witness_decode_legacy_calls", "witness_decode_sparse_calls", "customer_witness_checks", "evaluator_legacy_seconds", "evaluator_legacy_stream_seconds", "evaluator_standalone_seconds", "evaluator_sparse_stream_seconds", "witness_decode_seconds", "customer_witness_seconds", "refine_enabled", "refine_status", "refine_initial_ub", "refine_final_ub", "refine_evaluations", "refine_mosp_improvements", "refine_tie_acceptances", "refine_seconds")})
        pm=progress_stats(row)
        flat.update(pm)
        flat.update(worker_stats(data))
        flat["progress_csv"]=row.get("progress_csv")
        raw.append(flat)
    report_dir = out / "reports"
    report_dir.mkdir(parents=True, exist_ok=True)
    write_csv(report_dir/"runs.csv", list(raw[0]) if raw else ["dimension", "instance", "variant", "status", "upper_bound", "lower_bound", "gap_percent", "elapsed_seconds", "error"], raw)
    comparisons = []
    for comp in manifest["comparisons"]:
        for dim in [400,600,800,1000]:
            ctrl = by_id.get((dim, comp["control"]))
            test = by_id.get((dim, comp["treatment"]))
            metric = compare_runs(ctrl,test,comp["key"],manifest.get("threads",1))
            comparisons.append({
                "dimension":dim, "comparison":comp["key"], "label":comp["label"], "control":comp["control"], "treatment":comp["treatment"],
                "control_status":ctrl["status"] if ctrl else "NÃO EXECUTADO", "test_status":test["status"] if test else "NÃO EXECUTADO",
                "control_n":ctrl["valid"] if ctrl else 0, "treatment_n":test["valid"] if test else 0,
                "control_ub":ctrl["ub"] if ctrl else None, "treatment_ub":test["ub"] if test else None,
                "control_lb":ctrl["lb"] if ctrl else None, "treatment_lb":test["lb"] if test else None,
                "control_gap":ctrl["gap"] if ctrl else None, "treatment_gap":test["gap"] if test else None,
                "control_time":ctrl["time"] if ctrl else None, "treatment_time":test["time"] if test else None,
                "control_nodes_per_second":ctrl["nodes_per_second"] if ctrl else None,
                "treatment_nodes_per_second":test["nodes_per_second"] if test else None,
                "control_cpu_seconds":ctrl["cpu_seconds"] if ctrl else None,
                "treatment_cpu_seconds":test["cpu_seconds"] if test else None,
                "control_gap_auc":ctrl["gap_auc"] if ctrl else None,
                "treatment_gap_auc":test["gap_auc"] if test else None,
                **metric,
            })
    write_csv(report_dir/"comparisons.csv", list(comparisons[0]) if comparisons else ["dimension","comparison","assessment"], comparisons)
    summaries = []
    for (dim,k),s in sorted(by_id.items()):
        summaries.append({"dimension":dim, "variant":k, **{key:value for key,value in s.items() if key != "all_optimal"}})
    write_csv(report_dir/"by_configuration.csv", list(summaries[0]) if summaries else ["dimension", "variant", "status"], summaries)
    # Flatten every progress curve for plot-friendly import into Excel/R/Python.
    progress_rows=[]
    for row in records:
        f=row.get("progress_csv")
        if not f or not Path(f).is_file(): continue
        try:
            with open(f,newline="",encoding="utf-8") as stream:
                for point in csv.DictReader(stream):
                    progress_rows.append({"dimension":row["dimension"],"variant":row["variant"],
                       "repetition":row["repetition"],**point})
        except (OSError,ValueError): pass
    write_csv(report_dir/"progress_samples.csv",list(progress_rows[0]) if progress_rows else ["dimension","variant","repetition","time_seconds","gap_percent"],progress_rows)
    missing = 4*manifest["repeats"]*len(manifest["variants"]) - len(records)
    failed = [r for r in records if r["error"]]
    lines = [
        "# Relatório automático de comparação controlada — MOSP / Chu–Stuckey",
        "",
        f"Gerado (UTC): {datetime.now(timezone.utc).isoformat(timespec='seconds')}.",
        f"**Campanha:** {len(records)} execuções registradas; {missing} ainda não realizadas; {len(failed)} inválidas/falhas.",
        f"**Protocolo:** uma instância real por dimensão, quatro dimensões (400, 600, 800, 1000), {manifest['repeats']} repetição(ões), {manifest['time_limit_seconds']} s de B&B por execução, {manifest['threads']} trabalhadores para modos paralelos.",
        "**Controles:** cada par modifica somente o fator descrito na matriz experimental; mesmos arquivos, binário, limite de tempo, e mesmo warm start certificado para todas as condições que exigem MCNh. Execuções são SEQUENCIAIS. Custos de pré-cálculo do MCNh são contabilizados separadamente.",
        "",
        "## Interpretação obrigatória",
        "",
        "- `ΔUB = UB(controle) − UB(teste)` (positivo favorece teste); `ΔLB = LB(teste) − LB(controle)` (positivo favorece teste); `Δgap = gap(controle) − gap(teste)` (positivo favorece teste).",
        "- Speedup de **prova de otimalidade** = tempo do controle ÷ tempo do teste, calculado **somente quando ambas as variantes certificaram OPTIMAL em todas as repetições válidas**. TIME_LIMIT não prova speedup.",
        "- A amostra contém apenas uma instância por dimensão e, com uma repetição, não permite inferência estatística sobre a família de instâncias. Nenhum resultado abaixo representa desempenho comprovado para 610 instâncias.",
        "- Throughput é nós totais/segundo de parede, inclui as fases customer, relaxada, closure e fronteira; comparação por fase está no `runs.csv`. Não equivale a velocidade de prova.",
        "- Work inflation somente é calculado se ambos concluem OPTIMAL e a ablação mantém a estrutura de busca; contagens em TIME_LIMIT não são equivalentes a trabalho total.",
        "- AUC-gap usa interpolação trapezoidal dos registros periódicos, normalizada pelo último instante; menor é melhor. Cadência e duração afetam comparabilidade.",
        "- Utilização de worker é estimada como tempo em tarefas / tempo de janela de despacho; tempos inativos incluem inicialização/sincronização e não são medidas instrumentadas de bloqueio.",
        "- Os slots de worker são reutilizados em fases sucessivas e não identificam threads físicas persistentes.",
        "- A busca `close-stack` pode recorrer ao `customer` após uma falha; seu resultado mede o custo total da política com fallback, não uma prova exclusiva do método close-stack.",
        "- `full` possui fase própria de relaxação e fallback; os perfis de fases devem ser analisados conjuntamente. A confirmação de um lower bound certificado permanece responsabilidade do solver.",
        "",
        "## Situação dos experimentos por dimensão",
        "",
        "| Dimensão | Execuções válidas / previstas | OPTIMAL | TIME_LIMIT | Erros |",
        "|---:|---:|---:|---:|---:|",
    ]
    for dim in [400,600,800,1000]:
        total = sum(1 for x in records if x["dimension"] == dim)
        valid = [x for x in records if x["dimension"] == dim and not x["error"]]
        opt = sum(x["result"].get("status") == "OPTIMAL" for x in valid)
        timeout = sum(x["result"].get("status") == "TIME_LIMIT" for x in valid)
        errs = total - len(valid)
        lines.append(f"| {dim}×{dim} | {len(valid)}/{manifest['repeats']*len(manifest['variants'])} | {opt} | {timeout} | {errs} |")
    lines += ["", "## Comparação pareada por melhoria", ""]
    for comp in manifest["comparisons"]:
        lines.extend([f"### {comp['label']} (`{comp['key']}`)", "", f"Controle: `{comp['control']}`; teste: `{comp['treatment']}`. {comp['rationale']}.", "", 
                      "| Dim. | Status C/T | UB C→T | LB C→T | Gap C→T (%) | ΔUB | ΔLB | Δgap (p.p.) | Speedup prova | Interpretação |",
                      "|---:|---|---|---|---|---:|---:|---:|---:|---|"])
        for p in comparisons:
            if p["comparison"] != comp["key"]: continue
            def both(k1,k2):
                return f"{fmt(p[k1])}→{fmt(p[k2])}"
            lines.append(f"| {p['dimension']} | {p['control_status']}/{p['test_status']} | {both('control_ub','treatment_ub')} | {both('control_lb','treatment_lb')} | {both('control_gap','treatment_gap')} | {fmt(p['d_ub'])} | {fmt(p['d_lb'])} | {fmt(p['d_gap'])} | {fmt(p['proof_speedup']) + '×' if p['proof_speedup'] is not None else '—'} | {p['assessment']} |")
        lines.append("")
    lines += ["## Eficiência computacional e paralelismo", "",
              "Throughput em milhares de nós/s. Aceleração de throughput é apresentada mesmo sob TIME_LIMIT, mas NÃO demonstra speedup de prova. Work inflation somente em provas pareadas completas comparáveis.", "",
              "| Dim. | Comparação | Nós/s C→T | Ganho throughput | Work inflation | Eficiência paralela | ΔAUC gap (p.p.) |",
              "|---:|---|---|---:|---:|---:|---:|"]
    for p in comparisons:
        def per_k(x): return None if x is None else x/1000.0
        c,t=per_k(p["control_nodes_per_second"]),per_k(p["treatment_nodes_per_second"])
        lines.append(f"| {p['dimension']} | `{p['comparison']}` | {fmt(c)}→{fmt(t)} | {fmt(p['throughput_gain'])} | {fmt(p['work_inflation'])} | {fmt(p['parallel_efficiency'])} | {fmt(p['d_gap_auc'])} |")
    lines += ["", "### Consumo de recursos e distribuição de trabalho", "",
              "As taxas e utilizações são medianas por execução. `idle` inclui overhead estimado de despacho e não representa necessariamente CPU ociosa.", "",
              "| Dim. | Configuração | Nós/s | CPU (s) | RSS pico (MB) | AUC gap (%) | 1ª melhora LB (s) | Utilização customer | Utilização relaxada | Utilização closure |",
              "|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in summaries:
        lines.append(f"| {row['dimension']} | `{row['variant']}` | {fmt(row.get('nodes_per_second'),0)} | {fmt(row.get('cpu_seconds'))} | {fmt(row.get('peak_rss_mb'))} | {fmt(row.get('gap_auc'))} | {fmt(row.get('first_lb_improvement_seconds'))} | {fmt(row.get('worker_utilization_customer'),3)} | {fmt(row.get('worker_utilization_relaxed'),3)} | {fmt(row.get('worker_utilization_closure'),3)} |")
    lines += ["", "**Séries temporais:** `reports/progress_samples.csv` reúne snapshots com UB/LB/gap, nós por fase e provas relaxadas concluídas, identificados por dimensão, variante e repetição.", ""]
    lines += ["## Diagnóstico das melhorias ativadas", "", "Valores abaixo são medianas por execução. Zero pode indicar que o mecanismo foi habilitado mas não produziu atividade mensurável.", "",
              "| Dim. | Configuração | LB prunes | Provas relaxadas paralelas | Close-stack paralelo | Fallback close-stack | Tempo full (s) | Tempo prova relaxada (s) |",
              "|---:|---|---:|---:|---:|---:|---:|---:|"]
    for row in summaries:
        lines.append(f"| {row['dimension']} | `{row['variant']}` | {fmt(row.get('residual_prunes'), 0)} | {fmt(row.get('relaxed_parallel_calls'), 0)} | {fmt(row.get('closure_parallel_calls'), 0)} | {fmt(row.get('close_stack_fallbacks'), 0)} | {fmt(row.get('relaxation_full_seconds'), 3)} | {fmt(row.get('relaxed_proof_seconds'), 3)} |")
    if any(x.get("refine_initial_ub") is not None for x in summaries):
        lines += ["", "## Refinamento incremental de incumbentes (MCNh)", "",
                  "UB de entrada e UB após refinamento são certificados na sequência de produtos. O tempo de busca local integra o orçamento global do B&B.", "",
                  "| Dim. | Variante | UB MCNh | UB refinado | Avaliações incrementais | Melhorias MOSP | Tempo local (s) |",
                  "|---:|---|---:|---:|---:|---:|---:|"]
        for row in summaries:
            if row.get("refine_initial_ub") is None: continue
            lines.append(f"| {row['dimension']} | `{row['variant']}` | {fmt(row.get('refine_initial_ub'),0)} | {fmt(row.get('refine_final_ub'),0)} | {fmt(row.get('refine_evaluations'),0)} | {fmt(row.get('refine_mosp_improvements'),0)} | {fmt(row.get('refine_seconds'),4)} |")
    prepfile = out / "warm_preparation.json"
    if prepfile.exists():
        preps = json.loads(prepfile.read_text(encoding="utf-8"))
        lines += ["", "## Custo de preparação dos warm starts MCNh", "", "Computado uma vez por instância e modo de preprocessing; não incluído no limite de tempo do B&B.", "",
                  "| Dimensão / MCNh preprocessing | Tempo (s) | SHA-256 do certificado |",
                  "|---|---:|---|"]
        for key, meta in sorted(preps.items()):
            lines.append(f"| {key} | {fmt(meta.get('elapsed_seconds'), 4)} | `{str(meta.get('sha256', '—'))[:16]}` |")
    lines += ["", "## Rastreabilidade e replicação", "", f"Assinatura da campanha: `{manifest['signature']}`.", "",
              "- `manifest.json`: hash SHA-256 do solver, MCNh, dos quatro arquivos e especificação de cada fator.",
              "- `warm_preparation.json`: tempo/código/hash de cada MCNh; certificado reutilizado nas execuções comparáveis.",
              "- `runs/*.json`: metadados, comando, código de retorno e resposta JSON íntegra do solver.",
              "- `logs/*.out` e `logs/*.err`: saídas individuais para auditoria.",
              "- `reports/runs.csv`: dados primários tabulados; `by_configuration.csv`: sumários; `comparisons.csv`: deltas por par.",
              "", "## Limites da conclusão", "", 
              "Não são declarados ganhos algorítmicos sem observações pareadas e válidas. Se todas as execuções de um par terminarem por limite de tempo, avaliar UB/LB/gap e custos de profiling, sem classificar uma variante como mais rápida para *provar* otimalidade.", ""]
    if failed:
        lines += ["## Falhas e saídas rejeitadas", "", "Execuções abaixo não participam de cálculos de ganho:", ""]
        for row in failed[:50]: lines.append(f"- {row['dimension']} `{row['variant']}` repetição {row['repetition']}: {row['error']}")
    (report_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")
    return {"complete":missing == 0, "records":len(records), "failed":len(failed), "report":str(report_dir/"report.md")}

if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, required=True)
    a = p.parse_args()
    print(json.dumps(generate(a.output_dir), indent=2))
