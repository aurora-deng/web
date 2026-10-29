"""Safely unpack an already approved, publisher-SHA-verified Kokoro archive. No network."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--archive',type=Path,required=True)
a=p.parse_args()
expected='a1e94694776049035c4f2c6529f003aaece993c76aae9a78995831c3c4dcafc6'
assert hashlib.sha256(a.archive.read_bytes()).hexdigest()==expected,'unapproved Kokoro archive'
root=Path('/home/pikachu/phase11-models/kokoro-v1.1-zh');root.mkdir(parents=True,exist_ok=True)
lines=[]
with tarfile.open(a.archive) as archive:
    members=archive.getmembers()
    assert shutil.disk_usage(root).free>sum(m.size for m in members)+256*1024**2,'insufficient approved disk reserve'
    for member in members:
        parts=Path(member.name).parts[1:]
        if not parts:continue
        relative=Path(*parts)
        assert not relative.is_absolute() and '..' not in parts
        output=root/relative
        if member.isdir():output.mkdir(parents=True,exist_ok=True);continue
        assert member.isfile(),'model links not accepted'
        data=archive.extractfile(member).read();digest=hashlib.sha256(data).hexdigest()
        output.parent.mkdir(parents=True,exist_ok=True)
        if not output.exists() or hashlib.sha256(output.read_bytes()).hexdigest()!=digest:output.write_bytes(data)
        assert '\n' not in relative.as_posix() and '\r' not in relative.as_posix()
        lines.append(digest+'  '+relative.as_posix())
(root/'MODEL_FILES.sha256').write_text('\n'.join(sorted(lines))+'\n',encoding='ascii')
(root/'SOURCE.sha256').write_text(expected+'\n',encoding='ascii')
print('Verified Kokoro model and',len(lines),'resources; existing identical files reused.')
