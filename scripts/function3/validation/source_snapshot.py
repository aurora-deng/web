"""Package tracked and non-ignored source for an isolated Linux build; no secrets/builds."""
from pathlib import Path
import subprocess
import tarfile

root = Path(__file__).resolve().parents[3]
output = root / 'build-function3/source.tar.gz'
output.parent.mkdir(exist_ok=True)
names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=root).decode().split('\0')
with tarfile.open(output, 'w:gz') as archive:
    for name in sorted(set(names)):
        if name and (root/name).is_file():
            archive.add(root/name, arcname=name, recursive=False)
print(output)
