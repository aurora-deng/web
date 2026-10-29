"""Collect only allowlisted test evidence. Never includes tokens, keys, models or user uploads."""
from pathlib import Path
import hashlib
import json
import tarfile

source=Path('/home/pikachu/function3-src')
root=Path('/home/pikachu/phase11-data/function3/validation')
files={}
for name in ('release-final-ctest.log','capacity-final.json','mq-final.log','business-final.log',
             'ai-mixed.json','ai-final.log','debug-final-core.log','asan-final-core.log',
             'retained-binaries.json','task-cache-cleanup.json','wal-diagnostic.json','baseline-wal-mixed.json','current-wal-mixed.json'):
    p=root/name
    if p.is_file():files[name.replace('.log','.txt')]=p.read_bytes()
if (source/'wal-diagnostic.log').is_file():files['wal-diagnostic.txt']=(source/'wal-diagnostic.log').read_bytes()
for flavour in ('debug','asan'):
    for name in ('summary.json','ctest.log'):
        p=root/'checks'/flavour/name
        if p.is_file():files[flavour+'-'+name.replace('.log','.txt')]=p.read_bytes()
for run in ('benchmarks','benchmarks-before-atomic-handoff','benchmarks-before-delivery-isolation','benchmarks-before-batching','benchmarks-before-publisher-wakeup'):
    p=root/run
    if p.is_dir():
        for file in p.iterdir():
            if file.is_file() and file.suffix in ('.json','.txt'):files[run+'/'+file.name]=file.read_bytes()
# Fingerprint exactly the relevant current source, excluding runtime/build outputs.
paths=[]
for directory in ('server','frontend','scripts/function3','tests/function3','benchmarks'):
    for p in (source/directory).rglob('*'):
        if p.is_file() and p.suffix in ('.cpp','.h','.js','.cjs','.mjs','.css','.html','.py','.sql','.txt') and '__pycache__' not in p.parts:paths.append(p)
paths += [source/'main.cpp',source/'CMakeLists.txt']
digest=hashlib.sha256()
for p in sorted(set(paths)):
    digest.update(p.relative_to(source).as_posix().encode()+b'\0'+p.read_bytes()+b'\0')
manifest={'baselineCommit':'ffc0e141840109231784c7bc66c6d4494f65185f','sourceFiles':len(set(paths)),
          'sourceSha256':digest.hexdigest(),'evidence':{name:hashlib.sha256(body).hexdigest() for name,body in sorted(files.items())},
          'scope':'isolated Function3 tests; no formal deployment or remote push'}
files['MANIFEST.json']=(json.dumps(manifest,indent=2)+'\n').encode()
output=source/'function3-evidence.tar.gz'
import io
with tarfile.open(output,'w:gz') as archive:
    for name,body in sorted(files.items()):
        info=tarfile.TarInfo(name);info.size=len(body);info.mode=0o644
        archive.addfile(info,io.BytesIO(body))
print(json.dumps({'archive':str(output),'files':len(files),'bytes':output.stat().st_size,'sourceSha256':manifest['sourceSha256']}))
