"""Explicitly approved, user-local Prometheus/Node Exporter install; no service changes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path('/home/pikachu/phase11-deps/observability')
BUDGET = 600 * 1024**2
PACKAGES = (
    ('prometheus', 'prometheus', '3.13.5', ('prometheus', 'promtool')),
    ('prometheus', 'node_exporter', '1.12.1', ('node_exporter',)),
)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024**2), b''):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(url, path, limit):
    temporary = path.with_name(path.name+'.partial')
    try:
        with urllib.request.urlopen(url, timeout=40) as response, temporary.open('wb') as out:
            total = 0
            while chunk := response.read(min(1024**2, limit-total+1)):
                total += len(chunk)
                if total > limit:
                    raise RuntimeError('Approved download budget exceeded')
                out.write(chunk)
        temporary.replace(path)
        return total
    finally:
        temporary.unlink(missing_ok=True)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--approved-group', choices=['prometheus-3.13.5-node-1.12.1'], required=True)
    cli.add_argument('--offline-cache', action='store_true',help='Use the official, separately verified transfer cache')
    args = cli.parse_args()
    if platform.system()!='Linux' or platform.machine()!='x86_64' or os.geteuid()==0:
        cli.error('Use the approved unprivileged Linux x64 environment')
    ROOT.mkdir(parents=True, exist_ok=True)
    if ROOT.resolve()!=ROOT or shutil.disk_usage(ROOT).free < BUDGET + 512*1024**2:
        raise RuntimeError('Dependency path or available disk differs from approval')
    cache = ROOT/'downloads';cache.mkdir(exist_ok=True)
    report = []
    for owner, name, version, programs in PACKAGES:
        folder = ROOT/f'{name}-{version}'
        archive = cache/f'{name}-{version}.linux-amd64.tar.gz'
        release = f'https://github.com/{owner}/{name}/releases/download/v{version}/'
        sums = cache/f'{name}-{version}-sha256sums.txt'
        if args.offline_cache:
            entries=json.loads((cache/'transfer-manifest.json').read_text())
            entry=next(row for row in entries if row['name']==name and row['version']==version)
            if (entry['source']!=f'https://api.github.com/repos/{owner}/{name}/releases/tags/v{version}'
                    or sha(sums)!=entry['checksumsSha256']):
                raise RuntimeError('Official transfer checksum provenance differs')
        else:
            fetch(release+'sha256sums.txt', sums, 256*1024)
        candidates = {parts[1].lstrip('*'):parts[0] for line in sums.read_text().splitlines()
                      if len(parts:=line.split())==2 and len(parts[0])==64}
        expected = candidates.get(archive.name)
        if expected is None:
            raise RuntimeError('Official checksum list does not contain the approved asset')
        if not archive.is_file() or sha(archive)!=expected:
            if args.offline_cache:
                raise RuntimeError('Transferred asset missing or changed')
            fetch(release+archive.name, archive, BUDGET//2)
        if sha(archive)!=expected:
            raise RuntimeError('Official asset SHA-256 mismatch')
        folder.mkdir(exist_ok=True)
        with tarfile.open(archive) as bundle:
            prefix = f'{name}-{version}.linux-amd64/'
            for filename in (*programs, 'LICENSE', 'NOTICE'):
                try:
                    member = bundle.getmember(prefix+filename)
                except KeyError:
                    if filename=='NOTICE':
                        continue
                    raise RuntimeError('Approved package is missing a required member')
                if not member.isfile() or not 0<member.size<=BUDGET//2:
                    raise RuntimeError('Unexpected executable/archive member')
                destination = folder/filename
                with bundle.extractfile(member) as source, destination.open('wb') as output:
                    shutil.copyfileobj(source, output, 1024**2)
                destination.chmod(0o755 if filename in programs else 0o644)
        for executable in programs:
            result = subprocess.run([str(folder/executable), '--version'], capture_output=True, timeout=10)
            if result.returncode or version not in (result.stdout+result.stderr).decode(errors='replace'):
                raise RuntimeError('Installed version check failed')
        report.append({'name':name,'version':version,'license':'Apache-2.0','directory':str(folder),
                       'archiveBytes':archive.stat().st_size,'archiveSha256':expected,
                       'officialChecksums':release+'sha256sums.txt',
                       'programs':{p:sha(folder/p) for p in programs}})
        print(json.dumps({'installed':name,'version':version,'sha256Verified':True}),flush=True)
    used = sum(p.stat().st_size for p in ROOT.rglob('*') if p.is_file())
    if used>BUDGET:
        raise RuntimeError('Approved installed/download budget exceeded')
    (ROOT/'metrics-install-manifest.json').write_text(json.dumps({'packages':report,'usedBytes':used,
        'servicesInstalled':False,'globalEnvironmentChanged':False},indent=2)+'\n')


if __name__=='__main__':
    main()
