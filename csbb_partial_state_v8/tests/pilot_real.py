#!/usr/bin/env python3
import json,csv,subprocess,time
from pathlib import Path
R=Path(__file__).resolve().parents[1];BIN=R/'csbb_modern';DATA=R/'instances/frinhani_carvalho_soma';
OUT=R/'results/real_benchmark';OUT.mkdir(parents=True,exist_ok=True)
rows=[]
for f in sorted(DATA.glob('*/Random-*.txt')):
 base=[str(BIN),'--input',str(f),'--input-format','cs','--presence','legacy-one','--quiet','--json','--threads','1']
 initial=json.loads(subprocess.run(base+['--relaxation','full','--time-limit','0.000001'],text=True,capture_output=True,check=True,timeout=30).stdout)
 seed=OUT/(f.stem+'.warm')
 seed.write_text('value '+str(initial['upper_bound'])+'\nsequence_0_based '+' '.join(map(str,initial['best_sequence_0_based']))+'\n')
 for mode in ['off','root-merge-probe','full']:
  cmd=base+['--relaxation',mode,'--time-limit','2','--warm-start-file',str(seed),'--warm-start-policy','sequence','--relax-max-time-share','0.3']
  start=time.monotonic();p=subprocess.run(cmd,capture_output=True,text=True,timeout=8);wall=time.monotonic()-start
  if p.returncode != 0: print('ERR',p.stderr[:300],f,mode);raise RuntimeError(p.stderr)
  d=json.loads(p.stdout)
  record={'instance':f.name,'size':int(f.parent.name),'mode':mode,'initial_ub':initial['upper_bound'],
   **{k:d.get(k) for k in ['status','upper_bound','lower_bound','gap_percent','elapsed_seconds','nodes','relaxed_proof_status','proof_source','relaxed_proofs_attempted','relaxed_proof_infeasibilities','relaxation_full_merges','relaxation_full_unmerges','relaxation_full_seconds','original_fallback_used']}, 'wall_seconds':round(wall,3)}
  rows.append(record)
  (OUT/(f.stem+'_'+mode+'.json')).write_text(json.dumps(d,indent=2))
  print(f.name,mode,'init',record['initial_ub'],'UB',record['upper_bound'],'LB',record['lower_bound'],record['status'],'merges',record['relaxation_full_merges'],f"wall={wall:.2f}",flush=True)
with (OUT/'summary.csv').open('w',newline='') as fp:
 writer=csv.DictWriter(fp,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
(OUT/'summary.json').write_text(json.dumps(rows,indent=2))
