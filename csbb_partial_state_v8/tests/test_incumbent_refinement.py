#!/usr/bin/env python3
"""Step-5 regression: incremental MCNh incumbent refinement, proof safety, repeatability."""
import json, pathlib, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
BIN=ROOT/'csbb_modern'
MCNH=ROOT/'mcnh_modern'
INSTANCE=ROOT/'instances/A_AP-9.cs'
checks=0

def run(args, fail=False):
    global checks
    p=subprocess.run([str(BIN),'--input',str(INSTANCE),'--input-format','cs','--presence','legacy-one','--json','--quiet',*args],capture_output=True,text=True,timeout=20)
    if fail:
        assert p.returncode!=0,(p.stdout,p.stderr)
        checks+=1
        return None
    assert p.returncode==0,(p.stdout,p.stderr)
    r=json.loads(p.stdout)
    assert r['upper_bound']>=r['lower_bound']
    if r['status']=='OPTIMAL': assert r['upper_bound']==r['lower_bound']
    checks+=1
    return r

with tempfile.TemporaryDirectory() as tmp:
    warm=pathlib.Path(tmp)/'mcnh.warm'
    subprocess.run([str(MCNH),'--input',str(INSTANCE),'--input-format','cs','--presence','legacy-one',
                    '--objective-orientation','customer','--preprocess','1','--warm-start-output',str(warm)],check=True,capture_output=True,text=True)
    original=run(['--warm-start-file',str(warm),'--time-limit','2'])
    assert original['upper_bound']==original['lower_bound']==5
    for move in ('insert','swap','reverse','mixed'):
      for relax,branch,threads in [('off','customer','1'),('full','customer','2'),('full','close-stack','2')]:
        cert=pathlib.Path(tmp)/f'{move}_{relax}_{branch}.warm'
        params=['--warm-start-file',str(warm),'--warm-start-policy','strict','--witness-evaluator','dual',
                '--refine-incumbent','1','--refine-moves',move,'--refine-max-evals','350',
                '--refine-max-seconds','0.1','--refine-seed','7','--refine-output',str(cert),
                '--relaxation',relax,'--branching',branch,'--threads',threads,'--time-limit','2']
        r=run(params)
        assert r['upper_bound']==r['lower_bound']==5
        assert r['warm_start_verified'] is True
        assert r['refine_initial_ub']==6 and 5 <= r['refine_final_ub']<=6
        assert 0 <= r['refine_evaluations'] <= 350
        assert cert.exists()
        imported=run(['--warm-start-file',str(cert),'--warm-start-policy','strict','--witness-evaluator','dual','--time-limit','2'])
        assert imported['warm_start_verified_value']==r['refine_final_ub']
    a=run(['--warm-start-file',str(warm),'--refine-incumbent','1','--refine-max-evals','300',
           '--refine-max-seconds','1','--refine-seed','123','--time-limit','2'])
    b=run(['--warm-start-file',str(warm),'--refine-incumbent','1','--refine-max-evals','300',
           '--refine-max-seconds','1','--refine-seed','123','--time-limit','2'])
    for k in ('refine_initial_ub','refine_final_ub','refine_evaluations','refine_mosp_improvements','refine_tie_acceptances'):
        assert a[k]==b[k],(k,a[k],b[k])
    no_warm=run(['--refine-incumbent','1','--time-limit','2'])
    assert no_warm['refine_status']=='SKIPPED_NO_WARM_START'
    run(['--refine-incumbent','1','--refine-max-seconds','-0.2'],fail=True)
    run(['--refine-incumbent','1','--refine-moves','invalid'],fail=True)
    run(['--refine-output',str(pathlib.Path(tmp)/'bad.warm')],fail=True)
print(f'INCUMBENT REFINEMENT: {checks} checks passed')
