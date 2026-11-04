#!/usr/bin/env python3
import csv,json,random,subprocess,time,resource
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'results';DATA=ROOT/'instances';OUT.mkdir(exist_ok=True);DATA.mkdir(exist_ok=True)
rows=[]
for n in [400,600,800,1000]:
 for q in [2,10]:
  fn=DATA/f'synthetic_{n}x{n}_items_per_customer_{q}.cs'
  rng=random.Random(n*333+q)
  with fn.open('w') as fp:
   fp.write(f'{n} {n}\n')
   for p in range(n):
    selected=set(rng.sample(range(n),q))
    fp.write(' '.join('1' if j in selected else '0' for j in range(n))+'\n')
  for mode in ['off','full']:
   cmd=[str(ROOT/'csbb_modern'),'--input',str(fn),'--input-format','cs','--relaxation',mode,'--time-limit','0.30','--threads','1','--quiet','--json','--relax-max-time-share','0.35']
   t0=time.monotonic()
   p=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
   dt=time.monotonic()-t0
   try: d=json.loads(p.stdout)
   except Exception:
    print('FAIL',n,q,mode,'return',p.returncode,p.stderr[:400]);raise
   r={k:d.get(k) for k in ['status','upper_bound','lower_bound','gap_percent','elapsed_seconds','nodes','relaxed_proof_status','proof_source','relaxed_proofs_attempted','relaxation_full_merges','relaxation_full_unmerges','relaxation_full_seconds','original_fallback_used']}
   r.update(size=n,density=q,mode=mode,wall_seconds=round(dt,4),returncode=p.returncode)
   rows.append(r)
   print(n,q,mode,r['status'],r['upper_bound'],r['lower_bound'],r['relaxation_full_merges'],r['wall_seconds'],flush=True)
with (OUT/'scalability_pilot.csv').open('w',newline='') as fp:
 w=csv.DictWriter(fp,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
(OUT/'scalability_pilot.json').write_text(json.dumps(rows,indent=2))
