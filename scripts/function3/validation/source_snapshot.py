"""Package tracked and non-ignored source for an isolated Linux build; no secrets/builds."""
from pathlib import Path
import subprocess
import tarfile
import sys
import io
import json
import hashlib

root = Path(__file__).resolve().parents[3]
output = root / 'build-function3/source.tar.gz'
output.parent.mkdir(exist_ok=True)
# Reuse the audited public inventory: do not ship local data or verification outputs.
sys.path.insert(0,str(root/'scripts/project'))
from source_guard import names as public_source_names
names = list(public_source_names())
with tarfile.open(output, 'w:gz') as archive:
    for name in sorted(set(names)):
        if name and (root/name).is_file():
            archive.add(root/name, arcname=name, recursive=False)
    inventory={'schema':1,'files':{name:hashlib.sha256((root/name).read_bytes()).hexdigest() for name in names}}
    data=json.dumps(inventory,sort_keys=True).encode()
    member=tarfile.TarInfo('.source-snapshot-manifest.json');member.size=len(data)
    archive.addfile(member,io.BytesIO(data))
print(output)
