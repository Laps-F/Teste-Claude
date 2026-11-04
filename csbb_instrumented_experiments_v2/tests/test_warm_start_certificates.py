#!/usr/bin/env python3
"""Step 2: end-to-end MCNh product-sequence certificates, invalid inputs and DP."""
import functools
import json
import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BB = ROOT / 'csbb_modern'
MCNH = ROOT / 'mcnh_modern'


def run(args, timeout=12):
    return subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=timeout)


def get_bb(path, warm=None, mode='off', policy='value', extras=()):
    args = [BB, '--input', path, '--input-format', 'cs', '--presence', 'legacy-one', '--relaxation', mode,
            '--warm-start-policy', policy, '--quiet', '--json', '--time-limit', '4', *extras]
    if warm is not None: args += ['--warm-start-file', warm]
    p = run(args)
    return p, json.loads(p.stdout) if p.returncode == 0 else None


def oracle(matrix):
    nc=len(matrix); np=len(matrix[0]); cross=[1<<i for i in range(nc)]
    for j in range(np):
        rows=[i for i in range(nc) if matrix[i][j]]
        for a in rows:
            for b in rows: cross[a] |= 1<<b
    full=(1<<nc)-1
    @functools.lru_cache(None)
    def dp(rem):
        if not rem: return 0
        opened=0
        for i in range(nc):
            if not (rem>>i)&1: opened|=cross[i]
        return min(max(((opened|cross[i])&rem).bit_count(),dp(rem & ~(1<<i))) for i in range(nc) if (rem>>i)&1)
    return dp(full)


def write_cs(p,matrix):
    p.write_text(f'{len(matrix)} {len(matrix[0])}\n' + ''.join(' '.join(map(str,row))+'\n' for row in matrix))


def put(warm, src, **changes):
    lines=src.read_text().splitlines()
    for name, v in changes.items():
        lines=[(name+' '+str(v)) if x.startswith(name+' ') else x for x in lines]
    warm.write_text('\n'.join(lines)+'\n')


def main():
    random.seed(20092026)
    ntests=0
    with tempfile.TemporaryDirectory(prefix='csbb_warm_') as td:
        d=Path(td)
        for case in range(30):
            nc=random.randint(3,9); np=random.randint(3,11)
            mat=[[int(random.random() < [.2,.5,.9][case%3]) for _ in range(np)] for _ in range(nc)]
            for row in mat:
                if not any(row): row[random.randrange(np)] = 1
            f=d/'case.cs'; warm=d/'mcnh.warm'
            write_cs(f,mat)
            optimum=oracle(mat)
            for pre in (0,1):
                p=run([MCNH,'--input',f,'--input-format','cs','--presence','legacy-one','--preprocess',pre,
                       '--objective-orientation','customer','--warm-start-output',warm])
                assert p.returncode==0,(p.stderr,case,pre)
                for mode in ['off','full']:
                    q,out=get_bb(f,warm,mode)
                    assert q.returncode==0,(case,pre,mode,q.stderr)
                    assert out['warm_start_verified'] and out['warm_start_verified_value']>=optimum,(case,pre,mode,out)
                    assert out['warm_start_verified_value']==out['warm_start_declared_value']
                    assert out['warm_start_faggioli_value']==out['warm_start_sequence_value']
                    assert out['upper_bound']==out['lower_bound']==optimum and out['status']=='OPTIMAL',(case,pre,mode,out)
                    assert out['warm_start_sequence_domain']=='products'
                    ntests+=1
        # Asymmetric matrix prevents inadvertent row/column aliasing.
        mat=[[1,0,1,0],[0,1,1,0],[0,1,0,1]]
        f=d/'bad.cs';write_cs(f,mat)
        warm=d/'good.warm'
        assert run([MCNH,'--input',f,'--input-format','cs','--objective-orientation','customer',
                    '--warm-start-output',warm]).returncode==0
        original=warm.read_text()
        errors={
          'wrong_declared':{'value': '1'},
          'wrong_domain':{'sequence_domain':'customers'},
          'wrong_objective':{'objective_domain':'pieces'},
          'wrong_dimensions':{'n_products':'999'},
          'duplicate_sequence':{'sequence_0_based':'0 0 1 2'},
          'truncated_sequence':{'sequence_0_based':'0 1 2'},
        }
        for name,change in errors.items():
            mutated=d/(name+'.warm');put(mutated,warm,**change)
            for mode in ('off','full'):
                for policy in ('value','strict'):
                    p,out=get_bb(f,mutated,mode,policy)
                    assert p.returncode==2,(name,mode,policy,p.stdout,p.stderr)
                    ntests+=1
        old=d/'value_only.warm';old.write_text('value 1\n')
        for mode in ('off','full'):
            p,out=get_bb(f,old,mode)
            assert p.returncode==2
            ntests+=1
        # In sequence mode the *verified* cost is used, not a lie in value field.
        changed=d/'incorrect_value.warm';put(changed,warm,value='1')
        p,out=get_bb(f,changed,'off','sequence')
        assert p.returncode==0 and out['warm_start_verified'] and out['warm_start_validation_status']=='VERIFIED_SEQUENCE_DECLARATION_IGNORED'
        assert out['warm_start_value']==out['warm_start_verified_value'] != 1
        ntests+=1
        # Explicit numeric UBs are never accepted without a witness, even off.
        for mode in ('off','full'):
            p,out=get_bb(f,None,mode,extras=['--upper','1'])
            assert p.returncode==0 and out['upper_bound']==out['lower_bound']==oracle(mat) and out['full_explicit_upper_ignored']
            ntests+=1
        # Legacy orientation never becomes a trusted CS warm start.
        legacy=d/'legacy.warm'
        assert run([MCNH,'--input',f,'--input-format','cs','--objective-orientation','piece',
                    '--warm-start-output',legacy]).returncode==0
        p,out=get_bb(f,legacy)
        assert p.returncode==2
        ntests+=1
        # SCOOP compatibility uses same transposition; preserve MCNh input order.
        # Build SCOOP format from CS matrix by emitting columns in reverse.
        scoop=d/'case.scoop'
        scoop.write_text(f'{len(mat[0])} {len(mat)}\n'+''.join(' '.join(str(mat[c][j]) for c in range(len(mat)))+'\n' for j in reversed(range(len(mat[0])))))
        warm2=d/'scoop.warm'
        p=run([MCNH,'--input',scoop,'--input-format','scoop','--objective-orientation','customer','--warm-start-output',warm2])
        assert p.returncode==0,p.stderr
        bp=run([BB,'--input',scoop,'--input-format','scoop','--warm-start-file',warm2,'--json','--quiet','--relaxation','full'])
        assert bp.returncode==0,bp.stderr
        r=json.loads(bp.stdout)
        assert r['warm_start_verified'] and r['upper_bound']==r['lower_bound']==oracle(mat)
        ntests+=1
    print(f'WARM-START CERTIFICATES: {ntests} checks passed')

if __name__=='__main__':main()
