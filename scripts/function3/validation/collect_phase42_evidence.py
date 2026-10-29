"""Collect allowlisted result files only; never packages DB/media/keys/model data."""
import hashlib
import json
from pathlib import Path
import tarfile

source=Path('/home/pikachu/function3-src')
root=Path('/home/pikachu/phase11-data/function3/validation/phase42')
allowed=[]
for name in ('release-ctest.txt','activities.json','tree-capacity.json','tree-capacity.err','capacity-initial-attempt.json','asset-isolation.json','scope.json'):
    file=root/name
    if file.is_file():allowed.append(file)
for folder in ('benchmarks','benchmarks-build-overlap'):
    allowed += [p for p in (root/folder).glob('*') if p.is_file() and (p.suffix=='.json' or p.name.endswith('-fast.txt') or p.name=='NOTE.txt')]
for flavour in ('debug','asan'):
    allowed += [p for p in (root/'checks'/flavour).glob('*') if p.is_file() and p.name in ('summary.json','ctest.log')]

def digest(file):return hashlib.sha256(file.read_bytes()).hexdigest()
binaries={'before':root/'webserver-before','current':source/'build-release/webserver'}
(root/'binary-fingerprints.json').write_text(json.dumps({k:digest(v) for k,v in binaries.items()},indent=2)+'\n')
groups={}
for group in ('backend','client','frontend','tests/function3','tests/client','scripts/function3/validation','benchmarks'):
    files=sorted((p for p in (source/group).rglob('*') if p.is_file() and '__pycache__' not in p.parts),key=lambda p:p.relative_to(source).parts)
    combined=hashlib.sha256()
    for p in files:combined.update(p.relative_to(source).as_posix().encode()+b'\0'+p.read_bytes()+b'\0')
    groups[group]={'files':len(files),'sha256':combined.hexdigest()}
(root/'source-fingerprints.json').write_text(json.dumps(groups,indent=2)+'\n')
allowed += [root/'binary-fingerprints.json',root/'source-fingerprints.json']
manifest={str(p.relative_to(root)):{'sha256':digest(p),'bytes':p.stat().st_size} for p in sorted(allowed)}
(root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');allowed.append(root/'manifest.json')
destination=Path('/home/pikachu/function3-phase42-evidence.tar.gz')
with tarfile.open(destination,'w:gz') as archive:
    for p in allowed:archive.add(p,arcname=str(p.relative_to(root)),recursive=False)
print(json.dumps({'archive':str(destination),'files':len(allowed),'sha256':digest(destination)}))
