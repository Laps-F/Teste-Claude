#!/usr/bin/env python3
"""Exhaustive DP oracle for customer-search states; full CLI regressions."""
from pathlib import Path
import functools, json, random, subprocess, tempfile, textwrap
ROOT=Path(__file__).resolve().parents[1]
BB=ROOT/'csbb_modern'

def run(matrix, opts, time_limit=8):
    with tempfile.TemporaryDirectory() as td:
        p=Path(td)/'test.cs'
        p.write_text(f'{len(matrix)} {len(matrix[0])}\n'+''.join(' '.join(str(int(a)) for a in row)+'\n' for row in matrix))
        r=subprocess.run([str(BB),'--input',str(p),'--input-format','cs','--quiet','--json',
                          '--time-limit',str(time_limit),*opts],capture_output=True,text=True,timeout=time_limit+10)
        if r.returncode: raise AssertionError((r.returncode,r.stdout,r.stderr,opts,matrix))
        return json.loads(r.stdout)

def graph(matrix):
    n=len(matrix); m=len(matrix[0]); adj=[1<<i for i in range(n)]
    for j in range(m):
        group=[i for i in range(n) if matrix[i][j]]
        for a in group:
            for b in group: adj[a] |=1<<b
    return adj

def oracle_dp(adj):
    n=len(adj); full=(1<<n)-1
    @functools.lru_cache(None)
    def solve(rem):
        if not rem:return 0
        opened=0
        for a in range(n):
            if not (rem>>a)&1:opened |= adj[a]
        return min(max(((opened|adj[c])&rem).bit_count(),solve(rem^(1<<c)))
                   for c in range(n) if (rem>>c)&1)
    return solve

def main():
    rng=random.Random(337299)
    tests=0
    cases=[]
    # Adversarial examples where PRODUCT-graph model bounds are invalid.
    cases.append([[1,1,1,1,1]])  # optimum exactly 1
    cases.append([[1],[1],[1],[1],[1]]) # optimum 5
    cases.append([[1,0],[0,1]])
    cases.append([[1,0,1],[1,1,0],[0,1,1]])
    for t in range(125):
        n=rng.randrange(2,10); m=rng.randrange(2,12)
        d=[0.12,0.32,0.58,0.87][t%4]
        matrix=[[int(rng.random()<d) for _ in range(m)] for _ in range(n)]
        for row in matrix:
            if not any(row):row[rng.randrange(m)]=1
        cases.append(matrix)
    for ii,mat in enumerate(cases):
        adj=graph(mat); dp=oracle_dp(adj); n=len(mat); optimum=dp((1<<n)-1)
        if optimum<1:raise AssertionError('bad DP')
        # Inspect every reachable residual mask: O is fixed by the set of closed customers.
        for rem in range(1,1<<n):
            opened=0
            for a in range(n):
                if (not rem>>a&1):opened |=adj[a]
            active=(opened & rem).bit_count()
            one_step=active+min((adj[c]&~opened).bit_count() for c in range(n) if rem>>c&1)
            if one_step>dp(rem):
                raise AssertionError(('invalid first step bound',ii,rem,one_step,dp(rem)))
            # Check the induced customer's k-core against exact residual optimum.
            core_set=[a for a in range(n) if rem>>a&1]
            maxmindeg=0
            while core_set:
                v=min(core_set,key=lambda a:sum(int((adj[a]>>b)&1) for b in core_set if b!=a))
                min_deg=sum(int((adj[v]>>b)&1) for b in core_set if b!=v)
                maxmindeg=max(maxmindeg,min_deg)
                core_set.remove(v)
            if maxmindeg+1>dp(rem):
                raise AssertionError(('invalid residual degeneracy bound',ii,rem,maxmindeg+1,dp(rem)))
        # Four modes; 'off' and 'next' preserve root lower/optimum.
        for mode in ['off','next','core']:
            o=run(mat,['--residual-lb',mode,'--residual-core-max-vertices','0','--threads','1'])
            assert (o['status'],o['upper_bound'],o['lower_bound'])==('OPTIMAL',optimum,optimum),(ii,mode,o)
            assert o['residual_lb_mode']==mode
            tests+=1
        g=run(mat,['--auto-lower-mode','graph','--residual-lb','core','--residual-core-max-vertices','0'])
        assert g['customer_graph_core_lower_bound']<=optimum,(ii,g)
        assert g['applied_initial_lower_bound']<=optimum,(ii,g)
        assert (g['status'],g['upper_bound'],g['lower_bound'])==('OPTIMAL',optimum,optimum),(ii,g)
        tests+=1
    print(json.dumps({'cases':len(cases),'test_runs':tests,'residual_states_checked':sum((1<<len(a))-1 for a in cases), 'result':'PASS'},indent=2))
if __name__=='__main__':main()
