#!/usr/bin/env python3
"""Instrumented v2 smoke tests: progress series, conservation, workers, proof."""
import csv
import json
import math
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BIN=ROOT/'csbb_modern'
INSTANCE=ROOT/'instances/A_AP-9.cs'

class Telemetry(unittest.TestCase):
    def run_case(self, mode, branching, threads):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)/'progress.csv'
            cmd=[str(BIN),'--input',str(INSTANCE),'--input-format','cs',
                 '--relaxation',mode,'--branching',branching,'--threads',str(threads),
                 '--parallel-depth','1','--relax-proof-threads',str(threads),
                 '--closure-threads',str(threads),'--time-limit','2',
                 '--progress-interval','0.05','--progress-file',str(out),
                 '--profile','1','--json','--quiet']
            proc=subprocess.run(cmd,text=True,capture_output=True,timeout=20)
            self.assertEqual(proc.returncode,0,proc.stderr[-500:])
            r=json.loads(proc.stdout)
            self.assertEqual((r['status'],r['upper_bound'],r['lower_bound']),('OPTIMAL',5,5))
            total=sum(r[k] for k in ('nodes_customer','nodes_relaxed_proofs','nodes_close_stack','nodes_frontier'))
            self.assertEqual(total,r['nodes'])
            self.assertGreaterEqual(r['cpu_seconds'],0)
            self.assertGreater(r['peak_rss_mb'],0)
            for kind in ('customer','relaxed','closure'):
                for w in r['workers_'+kind]:
                    self.assertGreaterEqual(w['available_seconds']+1e-6,w['active_seconds'])
                    self.assertGreaterEqual(w['tasks'],0)
            with out.open(newline='') as f:
                points=list(csv.DictReader(f))
            self.assertGreaterEqual(len(points),2)
            self.assertTrue(all(float(p['gap_percent'])>=0 for p in points))
            self.assertTrue(all(float(points[i+1]['time_seconds'])>=float(points[i]['time_seconds']) for i in range(len(points)-1)))
            self.assertEqual(int(points[-1]['nodes_total']),r['nodes'])
            self.assertEqual(int(points[-1]['lower_bound']),r['lower_bound'])
            self.assertEqual(int(points[-1]['upper_bound']),r['upper_bound'])
            return r

    def test_parallel_customer(self): self.run_case('off','customer',2)
    def test_parallel_close_stack(self): self.run_case('off','close-stack',2)
    def test_parallel_relaxed_proofs(self): self.run_case('full','customer',2)
    def test_serial_without_monitor(self):
        p=subprocess.run([str(BIN),'--input',str(INSTANCE),'--json','--quiet','--threads','1'],
                         capture_output=True,text=True,timeout=20)
        self.assertEqual(p.returncode,0,p.stderr)
        r=json.loads(p.stdout)
        self.assertEqual(r['upper_bound'],5)
        self.assertEqual(r['nodes'],sum(r[k] for k in ('nodes_customer','nodes_relaxed_proofs','nodes_close_stack','nodes_frontier')))
    def test_requires_path(self):
        p=subprocess.run([str(BIN),'--input',str(INSTANCE),'--progress-interval','0.1'],
                         capture_output=True,text=True,timeout=20)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('requires --progress-file',p.stderr)
if __name__=='__main__': unittest.main()
