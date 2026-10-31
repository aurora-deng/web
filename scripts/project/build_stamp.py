"""Embed a public content fingerprint in the binary. No credentials or runtime data are read."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
def fingerprint():
    paths=[ROOT/'main.cpp',ROOT/'CMakeLists.txt']
    for name in ('backend','client','frontend'):
        paths.extend(p for p in (ROOT/name).rglob('*') if p.suffix in ('.h','.cpp','.js','.css','.html','.json') and not any(x.startswith(('build','.')) for x in p.relative_to(ROOT/name).parts))
    h=hashlib.sha256()
    # WindowsPath compares case-insensitively; PosixPath does not. Serialize the
    # same case-preserving, slash-separated ordering on both build hosts.
    for p in sorted(paths,key=lambda p:p.relative_to(ROOT).as_posix()):
        h.update(p.relative_to(ROOT).as_posix().encode()+b'\0'+p.read_bytes().replace(b'\r\n',b'\n'))
    return h.hexdigest()
def main():
    p=argparse.ArgumentParser();p.add_argument('--header',type=Path,required=True);a=p.parse_args()
    try:revision=subprocess.check_output(['git','rev-parse','--short=12','HEAD'],cwd=ROOT,stderr=subprocess.DEVNULL,text=True).strip()
    except (OSError,subprocess.CalledProcessError):revision='source-snapshot'
    payload=json.dumps({'sourceFingerprint':fingerprint(),'revision':revision,'requiredMigration':24,'testResults':'see phase49 evidence and retained phase43-48 history; not a runtime health assertion'},separators=(',',':'))
    text='#pragma once\nnamespace webserver::build { inline constexpr const char* versionJson=R"stamp('+payload+')stamp"; }\n'
    a.header.parent.mkdir(parents=True,exist_ok=True)
    if not a.header.exists() or a.header.read_text()!=text:a.header.write_text(text,encoding='utf-8')
if __name__=='__main__':main()
