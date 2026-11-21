"""Verify an extracted release and its external prerequisites without starting it."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess


def digest(path):
    result=hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda:stream.read(1024**2),b''):
            result.update(data)
    return result.hexdigest()


def check(root,environment=None,check_linkage=True):
    root=Path(root).resolve();env=os.environ if environment is None else environment
    release=json.loads((root/'release.json').read_text());problems=[]
    for relative,expected in release['files'].items():
        path=(root/relative).resolve()
        if not path.is_relative_to(root) or not path.is_file() or digest(path)!=expected:
            problems.append('release file missing or changed: '+relative)
    if release.get('machine')!=platform.machine():
        problems.append('machine architecture differs from release')
    native_path=root/'native-runtime.json';libraries_checked=False
    if release.get('profile')=='native' and not native_path.is_file():
        problems.append('native runtime inventory is missing')
    if native_path.is_file() and not problems:
        native=json.loads(native_path.read_text())
        libraries_checked=check_linkage
        for program in native['programs'].values():
            path=(root/program['path']).resolve()
            if not path.is_relative_to(root) or not path.is_file() or digest(path)!=program['sha256']:
                problems.append('native worker missing or changed');continue
            if not check_linkage:continue
            result=subprocess.run(['ldd',str(path)],capture_output=True,text=True,timeout=15)
            resolved={m[1]:Path(m[2]) for line in result.stdout.splitlines()
                      if (m:=re.match(r'\s*(\S+)\s+=>\s+(/\S+)\s+\(',line))}
            if result.returncode or '=> not found' in result.stdout:
                problems.append('native program has unresolved external libraries: '+program['path'])
            for dependency in program['linkage'].get('dependencies',[]):
                actual=resolved.get(dependency['soname'])
                if actual is None or digest(actual)!=dependency['sha256']:
                    problems.append('external library missing or changed: '+dependency['soname'])
    configured={key:bool(env.get(key)) for key in ('WEB_PHASE11_DATABASE','WEB_PHASE11_TOKEN_KEY_FILE',
        'WEB_TLS_CERT','WEB_TLS_KEY','WEB_PHASE11_ONNX_MODEL_ROOT','WEB_FUNCTION3_TTS_WORKER')}
    if configured['WEB_PHASE11_DATABASE']!=configured['WEB_PHASE11_TOKEN_KEY_FILE']:
        problems.append('database and token-key file must be configured together')
    if configured['WEB_TLS_CERT']!=configured['WEB_TLS_KEY']:
        problems.append('TLS certificate and key must be configured together')
    for key in ('WEB_PHASE11_TOKEN_KEY_FILE','WEB_TLS_CERT','WEB_TLS_KEY','WEB_FUNCTION3_TTS_WORKER'):
        if configured[key] and not Path(env[key]).is_file():problems.append('configured file missing: '+key)
    if configured['WEB_PHASE11_ONNX_MODEL_ROOT'] and not Path(env['WEB_PHASE11_ONNX_MODEL_ROOT']).is_dir():
        problems.append('configured ONNX model directory missing')
    if release.get('profile','business')!='native' and any(configured[k] for k in ('WEB_PHASE11_ONNX_MODEL_ROOT','WEB_FUNCTION3_TTS_WORKER')):
        problems.append('native runtime configured in a business-only release')
    return {'passed':not problems,'profile':release.get('profile','business'),'problems':problems,
            'externalLibrariesChecked':libraries_checked,'runtimeConfigured':configured,
            'databaseConnectionProbed':False,'modelWarmupProbed':False,'qualityVerified':False,
            'secretsIncluded':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parent)
    args=parser.parse_args();report=check(args.root)
    print(json.dumps(report,indent=2))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
