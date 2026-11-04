#!/usr/bin/env python3
"""Short non-conclusive smoke pilot; NOT a speedup claim."""
import csv, json, subprocess, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOLVER=ROOT/'csbb_modern'
paths=sorted((ROOT/'instances/frinhani_carvalho_soma').glob('*/Random-*-2-1.txt'))
out=[]
for p in paths:
  for nthreads in [1,4]:
    cmd=[str(SOLVER),'--input',str(p),'--input-format','cs','--quiet','--json',
         '--auto-lower-mode','graph','--branching','close-stack','--relaxation','off',
         '--threads',str(nthreads),'--parallel-depth','1','--time-limit','1.25','--profile','1']
    t=time.monotonic()
    try:
      res=subprocess.run(cmd,capture_output=True,text=True,timeout=8)
      if res.returncode: raise RuntimeError(res.stderr)
      d=json.loads(res.stdout)
      out.append({'instance':p.name,'threads':nthreads,'status':d['status'],
                  'ub':d['upper_bound'],'lb':d['lower_bound'],'gap':d['gap_percent'],
                  'elapsed':round(time.monotonic()-t,3),'closure_parallel_calls':d['closure_parallel_calls'],
                  'closure_max_workers':d['closure_max_workers'],'closure_frontier_states':d['closure_frontier_states'],
                  'closure_branch_nodes':d['closure_branch_nodes']})
    except Exception as ex:
      out.append({'instance':p.name,'threads':nthreads,'status':'TEST_ERROR','error':str(ex)[:200]})
results=ROOT/'results/parallel_closure_scalability_pilot.csv'
with results.open('w',newline='') as f:
  cols=['instance','threads','status','ub','lb','gap','elapsed','closure_parallel_calls','closure_max_workers','closure_frontier_states','closure_branch_nodes','error']
  w=csv.DictWriter(f,fieldnames=cols);w.writeheader();w.writerows(out)
print(json.dumps(out,indent=2))
print(results)
