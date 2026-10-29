"""Package tracked and non-ignored source for an isolated Linux build; no secrets/builds."""
from pathlib import Path
import subprocess
import tarfile
import sys

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
print(output)
