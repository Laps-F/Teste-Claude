#!/usr/bin/env python3
"""Independent MOSP customer-graph oracle, legacy-v-modern regression and adverse inputs."""
import json,random,subprocess,sys,os,functools,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MODERN=ROOT/'csbb_modern';BASE=ROOT/'csbb_baseline';LEGACY=ROOT/'original/chu_stuckey_2009'
DIR=ROOT/'tests/generated';DIR.mkdir(parents=True,exist_ok=True)

def oracle(mat):
 n=len(mat);m=len(mat[0]); adj=[1<<i for i in range(n)]
 for j in range(m):
  users=[i for i in range(n) if mat[i][j]]
  for i in users:
   for q in users:adj[i]|=1<<q
 full=(1<<n)-1
 @functools.lru_cache(None)
 def solve(rem):
  if not rem:return 0
  opened=0
  for i in range(n):
   if not rem>>i&1: opened|=adj[i]
  best=n
  for i in range(n):
   if rem>>i&1:best=min(best,max(((opened|adj[i])&rem).bit_count(),solve(rem&~(1<<i))))
  return best
 return solve(full)

def run_modern(fname,mode,extra=()):
 cmd=[str(MODERN),'--input',str(fname),'--input-format','cs','--quiet','--relaxation',mode,'--json','--time-limit','5',*extra]
 p=subprocess.run(cmd,capture_output=True,text=True,timeout=12)
 if p.returncode!=0:raise RuntimeError((p.stderr,p.stdout,cmd))
 return json.loads(p.stdout)

def run_legacy(fname):
 p=subprocess.run([str(LEGACY),'-x'],input=fname.read_text(),capture_output=True,text=True,timeout=12)
 if p.returncode!=0:raise RuntimeError('original failed '+p.stderr+' '+p.stdout[-300:])
 return int(p.stdout.strip().splitlines()[-1].split()[0])

def generate():
 rng=random.Random(302109)
 samples=[]
 for i in range(76):
  n=rng.randint(4,10);m=rng.randint(3,12)
  density=[.08,.20,.4,.75,.95][i%5]
  mat=[[int(rng.random()<density) for j in range(m)] for _ in range(n)]
  # include twins, isolates, disconnected product components
  if i%7==0:mat[1]=mat[0][:]
  if i%11==0:
   for a in range(n):
    for b in range(m):
     if (a<n//2)!=(b<m//2):mat[a][b]=0
  for row in mat:
   if not any(row):row[rng.randrange(m)]=1
  f=DIR/f'regression{i:03}.cs';f.write_text(f'{n} {m}\n'+''.join(' '.join(map(str,row))+'\n' for row in mat))
  samples.append((f,mat))
 return samples

def main():
 rows=[];start=time.monotonic();fail=0; proved_relaxed=0;fallback=0
 for f,mat in generate():
  opt=oracle(mat)
  orig=run_legacy(f)
  no=run_modern(f,'off')
  full=run_modern(f,'full')
  enhanced=run_modern(f,'full',['--threads','2','--parallel-depth','1','--branching','close-stack'])
  lifo=run_modern(f,'full',['--relax-unmerge-policy','lifo'])
  vals=[orig,no['upper_bound'],full['upper_bound'],enhanced['upper_bound'],lifo['upper_bound']]
  if min(vals)!=opt or max(vals)!=opt or any(x['status']!='OPTIMAL' or x['lower_bound']!=opt for x in [no,full,enhanced,lifo]):
   fail+=1;print('FAIL',f.name,'oracle',opt, 'values',vals, file=sys.stderr)
  proved_relaxed+=int(full['proof_source']=='RELAXED_GRAPH_EXACT')
  fallback+=int(full['original_fallback_used'])
  rows.append({'file':f.name,'oracle':opt,'legacy':orig,'modern_off':no['upper_bound'],'full':full['upper_bound'],'full_proof':full['proof_source'],'full_merges':full['relaxation_full_merges'],'full_unmerges':full['relaxation_full_unmerges'],'full_proof_calls':full['relaxed_proofs_attempted'],'parallel':enhanced['upper_bound'],'lifo':lifo['upper_bound']})
 # adversarial: an unverified upper bound of one MUST NOT certify optimum.
 f,mat=generate()[2];opt=oracle(mat)
 invalid_warm=ROOT/'tests/invalid_value_only.warm';invalid_warm.write_text('value 1\n')
 # New certificate policy: value-only warm starts fail closed, never silently
 # participating in proof search, in every relaxation mode.
 for mode in ['off','full']:
  p=subprocess.run([str(MODERN),'--input',str(f),'--input-format','cs','--quiet',
                    '--relaxation',mode,'--json','--warm-start-file',str(invalid_warm),
                    '--warm-start-policy','value'],capture_output=True,text=True,timeout=12)
  assert p.returncode==2 and 'ERROR:' in p.stderr,(mode,p.stderr)
 for overrides in [['--upper','1'],['--auto-lower-mode','graph']]:
  full=run_modern(f,'full',overrides)
  assert full['upper_bound']==opt and full['lower_bound']==opt and full['status']=='OPTIMAL',(overrides,full)
 # Counterexample to the earlier (invalid) graph/model lower-bound formulas.
 bad_lb=ROOT/'tests/invalid_graph_lb_counterexample.cs'
 for mode in ['off','full']:
  for bound_mode in ['legacy','model','graph']:
   d=run_modern(bad_lb,mode,['--auto-lower-mode',bound_mode])
   assert d['status']=='OPTIMAL' and d['upper_bound']==2 and d['lower_bound']==2,(mode,bound_mode,d)
 # Force early termination: output is never a false optimality proof.
 for mode in ['full','off']:
  r=run_modern(f,mode,['--node-limit','1'])
  assert r['upper_bound']>=r['lower_bound'] and r['gap_percent']>=0
  if r['status']=='OPTIMAL': assert r['upper_bound']==opt
 out=ROOT/'results/regression.json';out.parent.mkdir(parents=True,exist_ok=True)
 out.write_text(json.dumps({'test_count':len(rows),'failures':fail,'relaxed_graph_proofs':proved_relaxed,'fallbacks':fallback,'seconds':round(time.monotonic()-start,3),'cases':rows},indent=2))
 print(f'REGRESSION: {len(rows)} cases, {fail} failures, relaxed_graph_proofs={proved_relaxed}, fallbacks={fallback}, elapsed={time.monotonic()-start:.2f}s')
 return 1 if fail else 0
if __name__=='__main__':sys.exit(main())
