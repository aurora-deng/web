"""Seven-day expiry for explicitly marked, inactive monitoring labs only.

Never traverses a database cluster, source/build tree or exported evidence.
The owner marks its temporary telemetry at creation; unmarked paths are ignored.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import time

MARKER='temporary-telemetry.json'
FILES=('events.sqlite','events.sqlite-wal','events.sqlite-shm','nebula.log','observer.log',
       'probe.log','prometheus.log','node.log','collector.log','tempo.log','soak.ndjson')
DIRECTORIES=('prometheus','tempo','structured-logs','gateway')


def mark(run,now=None):
    (Path(run)/MARKER).write_text(json.dumps({'schemaVersion':1,'kind':'nebula-owned-telemetry',
        'createdAt':time.time() if now is None else now,'retentionDays':7}),encoding='utf-8')


def active(run):
    try:
        pid=int((run/'lab.pid').read_text())
        if os.name=='nt':return True # Cleanup of Linux-owned labs is Linux-only.
        command=Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
        return b'tools.observability.lab' in command and str(run).encode() in command
    except FileNotFoundError:return False
    except (OSError,ValueError):return True # Uncertain ownership: preserve.


def prune(root,now=None,apply=False):
    root=Path(root).resolve();now=time.time() if now is None else now;items=[]
    for entry in root.iterdir():
        if entry.is_symlink() or not entry.is_dir():continue
        marker=entry/MARKER
        if not marker.is_file() or marker.is_symlink() or marker.stat().st_size>1024:continue
        try:metadata=json.loads(marker.read_text(encoding='utf-8'))
        except (OSError,ValueError):continue
        if (metadata.get('schemaVersion')!=1 or metadata.get('kind')!='nebula-owned-telemetry'
                or metadata.get('retentionDays')!=7 or type(metadata.get('createdAt')) not in (int,float)
                or not metadata['createdAt']<now-7*86400 or active(entry)):continue
        for name in (*FILES,*DIRECTORIES):
            path=entry/name
            if not path.exists() or path.is_symlink():continue
            resolved=path.resolve()
            if not resolved.is_relative_to(entry.resolve()) or entry.resolve().parent!=root:continue
            if path.is_dir() and any(p.is_symlink() for p in path.rglob('*')):continue
            items.append(str(path))
            if apply:
                if path.is_dir():shutil.rmtree(path)
                else:path.unlink()
    return {'retentionDays':7,'applied':apply,'expiredTelemetry':items,'unmarkedDataPreserved':True,
            'exportsPreserved':True}


def main():
    cli=argparse.ArgumentParser(description=__doc__);cli.add_argument('--root',type=Path,required=True)
    cli.add_argument('--apply',action='store_true',help='Default is a read-only expiry plan')
    args=cli.parse_args();print(json.dumps(prune(args.root,apply=args.apply),indent=2))


if __name__=='__main__':main()
