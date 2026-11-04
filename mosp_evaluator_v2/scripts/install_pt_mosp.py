#!/usr/bin/env python3
"""Install the optional adapter into a verified PT-MOSP source tree."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

package=Path(__file__).resolve().parents[1]
adapter=package/'adapters/pt_mosp'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo',type=Path,required=True)
parser.add_argument('--check',action='store_true',help='verify compatibility without writing')
args=parser.parse_args()
repo=args.repo.resolve()
manifest=json.loads((adapter/'base_manifest.json').read_text())
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
actions=[]
for entry in manifest['files']:
    relative=Path(entry['path']);target=(repo/relative).resolve()
    if repo not in target.parents:raise SystemExit('Invalid target path')
    source=adapter/'overlay'/relative
    if digest(source)!=entry['patched_sha256']:raise SystemExit(f'Corrupt adapter file: {relative}')
    current=digest(target) if target.is_file() else None
    if current==entry['patched_sha256']:continue
    if current!=entry['original_sha256']:
        raise SystemExit(f'Base differs at {relative}; no files were changed. Review the patch against your version.')
    actions.append((source,target,relative,current))
for folder in ['src','include']:
    for source in sorted((package/folder).rglob('*')):
        if not source.is_file():continue
        relative=Path('MOSP/mosp_evaluator')/source.relative_to(package)
        target=(repo/relative).resolve()
        if repo not in target.parents:raise SystemExit('Invalid vendor target')
        if target.exists() and digest(target)!=digest(source):
            raise SystemExit(f'Existing library differs at {relative}; no files were changed.')
        if not target.exists():actions.append((source,target,relative,None))
for name in ['LICENSE','VERSION']:
    source=package/name;relative=Path('MOSP/mosp_evaluator')/name;target=(repo/relative).resolve()
    if target.exists() and digest(target)!=digest(source):raise SystemExit(f'Existing library differs: {relative}')
    if not target.exists():actions.append((source,target,relative,None))
if not (repo/'MOSP/MCNH/LinkedList.h').is_file():raise SystemExit('Missing original MCNH/LinkedList.h')
if args.check:
    print(json.dumps({'status':'COMPATIBLE','files_to_write':len(actions)}));sys.exit(0)
backup=repo/'mosp_evaluator_backup'
for source,target,relative,current in actions:
    if current is not None:
        saved=backup/relative
        if saved.exists() and digest(saved)!=current:raise SystemExit(f'Existing backup differs: {relative}')
# All base and backup checks precede mutation.
for source,target,relative,current in actions:
    if current is not None:
        saved=backup/relative;saved.parent.mkdir(parents=True,exist_ok=True)
        if not saved.exists():shutil.copyfile(target,saved)
    target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source,target)
print(json.dumps({'status':'INSTALLED','files_written':len(actions),'version':'2.0.0'}))
