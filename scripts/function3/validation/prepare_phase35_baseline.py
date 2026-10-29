"""Build the exact pre-Phase36 dirty-source snapshot in a separate marked directory."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tarfile

SOURCE=Path('/home/pikachu/function3-src');DEST=Path('/home/pikachu/function3-phase35-baseline-src')
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--archive',type=Path,required=True);p.add_argument('--sha256',required=True);a=p.parse_args()
digest=hashlib.sha256(a.archive.read_bytes()).hexdigest()
if digest!=a.sha256:raise RuntimeError('Baseline archive checksum mismatch')
marker=DEST/'PHASE35_BASELINE_SOURCE'
if DEST.exists() and (not marker.is_file() or marker.read_text().strip()!=digest):raise RuntimeError('Refusing unrelated or different baseline directory')
DEST.mkdir(exist_ok=True);marker.write_text(digest+'\n')
with tarfile.open(a.archive) as archive:
    members=archive.getmembers()
    if len(members)>4096 or sum(m.size for m in members)>100*1024*1024:raise RuntimeError('Unexpected source snapshot size')
    for member in members:
        path=Path(member.name)
        if not member.isfile() or path.is_absolute() or '..' in path.parts or '.git' in path.parts:raise RuntimeError('Unsafe source member')
        target=DEST/path;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(archive.extractfile(member).read())
args=['cmake','-S',str(DEST),'-B',str(DEST/'build-release'),'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF']
for line in (SOURCE/'build-release/CMakeCache.txt').read_text().splitlines():
    if not line.startswith(('WEBSERVER_','CMAKE_PREFIX_PATH:')) or '=' not in line:continue
    key,value=line.split('=',1)
    if ':INTERNAL' in key or key.split(':')[0] in ('WEBSERVER_ENABLE_ASAN','WEBSERVER_ENABLE_UBSAN','WEBSERVER_ENABLE_TSAN'):continue
    args.append('-D'+key+'='+value)
args+=['-DWEBSERVER_ENABLE_ASAN=OFF','-DWEBSERVER_ENABLE_UBSAN=OFF','-DWEBSERVER_ENABLE_TSAN=OFF']
subprocess.run(args,check=True);subprocess.run(['cmake','--build',str(DEST/'build-release'),'--target','webserver','-j2'],check=True)
# Only advance the previously existing named baseline test database, never create a new one.
pg='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
connection='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_baseline_test'
def sql(query):return subprocess.check_output([pg,connection,'-w','-qAtc',query],text=True).strip()
assert sql('SELECT current_database()')=='function3_baseline_test'
applied=set(sql('SELECT version FROM phase11_schema_migrations').splitlines())
for file in sorted((DEST/'server/phase11/storage/postgres/migrations').glob('*.sql')):
    version=int(file.name.split('_')[0]);assert version<=18
    if str(version) not in applied:subprocess.run([pg,connection,'-w','-v','ON_ERROR_STOP=1','-f',str(file)],check=True)
print('Prepared exact Phase35 baseline',digest,'with existing installed dependencies and existing test database')
