"""Allowlisted Phase35 results only; never copies passwords, cookies, dumps or audio."""
from pathlib import Path
import hashlib
import io
import json
import tarfile

source=Path('/home/pikachu/function3-src')
root=Path('/home/pikachu/phase11-data/function3/validation/phase35')
files={}
for name in ('release-ctest.log','local-delivery.log','shared-runtime.json',
             'read-cache-benchmark.json','read-cache-first-attempt.json'):
    file=root/name
    if not file.is_file():raise RuntimeError('Missing final evidence: '+name)
    files[name.replace('.log','.txt')]=file.read_bytes()
for flavour in ('debug','asan'):
    for name in ('summary.json','ctest.log'):
        file=root/'checks'/flavour/name
        if not file.is_file():raise RuntimeError('Missing '+str(file))
        if name=='summary.json' and not json.loads(file.read_text())['passed']:
            raise RuntimeError('Build/check has not passed: '+flavour)
        files[flavour+'-'+name.replace('.log','.txt')]=file.read_bytes()
# Contains account count and combined digest, never individual password hashes.
files['migration-report.json']=(Path('/home/pikachu/phase11-data/function3/runtime/migration-report.json')).read_bytes()
paths=[]
for directory in ('server','frontend','scripts/function3','tests/function3','benchmarks'):
    paths += [p for p in (source/directory).rglob('*') if p.is_file()
              and p.suffix in ('.cpp','.h','.js','.cjs','.mjs','.css','.html','.py','.sql','.txt')
              and '__pycache__' not in p.parts]
paths += [source/'main.cpp',source/'CMakeLists.txt']
digest=hashlib.sha256()
for p in sorted(set(paths)):
    digest.update(p.relative_to(source).as_posix().encode()+b'\0'+p.read_bytes()+b'\0')
manifest={'sourceFiles':len(set(paths)),'sourceSha256':digest.hexdigest(),
          'scope':'Phase35 daily phase11 read-only acceptance + existing isolated test DB fixtures; no remote push',
          'evidence':{name:hashlib.sha256(body).hexdigest() for name,body in sorted(files.items())}}
files['MANIFEST.json']=(json.dumps(manifest,indent=2)+'\n').encode()
output=source/'function3-phase35-evidence.tar.gz'
with tarfile.open(output,'w:gz') as archive:
    for name,body in sorted(files.items()):
        info=tarfile.TarInfo(name);info.size=len(body);info.mode=0o644
        archive.addfile(info,io.BytesIO(body))
print(json.dumps({'archive':str(output),'files':len(files),'sourceSha256':manifest['sourceSha256']}))
