#!/usr/bin/env python3
"""Audit the adapted, actual PT API over all backend/energy/representation/move options."""
import argparse
import itertools
import json
import math
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument('--repo',type=Path,required=True)
parser.add_argument('--binary',default='mainMOSP_eval')
parser.add_argument('--output',type=Path)
args=parser.parse_args()
repo=args.repo.resolve();binary=(repo/'MOSP'/args.binary).resolve()
assert binary.is_file(),binary
patterns=[{0,1},{1,2},{0,2},{3,4},{4,5},{3,5},{0,3}]

def oracle(order,patterns,m):
    profile=[]
    first=[len(order)]*m;last=[-1]*m
    for t,j in enumerate(order):
        for x in patterns[j]:first[x]=min(first[x],t);last[x]=t
    for t in range(len(order)):profile.append(sum(first[x]<=t<=last[x] for x in range(m)))
    return max(profile,default=0),sum(profile)

def decode(items,patterns):
    visited=set();used=set();order=[]
    for j,p in enumerate(patterns):
        if not p:order.append(j);used.add(j)
    for x in items:
        visited.add(x)
        for j,p in enumerate(patterns):
            if j not in used and x in p and p<=visited:order.append(j);used.add(j)
    return order

records=[]
with tempfile.TemporaryDirectory(prefix='pt-mosp-evaluator-') as tmp:
    root=Path(tmp);n=len(patterns);m=6
    paths=[]
    for read in [0,1]:
        matrix=[[int(x in p) for x in range(m)] for p in patterns]
        if read==0:matrix=list(zip(*matrix))
        path=root/f'layout_{read}.txt'
        path.write_text(f'{n} {m}\n'+'\n'.join(' '.join(map(str,row)) for row in matrix)+'\n')
        paths.append(path)
    for read,sequence,move,energy,backend in itertools.product([0,1],[0,1],range(3),['mosp','mmosp'],['full','incremental','auto']):
        command=[str(binary),str(paths[read]),'--READ',str(read),'--SEQUENCE',str(sequence),
            '--MOV_TYPE',str(move),'--ENERGY',energy,'--EVALUATOR',backend,'--AUDIT','1',
            '--N_REPLICAS','4','--MCL','16','--PTL','8','--THREAD_USED','4','--TYPE_UPDATE','0',
            '--OUTDIR',str(root/'results')]
        result=subprocess.run(command,text=True,capture_output=True,timeout=30)
        assert result.returncode==0,(command,result.stdout,result.stderr)
        record=json.loads(result.stdout.splitlines()[-1])
        order=decode(record['solution'],patterns) if sequence else record['solution']
        assert order==record['pattern_sequence'] and sorted(order)==list(range(n))
        peak,total=oracle(order,patterns,m)
        score=peak+total/(n*peak) if peak else 0.0
        assert (record['mosp'],record['total_open'])==(peak,total),record
        assert abs(record['mmosp']-score)<1e-12
        assert record['energy']==(peak if energy=='mosp' else record['mmosp'])
        assert record['audited_evaluations']==4*16*8,record
        assert record['exchange_synchronizations']>0,record
        txt=root/'results'/'PT'/f'{energy}_{backend}'/f'layout_{read}(0)_res.txt'
        assert float(txt.read_text().splitlines()[2])==peak,'legacy output must report raw MOSP'
        record.update(read=read,move=move)
        records.append(record)
    # Degenerate inputs exercise constructor/neighbor guards and empty-pattern completion.
    for label,edge_patterns,items in [('singleton',[{0}],1),('empty-patterns',[set(),{0},set(),{0}],2),('no-items',[set(),set()],0),('no-patterns',[],3)]:
        path=root/f'{label}.txt';path.write_text(f'{len(edge_patterns)} {items}\n'+
            '\n'.join(' '.join(str(int(x in p)) for x in range(items)) for p in edge_patterns)+'\n')
        command=[str(binary),str(path),'--N_REPLICAS','4','--MCL','8','--PTL','6','--THREAD_USED','2',
                 '--TYPE_UPDATE','0','--SEQUENCE','0','--ENERGY','mmosp','--EVALUATOR','auto','--AUDIT','1','--OUTDIR',str(root/'edge')]
        result=subprocess.run(command,text=True,capture_output=True,timeout=30)
        assert result.returncode==0,(label,result.stdout,result.stderr)
        record=json.loads(result.stdout.splitlines()[-1]);peak,total=oracle(record['pattern_sequence'],edge_patterns,items)
        assert (record['mosp'],record['total_open'])==(peak,total)
        records.append(record|{'edge_case':label})
summary={'status':'PASS','pt_configurations':72,'edge_cases':4,'pt_proposals_audited':sum(r['audited_evaluations'] for r in records),'records':records}
if args.output:
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='records'}))
