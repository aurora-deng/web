"""Refresh source; prune only unchanged files owned by the previous snapshot.

Unknown files, build caches and user-modified stale files are preserved.
"""
from pathlib import Path
import hashlib
import json
import tarfile

ROOT=Path('/home/pikachu/function3-src')
ARCHIVE=Path('/home/pikachu/function3-source.tar.gz')
STATE='.source-snapshot-state.json'
MANIFEST='.source-snapshot-manifest.json'

def source_path(root,name):
    path=Path(name)
    if path.is_absolute() or '..' in path.parts or not path.parts:raise RuntimeError('unsafe source path')
    target=root/path
    if not target.resolve().is_relative_to(root.resolve()):raise RuntimeError('source path escaped workspace')
    return target

def removed_files(root,previous,current):
    removable=[];preserved=[]
    for name,expected in previous.items():
        if name in current:continue
        target=source_path(root,name)
        if not target.exists():continue
        if target.is_symlink() or not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest()!=expected:
            preserved.append(name)
        else:removable.append(target)
    return removable,preserved

def main():
    count=0;state=ROOT/STATE
    previous=json.loads(state.read_text())['files'] if state.exists() else {}
    with tarfile.open(ARCHIVE) as archive:
        metadata=json.load(archive.extractfile(MANIFEST));assert metadata['schema']==1
        current=metadata['files'];members={member.name:member for member in archive.getmembers()}
        if set(members)!=(set(current)|{MANIFEST}):raise RuntimeError('source inventory differs from archive')
        removable,preserved=removed_files(ROOT,previous,current)
        for name,expected in current.items():
            source_path(ROOT,name);member=members[name]
            if not member.isfile():raise RuntimeError('non-regular source member')
            if hashlib.sha256(archive.extractfile(member).read()).hexdigest()!=expected:raise RuntimeError('source checksum mismatch')
        for name in current:
            output=source_path(ROOT,name);data=archive.extractfile(members[name]).read()
            if output.exists() and output.read_bytes()==data:continue
            output.parent.mkdir(parents=True,exist_ok=True);output.write_bytes(data);count+=1
        for target in removable:target.unlink()
        state.write_text(json.dumps(metadata,sort_keys=True)+'\n')
    print(json.dumps({'changed':count,'removedOwnedFiles':len(removable),'preservedModifiedStaleFiles':preserved}))

if __name__=='__main__':main()
