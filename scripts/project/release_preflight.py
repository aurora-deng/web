"""Verify an extracted release and optionally match it to a running local business server."""
import argparse
import hashlib
import http.client
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


def probe_business(port, release):
    """Check the served profile, build, schema and assets using anonymous read-only requests."""
    revision = release.get('revision')
    fingerprint = release.get('sourceFingerprint')
    migration = release.get('migration')
    expected_assets = release['files'].get('frontend/phase11/asset-manifest.json')
    if (not isinstance(revision, str) or not revision or not isinstance(fingerprint, str)
            or not fingerprint or type(migration) is not int or not expected_assets):
        return ['release manifest lacks a verified business identity']

    def fetch(path, status, read_body=True):
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
        try:
            conn.request('GET', path, headers={'Cache-Control': 'no-cache'})
            with conn.getresponse() as reply:
                if reply.status != status:
                    raise ValueError(f'{path} returned HTTP {reply.status}, expected {status}')
                if read_body:
                    body = reply.read(16385)
                    if len(body) > 16384:
                        raise ValueError(f'{path} returned an oversized response')
                    return body
        finally:
            conn.close()

    checks = (
        ('/health/live', 200, lambda b: json.loads(b) == {'status': 'alive'}),
        ('/health/ready', 200, lambda b: json.loads(b) == {'status': 'ready'}),
        ('/health/version', 200, lambda b: matches_build(b, revision, fingerprint, migration)),
        ('/phase11/asset-manifest.json', 200, lambda b: hashlib.sha256(b).hexdigest() == expected_assets),
        ('/api/me', 401, None),
    )
    problems = []
    for path, status, matches in checks:
        try:
            body = fetch(path, status, read_body=matches is not None)
            if matches is not None and not matches(body):
                problems.append(path + ' does not match the verified business release')
        except (UnicodeError, json.JSONDecodeError):
            problems.append(path + ' returned invalid JSON')
        except (OSError, http.client.HTTPException):
            problems.append(path + ' could not be read from the local business server')
        except ValueError as error:
            problems.append(str(error))
    return problems


def matches_build(body, revision, fingerprint, migration):
    version = json.loads(body)
    return (isinstance(version, dict) and version.get('schemaVerifiedAtStartup') is True
            and version.get('requiredMigration') == migration
            and version.get('sourceFingerprint') == fingerprint
            and version.get('revision') in (revision, revision[:12]))


def check(root,environment=None,check_linkage=True,*,business_port=None):
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
    probe_ran = business_port is not None and not problems
    if probe_ran:
        problems.extend(probe_business(business_port, release))
    return {'passed':not problems,'profile':release.get('profile','business'),'problems':problems,
            'externalLibrariesChecked':libraries_checked,'runtimeConfigured':configured,
            'databaseConnectionProbed':False,'modelWarmupProbed':False,'qualityVerified':False,
            'localBusinessProbed':probe_ran,'localBusinessMatched':probe_ran and not problems,
            'secretsIncluded':False}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parent)
    parser.add_argument('--business-port',type=int,help='Also verify the running business release at 127.0.0.1:PORT')
    args=parser.parse_args()
    if args.business_port is not None and not 1 <= args.business_port <= 65535:
        parser.error('--business-port must be in 1..65535')
    report=check(args.root,business_port=args.business_port)
    print(json.dumps(report,indent=2))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
