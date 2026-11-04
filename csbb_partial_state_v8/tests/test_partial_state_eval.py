#!/usr/bin/env python3
"""Regression of reversible partial customer-state counts against the exact baseline.

Checks: objective, proof, node-equivalence for deterministic serial cases,
parallel proof correctness, optional per-node invariant verifier, interruption.
"""
import json, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOLVER=ROOT/'csbb_modern'
BASE=ROOT/'csbb_baseline'
SAMPLES=sorted((ROOT/'tests/generated').glob('regression*.cs'))
assert len(SAMPLES)>=70
checks=0
with_counts_work=0

def run(path,mode,extra,timeout=12):
    global checks
    args=[str(SOLVER),'--input',str(path),'--input-format','cs','--presence','legacy-one',
          '--partial-state-eval',mode,'--partial-state-verify','1' if mode=='counts' else '0',
          '--time-limit','6','--json','--quiet']+extra
    p=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=timeout)
    if p.returncode!=0:
        raise AssertionError(f'{path.name} mode={mode} args={extra}: {p.stderr[-900:]}')
    j=json.loads(p.stdout)
    checks+=1
    return j

# Baseline equivalence when no branch choices depend on cache.
for path in SAMPLES:
    extra=['--threads','1','--relaxation','off','--residual-lb','next','--auto-lower-mode','graph']
    a=run(path,'off',extra);b=run(path,'counts',extra)
    assert a['status']=='OPTIMAL' and b['status']=='OPTIMAL',(path.name,a['status'],b['status'])
    assert a['upper_bound']==b['upper_bound']==a['lower_bound']==b['lower_bound'], path.name
    assert a['nodes']==b['nodes'], ('tree diverged',path.name,a['nodes'],b['nodes'])
    if b['partial_cache_builds']>0:
        with_counts_work+=1
# Include all complex paths on a subset.
for path in SAMPLES[:20]:
    for extra in [
        ['--threads','2','--parallel-depth','1','--relaxation','off','--residual-lb','core'],
        ['--threads','2','--parallel-depth','1','--relaxation','full','--relax-proof-threads','2','--relax-proof-depth','1'],
        ['--threads','2','--branching','close-stack','--closure-threads','2','--relaxation','full'],
        ['--threads','1','--relaxation','off','--group-moves','0','--sort-moves','0','--hash','0'],
    ]:
        a=run(path,'off',extra);b=run(path,'counts',extra)
        assert a['status']==b['status']=='OPTIMAL',(path.name,extra,a['status'],b['status'])
        assert a['upper_bound']==b['upper_bound']==a['lower_bound']==b['lower_bound'],(path.name,extra)
# Confirm that interrupted solve returns valid bounds under cache option.
for size in (400,600):
    path=ROOT/f'instances/frinhani_carvalho_soma/{size}/Random-{size}-{size}-2-1.txt'
    j=run(path,'counts',['--threads','2','--time-limit','0.15','--relaxation','off'],timeout=8)
    assert j['status'] in {'TIME_LIMIT','OPTIMAL','NODE_LIMIT'}
    assert j['upper_bound']>=j['lower_bound']
print(f'PARTIAL-STATE: {checks} checks passed; {with_counts_work} serial runs built cache')
