#!/usr/bin/env python3
"""Lightweight regression of the controlled batch protocol and the report."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import controlled_experiments as ex
import controlled_report as rp


def relevant(cfg):
    cfg = dict(cfg)
    if cfg['relaxation'] != 'full':
        for key in ('relax-proof-threads','relax-proof-depth','relax-proof-frontier-cap','relax-unmerge-policy','relax-max-time-share','relax-merge-policy'):
            cfg.pop(key, None)
    if cfg['branching'] == 'customer':
        for key in ('closure-threads','closure-parallel-depth','closure-frontier-cap'):
            cfg.pop(key, None)
    return cfg


class ProtocolTests(unittest.TestCase):
    def test_every_pair_changes_one_effective_factor(self):
        V, P = ex.variants_and_pairs(4)
        for key, comp in P.items():
            left = V[comp.control]; right=V[comp.treatment]
            l = relevant({**ex.BASE, **left.overrides}); r = relevant({**ex.BASE, **right.overrides})
            keys = set(l) | set(r)
            # Activating a previously inactive subsystem also activates its
            # subordinate tuning options; that is one functional intervention.
            if l.get('relaxation') != r.get('relaxation'):
                keys = {k for k in keys if not k.startswith('relax-')}
            if l.get('branching') != r.get('branching'):
                keys = {k for k in keys if not k.startswith('closure-')}
            differences = [k for k in keys if l.get(k) != r.get(k)]
            differences += ['warm-mode'] if left.warm != right.warm else []
            self.assertEqual(len(differences), 1, (key, differences))

    def test_config_list(self):
        V, P = ex.variants_and_pairs(4)
        self.assertEqual(len(P), 19)
        self.assertTrue(len(ex.selected_variants(list(P.values()),V)) <= len(V))

    def test_solver_command_contract(self):
        V, _ = ex.variants_and_pairs(4)
        cmd = ex.solver_command(Path('/bin/s'), Path('/tmp/i'), V['full_relax_parallel'], Path('/tmp/warm'), 1.0, None)
        for flag in ['--relaxation', '--relax-proof-threads','--threads','--warm-start-policy','--json','--time-limit']:
            self.assertIn(flag, cmd)
        self.assertEqual(cmd[cmd.index('--warm-start-policy')+1], 'strict')

    def test_invalid_certificate_not_valid_data(self):
        self.assertEqual(ex.validated_output({'status':'OPTIMAL','upper_bound':5,'lower_bound':5,'warm_start_verified':False}, True),'INVALID_WARM_START')
        self.assertEqual(ex.validated_output({'status':'OPTIMAL','upper_bound':2,'lower_bound':5}, False),'INVALID_BOUNDS')
        self.assertEqual(ex.validated_output({'status':'OPTIMAL','upper_bound':4,'lower_bound':3}, False),'INVALID_OPTIMALITY')
        self.assertIsNone(ex.validated_output({'status':'TIME_LIMIT','upper_bound':8,'lower_bound':2}, False))

    def test_speedup_only_for_optimal(self):
        row={'valid':1,'failures':0,'ub':8,'lb':2,'gap':300,'time':10,'all_optimal':False}
        result=rp.compare_runs(row,row)
        self.assertIsNone(result['proof_speedup'])
        r2={**row,'ub':8,'lb':8,'gap':0,'all_optimal':True,'time':5}
        r1={**r2,'time':10}
        self.assertEqual(rp.compare_runs(r1,r2)['proof_speedup'],2)

    def test_real_instance_shaping(self):
        for dim in [400,600,800,1000]:
            inst=ROOT/f'instances/frinhani_carvalho_soma/{dim}/Random-{dim}-{dim}-2-1.txt'
            self.assertEqual(ex.read_instance_shape(inst),(dim,dim))

if __name__ == '__main__': unittest.main()
