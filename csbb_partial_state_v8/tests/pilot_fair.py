#!/usr/bin/env python3
import csv,json,subprocess,time
from pathlib import Path
R=Path(__file__).resolve().parents[1];B=R/'csbb_modern';out=[]
for n in [400,600,800,1000]:
 for density in [2,10]:
  instance=R/'instances'/f'synthetic_{n}x{n}_items_per_customer_{density}.cs'
  base=[str(B),'--input',str(instance),'--input-format','cs','--quiet','--json','--threads','1','--time-limit','0.30']
  seed=json.loads(subprocess.run(base+['--relaxation','full','--time-limit','0.000001'],text=True,capture_output=True,check=True).stdout)
  assert len(seed['best_sequence_0_based'])==n
  warm=R/'instances'/f'seed_{n}_{density}.warm'
  warm.write_text(f"value {seed['upper_bound']}\nsequence_0_based "+' '.join(str(x) for x in seed['best_sequence_0_based'])+'\n')
  for mode in ['off','full']:
   p=subprocess.run(base+['--relaxation',mode,'--warm-start-file',str(warm),'--warm-start-policy','sequence','--relax-max-time-share','0.35'],text=True,capture_output=True,timeout=30)
   if p.returncode!=0:print('BAD',n,density,mode,p.stderr);raise Exception()
   d=json.loads(p.stdout)
   r={'n':n,'items_per_customer':density,'mode':mode,'shared_incumbent':seed['upper_bound'],**{x:d.get(x) for x in ['status','upper_bound','lower_bound','gap_percent','nodes','elapsed_seconds','relaxation_full_merges','relaxed_proofs_attempted','relaxed_proof_infeasibilities','original_fallback_used','proof_source']}}
   out.append(r)
   print(n,density,mode,'initial',r['shared_incumbent'],'UB',r['upper_bound'],'LB',r['lower_bound'],'status',r['status'],flush=True)
with (R/'results/scalability_fair.csv').open('w',newline='') as fp:
 w=csv.DictWriter(fp,list(out[0]));w.writeheader();w.writerows(out)
(R/'results/scalability_fair.json').write_text(json.dumps(out,indent=2))
