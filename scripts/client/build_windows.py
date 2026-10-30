"""Build with the existing MinGW compiler using relative paths.

The installed MinGW Ninja uses ANSI filesystem calls and fails on this Chinese
checkout's external-test object paths. This explicit fallback keeps all command
arguments ASCII/relative, changes no global environment and downloads nothing.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import json
import time

root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser()
parser.add_argument('--boost-root',type=Path,required=True)
parser.add_argument('--configuration',choices=['release','debug'],default='release')
args=parser.parse_args()
if os.name!='nt':raise SystemExit('Windows only')
compiler=shutil.which('g++')
if not compiler:raise SystemExit('Use the existing C++ compiler; no installation attempted')
boost=args.boost_root.resolve()
if not (boost/'boost/beast.hpp').exists():raise SystemExit('Approved Boost headers missing')
output=root/'build-client'/args.configuration
output.mkdir(parents=True,exist_ok=True)
flags=['-std=c++20','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-D_WIN32_WINNT=0x0A00','-DBOOST_ASIO_NO_DEPRECATED',
       '-I.','-I'+os.path.relpath(boost,root)]
flags+=['-O2','-DNDEBUG'] if args.configuration=='release' else ['-Og','-g1']
sources=['client/companion/JsonRuntime/JsonRuntime.cpp','client/storage/SqliteStore/SqliteStore.cpp',
         'client/storage/WindowsProtection/WindowsProtection.cpp',
         'client/assistant/LocalKnowledge/LocalKnowledge.cpp','client/assistant/PrivateHistory/PrivateHistory.cpp',
         'client/companion/BoundedPool/BoundedPool.cpp','client/companion/LoopbackPolicy/LoopbackPolicy.cpp',
         'client/companion/SChannelStream/SChannelStream.cpp','client/companion/UpstreamTransport/UpstreamTransport.cpp',
         'client/companion/HttpConnectionPool/HttpConnectionPool.cpp',
         'client/companion/ProxyServer/ProxyServer.cpp','client/assistant/DocumentImport/DocumentImport.cpp',
         'client/storage/CredentialVault/CredentialVault.cpp','client/assistant/LocalAssistantService/LocalAssistantService.cpp',
         'client/assistant/ActionService/ActionService.cpp','client/assistant/PrivateMigration/PrivateMigration.cpp',
         'client/assistant/PersonalSpeech/PersonalSpeech.cpp','client/assistant/WindowsCloudSpeech/WindowsCloudSpeech.cpp',
         'backend/ai/speech/application/SpeechText/SpeechText.cpp','backend/ai/speech/application/WaveCodec/WaveCodec.cpp',
         'backend/ai/speech/application/SpeechWire/SpeechWire.cpp','backend/ai/speech/application/CloudSpeechPolicy/CloudSpeechPolicy.cpp']
objects=[]
# Shared domain/port headers are also transitive client dependencies. A broad
# conservative watermark prevents stale objects after a server-domain refactor.
header_time=max(p.stat().st_mtime for folder in (root/'client',root/'backend') for p in folder.rglob('*') if p.suffix in ('.h','.ipp'))
for source in sources:
    target=output/(Path(source).stem+'.o');objects.append(os.path.relpath(target,root))
    command=[compiler,*flags,'-c',source,'-o',objects[-1]]
    if not target.exists() or target.stat().st_mtime<max((root/source).stat().st_mtime,header_time,Path(__file__).stat().st_mtime):
        subprocess.run(command,cwd=root,check=True)
    print('Compiled',source,flush=True)
report={'configuration':args.configuration,'compiler':compiler,'boost':'1.92.0','tests':[]}
for name in ['storage','private_data','loopback_policy','action','speech']:
    test=output/f'companion_{name}_contract.exe'
    source='tests/function3/unit/speech_policy.cpp' if name=='speech' else f'tests/client/{name}_contract.cpp'
    subprocess.run([compiler,*flags,source,*objects,'-o',os.path.relpath(test,root),
        '-lcrypt32','-lbcrypt','-ladvapi32','-lws2_32','-lmswsock','-lsecur32','-lwinhttp'],cwd=root,check=True)
    start=time.monotonic();result=subprocess.run([str(test)],cwd=output,check=True,capture_output=True,text=True,encoding='utf-8')
    report['tests'].append({'name':name,'passed':True,'seconds':time.monotonic()-start,'output':result.stdout.strip()})
    print(result.stdout.strip(),flush=True)
(output/'validation.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
subprocess.run([compiler,*flags,'tests/client/tls_probe.cpp',*objects,'-o',os.path.relpath(output/'companion_tls_probe.exe',root),
    '-lcrypt32','-lbcrypt','-ladvapi32','-lws2_32','-lmswsock','-lsecur32','-lwinhttp'],cwd=root,check=True)
subprocess.run([compiler,*flags,'tests/client/http_pool_probe.cpp',*objects,'-o',os.path.relpath(output/'companion_http_pool_probe.exe',root),
    '-lcrypt32','-lbcrypt','-ladvapi32','-lws2_32','-lmswsock','-lsecur32','-lwinhttp'],cwd=root,check=True)
print('Built TLS and HTTP pool diagnostics; no network connection or certificate trust performed',flush=True)
subprocess.run([compiler,*flags,'client/companion/CompanionMain/CompanionMain.cpp',*objects,'-municode','-o',os.path.relpath(output/'nebula_companion.exe',root),
    '-lcrypt32','-lbcrypt','-ladvapi32','-lws2_32','-lmswsock','-lsecur32','-lwinhttp'],cwd=root,check=True)
print('Built native Windows companion; no service started',flush=True)
