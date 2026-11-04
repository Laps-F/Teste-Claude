#!/usr/bin/env python3
import csv
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import shlex
root=Path(__file__).resolve().parents[1];os.chdir(root)
subprocess.run(['make','-B','build/auto-benchmark'],check=True)
results=root/'results';results.mkdir(exist_ok=True)
with (results/'auto_benchmark_raw.csv').open('w') as f:subprocess.run([str(root/'build/auto-benchmark')],stdout=f,check=True)
rows=list(csv.DictReader((results/'auto_benchmark_raw.csv').open()))
keys=['patterns','items','density','input','move','distance','accept_fraction','samples','backend'];groups={}
for row in rows:groups.setdefault(tuple(row[k] for k in keys),[]).append(row)
with (results/'auto_benchmark_medians.csv').open('w',newline='') as f:
    writer=csv.DictWriter(f,fieldnames=keys+['repetitions','median_seconds','us_per_proposal','calibration_seconds','full_calls','incremental_calls','sum_mosp','sum_profile','sum_energy'])
    writer.writeheader()
    for key,samples in groups.items():
        entry=dict(zip(keys,key));median=statistics.median(float(s['seconds']) for s in samples)
        for column in ['sum_mosp','sum_profile','sum_energy']:
            assert len({s[column] for s in samples})==1
        entry.update(repetitions=len(samples),median_seconds=median,us_per_proposal=median*1e6/int(entry['samples']))
        for column in ['calibration_seconds','full_calls','incremental_calls','sum_mosp','sum_profile','sum_energy']:entry[column]=samples[0][column]
        writer.writerow(entry)
cpu='unknown'
if Path('/proc/cpuinfo').exists():
    cpu=next((s.split(':',1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')),cpu)
env={'date':'2026-10-29','platform':platform.platform(),'cpu':cpu,'compiler':subprocess.check_output(shlex.split(os.environ.get('CXX','g++'))+['--version'],text=True).splitlines()[0],
     'flags':os.environ.get('CXXFLAGS','-O3 -std=c++17 -Wall -Wextra -Wpedantic'),'threads_per_timed_run':1,'matrix_seed':20261030,'repetitions':3,
     'source':'synthetic','acceptance':'prescribed exactly 50%; independent of energy','raw_rows':len(rows),'median_rows':len(groups)}
(results/'environment_v2.json').write_text(json.dumps(env,indent=2)+'\n')
print(json.dumps({'status':'PASS','raw_rows':len(rows),'median_rows':len(groups)}))
