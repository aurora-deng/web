"""Refresh only changed regular source files, preserving build caches and runtime data."""
from pathlib import Path
import tarfile

root=Path('/home/pikachu/function3-src')
count=0
with tarfile.open('/home/pikachu/function3-source.tar.gz') as archive:
    for member in archive.getmembers():
        path=Path(member.name)
        if path.is_absolute() or '..' in path.parts or not member.isfile():
            raise RuntimeError('unsafe archive entry')
        output=root/path
        data=archive.extractfile(member).read()
        if output.exists() and output.read_bytes()==data:continue
        output.parent.mkdir(parents=True,exist_ok=True)
        output.write_bytes(data)
        count+=1
print('Changed source files:',count)
