"""Allowlisted numeric/check results only; excludes keys, cookies, dumps, logs with request bodies and audio."""
from pathlib import Path
import hashlib
import io
import json
import tarfile

SOURCE=Path('/home/pikachu/function3-src');ROOT=Path('/home/pikachu/phase11-data/function3/validation')
files={}
def add(name,path):
    if not path.is_file():raise RuntimeError('Missing required evidence '+name)
    if path.stat().st_size>2*1024*1024:raise RuntimeError('Unexpected evidence size '+name)
    files[name]=path.read_bytes()
add('release-ctest.txt',Path('/home/pikachu/phase40-ctest.log'))
for flavour in ('debug','asan'):
    folder=ROOT/'phase41/checks'/flavour
    assert json.loads((folder/'summary.json').read_text())['passed']
    for name in ('summary.json','ctest.log'):add(flavour+'-'+name.replace('.log','.txt'),folder/name)
for name in ('worker.json','migration022.json','group-ai.json'):add(name,ROOT/'phase41'/name)
add('activities.json',ROOT/'phase37/activities.json');add('private-archive.json',ROOT/'phase39/archive.json');add('speech.json',ROOT/'phase40/speech.json')
for directory in ('benchmarks','diagnostic-on','diagnostic-off'):
    folder=ROOT/'phase41'/directory;add(directory+'/summary.json',folder/'summary.json')
    for file in sorted(folder.glob('*-mixed.json')):add(directory+'/'+file.name,file)
    for file in sorted(folder.glob('*-fast.txt')):add(directory+'/'+file.name,file)
fingerprints={}
for name,folder in [('backend','backend'),('client','client'),('frontend','frontend'),('validation','scripts/function3/validation'),('benchmarks','benchmarks'),('tests','tests')]:
    digest=hashlib.sha256();count=0
    for file in sorted((SOURCE/folder).rglob('*')):
        if not file.is_file() or '__pycache__' in file.parts:continue
        if file.suffix not in ('.cpp','.h','.ipp','.js','.css','.html','.py','.sql') and file.name!='CMakeLists.txt':continue
        relative=file.relative_to(SOURCE).as_posix();digest.update(relative.encode()+b'\0'+file.read_bytes()+b'\0');count+=1
    fingerprints[name]={'files':count,'sha256':digest.hexdigest()}
files['source-fingerprints.json']=(json.dumps(fingerprints,indent=2)+'\n').encode()
binaries={}
for flavour in ('release','debug','asan'):
    binary=SOURCE/('build-'+flavour)/'webserver'
    binaries[flavour]={'bytes':binary.stat().st_size,'sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
files['binary-fingerprints.json']=(json.dumps(binaries,indent=2)+'\n').encode()
files['manifest.json']=(json.dumps({name:hashlib.sha256(data).hexdigest() for name,data in sorted(files.items())},indent=2)+'\n').encode()
output=Path('/home/pikachu/function3-phase41-evidence.tar.gz')
with tarfile.open(output,'w:gz')as archive:
    for name,data in sorted(files.items()):
        item=tarfile.TarInfo(name);item.size=len(data);item.mode=0o644;archive.addfile(item,io.BytesIO(data))
print('Evidence package',output,'files',len(files),'sha256',hashlib.sha256(output.read_bytes()).hexdigest())
