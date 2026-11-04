#!/usr/bin/env python3
"""Conservative close-stack parallelism regression against exact customer search.

All closure failures still pass to customer search; a complete closure frontier is
never used as an infeasibility certificate. Also tests time/node exits and flags.
"""
import json
import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'csbb_modern'


def run(path, *args, timeout=12):
    cmd = [str(EXE), '--input', str(path), '--input-format', 'cs', '--quiet', '--json',
           '--relaxation', 'off', '--auto-lower-mode', 'graph', *args]
    result = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise AssertionError(f'Failed: {cmd}\n{result.stderr[-2000:]}')
    return json.loads(result.stdout)


def verified(result, optimum, desc):
    assert (result['status'], result['upper_bound'], result['lower_bound']) == ('OPTIMAL', optimum, optimum), (desc, result)


def main():
    rng = random.Random(20092026)
    cases = []
    parallel_hits = 0
    more_than_one_worker = 0
    checked = 0
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        # heterogeneous density, duplicates, disconnected components and singletons
        for ix in range(20):
            n = 4 + ix % 7
            m = 4 + (ix * 5) % 9
            density = [.10, .20, .36, .54, .82][ix % 5]
            matrix = [[int(rng.random() < density) for _ in range(m)] for _ in range(n)]
            for row in matrix:
                if not any(row): row[rng.randrange(m)] = 1
            if ix % 11 == 0: matrix[-1] = matrix[0].copy()
            p = td / f'r{ix:03}.cs'
            p.write_text(f'{n} {m}\n' + ''.join(' '.join(map(str, row)) + '\n' for row in matrix))
            cases.append(p)
        cases += [ROOT / 'instances/A_AP-9.cs', ROOT / 'tests/invalid_graph_lb_counterexample.cs']
        for p in cases:
            oracle = run(p, '--branching', 'customer', '--threads', '1')
            assert oracle['status'] == 'OPTIMAL', (p, oracle)
            optimum = oracle['upper_bound']
            for threads, depth, cap, mode in [
                (1, 1, 8192, 'close-stack'),
                (2, 1, 8192, 'close-stack'),
                (4, 2, 16, 'close-stack'),
                (3, 3, 1, 'close-stack'),
                (4, 2, 64, 'auto'),
            ]:
                out = run(p, '--branching', mode, '--threads', str(threads),
                          '--parallel-depth', str(depth), '--closure-frontier-cap', str(cap),
                          '--residual-lb', 'next')
                verified(out, optimum, (p.name, threads, depth, cap, mode))
                assert out['closure_threads'] == threads, (p.name, out)
                assert out['closure_parallel_depth'] == depth, (p.name, out)
                assert out['closure_max_workers'] <= threads
                parallel_hits += out['closure_parallel_calls']
                if out['closure_max_workers'] > 1: more_than_one_worker += 1
                checked += 1
        assert parallel_hits > 0 and more_than_one_worker > 0, 'Parallel closure was never actually entered'
        # Direct override of inherited workers/depth; disabling explicitly must be serial.
        p = ROOT / 'instances/A_AP-9.cs'
        serial = run(p, '--branching', 'close-stack', '--threads', '8',
                     '--closure-threads', '1', '--closure-parallel-depth', '2')
        assert (serial['closure_threads'], serial['closure_parallel_calls']) == (1, 0)
        disabled = run(p, '--branching', 'close-stack', '--threads', '8',
                       '--closure-parallel-depth', '0')
        assert disabled['closure_parallel_calls'] == 0
        for cap in [1, 2, 3]:
            out = run(p, '--branching', 'close-stack', '--threads', '4',
                      '--closure-parallel-depth', '3', '--closure-frontier-cap', str(cap))
            verified(out, 5, ('cap', cap))
        # Node limit and time limit: no false OPTIMAL certificate.
        for cap in [1, 5, 15, 100, 500]:
            out = run(p, '--branching', 'close-stack', '--threads', '4',
                      '--parallel-depth', '2', '--node-limit', str(cap))
            assert out['lower_bound'] <= 5 <= out['upper_bound'], (cap, out)
            if out['status'] == 'OPTIMAL': assert out['lower_bound'] == out['upper_bound'] == 5
        for time_limit in [0.00001, 0.001]:
            out = run(p, '--branching', 'close-stack', '--threads', '4',
                      '--parallel-depth', '2', '--time-limit', str(time_limit))
            assert out['lower_bound'] <= 5 <= out['upper_bound'], out
        # Full relaxation combined with closure/auto, plus older customer dispatcher.
        for branch in ('customer','close-stack','auto'):
            out = run(p, '--branching', branch, '--threads', '4', '--parallel-depth', '2',
                      '--relaxation', 'full', '--relax-proof-threads', '3', timeout=25)
            verified(out, 5, ('full',branch))
        # Invalid options fail without running.
        for flags in [['--closure-threads','-1'], ['--closure-parallel-depth','-2'],
                      ['--closure-frontier-cap','0']]:
            proc = subprocess.run([str(EXE), '--input', str(p), *flags], text=True, capture_output=True)
            assert proc.returncode != 0, flags
    stats = {'random_instances':20,'additional_instances':2, 'regression_variant_runs':checked,
             'parallel_closure_calls':parallel_hits,'runs_with_multiple_workers':more_than_one_worker,
             'caps':'PASS','node_time_limits':'PASS','full_relaxation':'PASS','invalid_flags':'PASS','result':'PASS'}
    results = ROOT / 'results'
    results.mkdir(parents=True, exist_ok=True)
    (results/'parallel_close_stack_regression.json').write_text(json.dumps(stats,indent=2)+'\n')
    print(json.dumps(stats,indent=2))

if __name__ == '__main__':
    main()
