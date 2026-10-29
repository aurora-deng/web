"""Capture a recoverable public-source baseline and enforce the communication freeze.

No dependency downloads, Git mutations, runtime control or database access.
Credentials/private data are excluded even if accidentally present in the index.
"""
import argparse
import fnmatch
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT=Path(__file__).resolve().parents[2]
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def names():
    raw=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT)
    for name in sorted(set(raw.decode().split('\0'))):
        if not name or not (ROOT/name).is_file():continue
        parts=Path(name).parts
        # Filter generated directories, not legitimate files such as build_stamp.py.
        # .github/.gitignore are public project configuration, not .git metadata.
        if parts[0] in ('runtime-data','client-data','models'):continue
        if any(p.startswith(('build','artifacts')) or p in ('.git','.aws','.codex') for p in parts[:-1]):continue
        if parts[-1].startswith('.env') and parts[-1]!='.env.example':continue
        if any(fnmatch.fnmatch(name.lower(),p) for p in ['*.local.*','*.key','*.dump','*.sqlite*','*.db','*.env','*.pem']):continue
        yield name
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['capture','check']);p.add_argument('--directory',type=Path,default=ROOT/'build-function3/phase43-baseline');a=p.parse_args()
    target=a.directory.resolve()
    if not target.is_relative_to(ROOT/'build-function3'):raise SystemExit('Baseline must stay in the ignored workspace build directory')
    manifest=target/'source.json'
    if a.action=='capture':
        if manifest.exists():raise SystemExit('Baseline already exists; never overwrite it')
        target.mkdir(parents=True,exist_ok=True)
        files={name:digest(ROOT/name) for name in names()}
        with tarfile.open(target/'public-source.tar.gz','w:gz') as archive:
            for name in files:archive.add(ROOT/name,arcname=name,recursive=False)
        value={'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),'files':files,
               'communication':{k:v for k,v in files.items() if k.startswith('backend/server/')}}
        manifest.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
        print(json.dumps({'sourceFiles':len(files),'frozenCoreFiles':len(value['communication']),'directory':str(target)}))
    else:
        value=json.loads(manifest.read_text(encoding='utf-8'));actual={p.relative_to(ROOT).as_posix():digest(p) for p in (ROOT/'backend/server').rglob('*') if p.is_file()}
        expected=value['communication'];changed=[name for name in sorted(set(actual)|set(expected)) if actual.get(name)!=expected.get(name)]
        if changed:raise SystemExit('Communication core changed: '+', '.join(changed))
        print(json.dumps({'communicationCoreUnchanged':True,'files':len(actual)}))
if __name__=='__main__':main()
