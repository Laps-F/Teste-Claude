#!/usr/bin/env python3
"""MOSP evaluator v2 bridge: original semantics, malformed certificates, and parallel searches."""
import json, random, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOLVER=ROOT/'csbb_modern'
MCNH=ROOT/'mcnh_modern'

def run(*args, timeout=12):
    return subprocess.run(list(map(str,args)),capture_output=True,text=True,timeout=timeout)

def result(inst,warm, *opts,secs=2.0):
    args=[SOLVER,'--input',inst,'--input-format','cs','--presence','legacy-one',
          '--quiet','--json','--time-limit',str(secs),'--profile-evaluator','1',*opts]
    if warm: args+=['--warm-start-file',warm]
    out=run(*args,timeout=max(10,secs+5))
    return out, json.loads(out.stdout) if out.returncode==0 else None

def main():
    checks=0
    rng=random.Random(442233)
    with tempfile.TemporaryDirectory(prefix='csbb_eval_v6_') as td:
        td=Path(td)
        for trial in range(24):
            nc=rng.randrange(3,10);np=rng.randrange(3,13)
            mat=[[int(rng.random()<[.12,.3,.8][trial%3]) for _ in range(np)] for _ in range(nc)]
            for row in mat:
                if not any(row): row[rng.randrange(np)]=1
            inp=td/'case.cs'; warm=td/'solution.warm'
            inp.write_text(f'{nc} {np}\n'+''.join(' '.join(map(str,r))+'\n' for r in mat))
            cmd=[MCNH,'--input',inp,'--input-format','cs','--presence','legacy-one',
                 '--preprocess',str(trial%2),'--objective-orientation','customer','--warm-start-output',warm]
            p=run(*cmd)
            assert p.returncode==0,(trial,p.stderr)
            configurations=[['--witness-evaluator','legacy','--witness-decode','legacy'],
                ['--witness-evaluator','standalone','--witness-decode','sparse','--verify-customer-witness','1'],
                ['--witness-evaluator','dual','--witness-decode','dual','--verify-customer-witness','1',
                  '--branching','close-stack','--relaxation','full','--threads','2']]
            answers=[]
            for opts in configurations:
                q,r=result(inp,warm,*opts)
                assert q.returncode==0,(trial,opts,q.stderr)
                assert r['status']=='OPTIMAL' and r['upper_bound']==r['lower_bound'],(trial,opts,r)
                assert r['warm_start_verified'],(trial,opts)
                assert r['warm_start_faggioli_value']==r['warm_start_sequence_value'],(trial,opts)
                answers.append(r['upper_bound']);checks+=1
            assert len(set(answers))==1,(trial,answers)
            # All witness evaluators must reject a falsified declared UB.
            wrong=td/'tampered.warm'
            content=warm.read_text()
            wrong.write_text('\n'.join('value 9999' if x.startswith('value ') else x for x in content.splitlines())+'\n')
            for backend in ['legacy','standalone','dual']:
                q,r=result(inp,wrong,'--witness-evaluator',backend)
                assert q.returncode==2 and r is None,(backend,q.returncode,q.stderr)
                checks+=1
        # Real benchmark shapes and orientations, cheap timer-limited integration tests.
        for size in [400,600,800,1000]:
            inp=ROOT/f'instances/frinhani_carvalho_soma/{size}/Random-{size}-{size}-2-1.txt'
            warm=td/f'{size}.warm'
            p=run(MCNH,'--input',inp,'--input-format','cs','--presence','legacy-one',
                  '--objective-orientation','customer','--preprocess','1','--warm-start-output',warm,timeout=45)
            assert p.returncode==0,(size,p.stderr)
            for backend in ['standalone','dual']:
                q,r=result(inp,warm,'--witness-evaluator',backend,'--witness-decode','sparse',secs=0.2)
                assert q.returncode==0,(size,backend,q.stderr)
                assert r['warm_start_verified'] and r['warm_start_verified_value']==r['warm_start_declared_value']
                assert r['evaluator_incidences']>0 and r['evaluator_standalone_calls']==1,(size,backend,r)
                checks+=1
    print(f'EVALUATOR-INTEGRATION: {checks} checks passed (24 random, four real dimensions, invalid certificates)')
if __name__=='__main__':main()
