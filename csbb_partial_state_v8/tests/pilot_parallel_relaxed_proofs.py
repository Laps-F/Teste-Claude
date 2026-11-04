#!/usr/bin/env python3
import json,subprocess,csv,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
out=root/'results/parallel_relaxed_pilot.csv'
instances=[root/f'instances/frinhani_carvalho_soma/{n}/Random-{n}-{n}-2-1.txt' for n in (400,600,800,1000)]
rows=[]
for p in instances:
    for worker in (1,4):
        args=[str(root/'csbb_modern'),'--input',str(p),'--input-format','cs','--presence','legacy-one','--quiet','--json','--relaxation','full',
              '--threads',str(worker),'--relax-proof-threads',str(worker),'--relax-proof-depth','1',
              '--time-limit','1.5','--relax-max-time-share','0.6']
        start=time.monotonic()
        run=subprocess.run(args,capture_output=True,text=True,timeout=12)
        if run.returncode:raise RuntimeError((p,worker,run.stderr))
        d=json.loads(run.stdout)
        assert d['upper_bound']>=d['lower_bound'],(p,worker,d)
        assert d['status'] != 'OPTIMAL' or d['upper_bound']==d['lower_bound']
        rows.append({k:d.get(k) for k in ('status','upper_bound','lower_bound','gap_percent','elapsed_seconds','relaxed_proof_parallel_calls','relaxed_proof_max_workers','relaxed_proof_frontier_states','relaxed_proofs_attempted','relaxed_proof_status') }|{'instance':p.name,'requested_workers':worker,'wall_clock':round(time.monotonic()-start,3)})
        print(p.name,worker,d['status'],'UB',d['upper_bound'],'LB',d['lower_bound'],'parallel',d['relaxed_proof_parallel_calls'],flush=True)
with out.open('w',newline='') as f:
    writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
print('Pilot CSV',out)
