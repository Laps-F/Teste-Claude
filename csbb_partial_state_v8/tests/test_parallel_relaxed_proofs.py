#!/usr/bin/env python3
"""Correctness/regression of exact contracted-graph parallel proofs."""
import json, random, subprocess, tempfile, functools, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SOLVER=ROOT/'csbb_modern'

def run(path, flags, limit=5.0):
    cmd=[str(SOLVER),'--input',str(path),'--input-format','cs','--quiet','--json','--relaxation','full','--time-limit',str(limit),*flags]
    p=subprocess.run(cmd,capture_output=True,text=True,timeout=limit+5)
    if p.returncode: raise AssertionError((cmd,p.stderr[-1000:]))
    return json.loads(p.stdout)

def dp_oracle(mat):
    n=len(mat);m=len(mat[0]); nb=[1<<i for i in range(n)]
    for j in range(m):
        vs=[i for i in range(n) if mat[i][j]]
        for i in vs:
            for v in vs: nb[i] |= 1<<v
    @functools.lru_cache(None)
    def f(r):
        if r==0:return 0
        opened=0
        for i in range(n):
            if not r>>i&1:opened |= nb[i]
        return min(max(((opened|nb[i])&r).bit_count(),f(r&~(1<<i))) for i in range(n) if r>>i&1)
    return f((1<<n)-1)


def main():
    rng=random.Random(332009)
    rows=[]
    with tempfile.TemporaryDirectory() as tmp:
        td=Path(tmp)
        cases=[]
        # Include disconnected, dense and sparse graphs, some with duplicate patterns.
        for j in range(56):
            n=4+j%9;m=3+(j*3)%12
            density=[.12,.24,.44,.72,.92][j%5]
            mat=[[int(rng.random()<density) for _ in range(m)] for _ in range(n)]
            for r in mat:
                if not any(r): r[rng.randrange(m)]=1
            if j%9==0: mat[-1]=mat[0].copy()
            f=td/f'r{j:03}.cs'
            f.write_text(f'{n} {m}\n'+''.join(' '.join(map(str,r))+'\n' for r in mat))
            cases.append((f,dp_oracle(mat)))
        cases.extend([(ROOT/'instances/A_AP-9.cs',5),
             (ROOT/'tests/invalid_graph_lb_counterexample.cs',2)])
        parallel_calls=0
        for f,optimum in cases:
            base=run(f,['--relax-proof-threads','1','--threads','1'])
            assert (base['upper_bound'],base['lower_bound'],base['status'])==(optimum,optimum,'OPTIMAL'),(f,base)
            for nworkers,depth,res_lb in [(2,1,'off'),(4,2,'next'),(3,2,'core')]:
                out=run(f,['--relax-proof-threads',str(nworkers),'--relax-proof-depth',str(depth),
                           '--threads',str(nworkers),'--parallel-depth','1','--residual-lb',res_lb])
                assert (out['upper_bound'],out['lower_bound'],out['status'])==(optimum,optimum,'OPTIMAL'),(f,nworkers,out)
                assert out['relax_proof_threads']==nworkers and out['relax_proof_depth']==depth
                assert out['relaxed_proof_max_workers']<=nworkers
                if out['relaxed_proof_status']=='INFEASIBLE_PROVED':assert out['proof_source']=='RELAXED_GRAPH_EXACT'
                parallel_calls += out['relaxed_proof_parallel_calls']
            rows.append({'instance':f.name,'optimum':optimum,'serial_proofs':base['relaxed_proofs_attempted']})
        assert parallel_calls>0,'No contracted proof actually launched multiple workers'
        # Bounded frontier remains complete even when the cap is very small.
        capped=run(ROOT/'instances/A_AP-9.cs',['--threads','4','--relax-proof-threads','4',
                 '--relax-proof-depth','4','--relax-proof-frontier-cap','3'])
        assert (capped['upper_bound'],capped['lower_bound'])==(5,5)
        assert capped['relaxed_proof_frontier_states']<=3*capped['relaxed_proofs_attempted']
        # Automatic inheritance must parallelize contracted proofs.
        inh=run(ROOT/'instances/A_AP-9.cs',['--threads','4','--parallel-depth','2'])
        assert inh['relax_proof_threads']==4 and inh['relax_proof_depth']==2
        assert inh['relaxed_proof_parallel_calls']>0
        # Early interruption must not fabricate optimality certificates.
        for node_cap in [1,4,12,50,200]:
            out=run(ROOT/'instances/A_AP-9.cs',['--threads','4','--relax-proof-threads','4','--relax-proof-depth','2',
                                                '--node-limit',str(node_cap)])
            assert out['lower_bound']<=5<=out['upper_bound'],(node_cap,out)
            if out['status']=='OPTIMAL':assert out['upper_bound']==out['lower_bound']==5
        # Explicitly limit relaxation time share; fallback must preserve certification.
        out=run(ROOT/'instances/A_AP-9.cs',['--threads','4','--relax-proof-depth','2',
                                             '--relax-max-time-share','0','--time-limit','2'])
        assert out['lower_bound']<=5<=out['upper_bound']
        # Invalid flags fail closed.
        for flags in [['--relax-proof-threads','-2'],['--relax-proof-depth','-3'],['--relax-proof-frontier-cap','0']]:
            p=subprocess.run([str(SOLVER),'--input',str(ROOT/'instances/A_AP-9.cs'),*flags],capture_output=True,text=True)
            assert p.returncode!=0
        print(json.dumps({'cases':len(rows),'variant_runs':len(rows)*4,'parallel_proof_calls':parallel_calls,
                          'inheritance':'PASS','node_limits':'PASS','invalid_flags':'PASS','result':'PASS'},indent=2))
        (ROOT/'results/parallel_relaxed_regression.json').write_text(json.dumps(rows,indent=2))
if __name__=='__main__':main()
