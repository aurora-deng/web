"""Verify/reproduce the approved private SDK changes using existing sources only.

No download, FetchContent, global environment changes, or automatic dependency install.
The complete patch includes the FIFO policy; source hashes prevent accidental patching
of another SDK version. Rebuilding is explicit, not part of project configuration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--rebuild',action='store_true')
a=p.parse_args()
root=Path('/home/pikachu/phase11-deps/rocketmq-cpp/5.1.1')
source=root/'rocketmq-clients'
directory=Path(__file__).parent/'patches/SdkCompatibility'
manifest=json.loads((directory/'rocketmq-sdk-compat.json').read_text())
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
states=[]
for entry in manifest:
    target=source/entry['path']
    assert target.resolve().is_relative_to(source.resolve())
    actual=sha(target)
    if actual==entry['installedSourceSha256']:states.append('patched')
    elif actual==entry['upstreamSha256']:states.append('upstream')
    else:raise RuntimeError('Different SDK source; preserved without modification: '+str(target))
if set(states)=={'upstream'}:
    patch=(directory/'rocketmq-sdk-compat.patch').resolve()
    subprocess.run(['git','apply','--check',str(patch)],cwd=source,check=True)
    subprocess.run(['git','apply',str(patch)],cwd=source,check=True)
elif set(states)!={'patched'}:
    raise RuntimeError('Mixed SDK source states; inspect before proceeding')
for entry in manifest:assert sha(source/entry['path'])==entry['installedSourceSha256']
# Protocol definitions come from the separately approved, already present source.
for name in ('admin.proto','definition.proto','service.proto'):
    target=source/'cpp/proto/apache/rocketmq/v2'/name
    original=root/'rocketmq-apis/apache/rocketmq/v2'/name
    target.parent.mkdir(parents=True,exist_ok=True)
    if target.exists() and target.read_bytes()!=original.read_bytes():raise RuntimeError('Different protocol source preserved')
    if not target.exists():shutil.copy2(original,target)
print('Existing SDK source, explicit completion policy and protocols verified.')
if a.rebuild:
    subprocess.run(['cmake','-S',str(source/'cpp'),'-B',str(root/'build'),
        '-DBUILD_EXAMPLES=OFF','-DBUILD_TESTS=OFF',
        '-DCMAKE_PREFIX_PATH=/home/pikachu/.local-grpc-systemssl',
        '-DCMAKE_INSTALL_PREFIX='+str(root/'install')],check=True)
    subprocess.run(['python3',str(Path(__file__).with_name('bound_sdk_fifo.py'))],check=True)
