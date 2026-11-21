"""Read-only build/runtime preflight. Does not install, migrate or start services."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

NATIVE = ('GRPC','ONNX_GENAI','ROCKETMQ','SHERPA_TTS')
WORKERS = {'onnx':'webserver-onnx-worker',
           'speech':'backend/application/modules/function3-tts-worker'}


def digest(path):
    value=hashlib.sha256()
    with path.open('rb') as stream:
        for part in iter(lambda:stream.read(1024**2),b''):
            value.update(part)
    return value.hexdigest()


def cache_values(path):
    return {row.split(':',1)[0]:row.split('=',1)[1] for row in path.read_text().splitlines()
            if '=' in row and not row.startswith(('#','//'))}


def linkage(path):
    # Only call for project-built ELF files in the explicit build directory.
    if not sys.platform.startswith('linux'):
        return {'checked':False,'reason':'Linux ELF runtime required'}
    with path.open('rb') as stream:
        magic=stream.read(4)
    if magic!=b'\x7fELF':
        return {'checked':False,'reason':'not an ELF binary'}
    result=subprocess.run(['ldd',str(path)],capture_output=True,text=True,timeout=15)
    dependencies=[];missing=[]
    for row in result.stdout.splitlines():
        if '=> not found' in row:
            missing.append(row.split()[0])
        elif match:=re.match(r'\s*(\S+)\s+=>\s+(/\S+)\s+\(',row):
            name,resolved=match.groups();target=Path(resolved)
            dependencies.append({'soname':name,'path':str(target.resolve()),'sha256':digest(target),
                'provisioning':'system' if str(target.resolve()).startswith(('/usr/','/lib')) else 'external-approved'})
    return {'checked':True,'resolved':result.returncode==0 and not missing,
            'missing':missing,'dependencies':dependencies}


def inventory(build,profile,environment=None,check_linkage=True):
    build=Path(build).resolve();env=os.environ if environment is None else environment
    cache=cache_values(build/'CMakeCache.txt');problems=[]
    flags={key.removeprefix('WEBSERVER_ENABLE_'):value in ('ON','TRUE','1')
           for key,value in cache.items() if key.startswith('WEBSERVER_ENABLE_')}
    for flag in ('PHASE11','POSTGRES','SODIUM','CURL_CLIENT',*NATIVE):
        expected=(flag in NATIVE if profile=='native' else False) or (
            flag in ('PHASE11','POSTGRES','SODIUM','CURL_CLIENT') and profile!='communication')
        if flag=='PHASE11':expected=True  # Current communication profile retains in-memory routes.
        if flag=='CURL_CLIENT' and profile=='business':expected=flags.get(flag,False)
        if flags.get(flag,False)!=expected:
            problems.append('compiled feature '+flag+' differs from '+profile+' profile')
    binaries={}
    programs={'server':'webserver',**(WORKERS if profile=='native' else {})}
    for name,relative in programs.items():
        path=build/relative
        if not path.is_file():
            problems.append('required program missing: '+relative);continue
        link=linkage(path) if check_linkage else {'checked':False}
        if check_linkage and not link.get('checked'):
            problems.append('runtime linkage was not checked: '+relative)
        if link.get('checked') and not link.get('resolved'):
            problems.append('unresolved runtime libraries: '+relative)
        binaries[name]={'path':relative,'sha256':digest(path),'linkage':link}
    configured={key:bool(env.get(key)) for key in ('WEB_PHASE11_DATABASE','WEB_PHASE11_TOKEN_KEY_FILE',
        'WEB_PHASE11_ONNX_MODEL_ROOT','WEB_PHASE11_ONNX_GRPC_TARGET','WEB_FUNCTION3_TTS_WORKER',
        'WEB_FUNCTION3_MQ_ENDPOINT','WEB_AUTH_SECRET')}
    runtime_problems=[]
    if configured['WEB_PHASE11_DATABASE']!=configured['WEB_PHASE11_TOKEN_KEY_FILE']:
        runtime_problems.append('database and token-key file must be configured together')
    if configured['WEB_PHASE11_TOKEN_KEY_FILE'] and not Path(env['WEB_PHASE11_TOKEN_KEY_FILE']).is_file():
        runtime_problems.append('configured token-key file is missing')
    for key,kind in (('WEB_PHASE11_ONNX_MODEL_ROOT','directory'),('WEB_FUNCTION3_TTS_WORKER','file')):
        if configured[key] and not (Path(env[key]).is_dir() if kind=='directory' else Path(env[key]).is_file()):
            runtime_problems.append('configured '+key+' is missing')
    if configured['WEB_PHASE11_ONNX_MODEL_ROOT'] and not flags.get('ONNX_GENAI'):
        runtime_problems.append('ONNX configured in a build without ONNX support')
    if configured['WEB_FUNCTION3_TTS_WORKER'] and not flags.get('SHERPA_TTS'):
        runtime_problems.append('TTS configured in a build without sherpa support')
    return {'schemaVersion':1,'profile':profile,'passed':not problems,'features':flags,'programs':binaries,
        'buildProblems':problems,'runtime':{'configured':configured,'problems':runtime_problems,
            'configurationValid':not runtime_problems,'databaseConnectionProbed':False,
            'modelWarmupProbed':False,'generationQualityVerified':False},
        'externalFilesBundled':False,'privateValuesIncluded':False}


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--build',type=Path,required=True)
    cli.add_argument('--profile',choices=('communication','business','native'),required=True)
    cli.add_argument('--output',type=Path,required=True)
    cli.add_argument('--require-runtime-config',action='store_true')
    args=cli.parse_args();report=inventory(args.build,args.profile)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'passed':report['passed'],'profile':args.profile,'problems':report['buildProblems'],
                      'runtimeConfigurationValid':report['runtime']['configurationValid']}))
    return 0 if report['passed'] and (not args.require_runtime_config or report['runtime']['configurationValid']) else 1


if __name__=='__main__':
    raise SystemExit(main())
