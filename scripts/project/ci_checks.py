"""Run source or provisioned full-business CI, with no dependency installation."""
import argparse
import ast
import json
import os
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[2]
def run(args,**kw):return subprocess.run(args,cwd=ROOT,check=True,**kw)
def main():
    p=argparse.ArgumentParser();p.add_argument('profile',choices=['source','full']);p.add_argument('--build',type=Path,default=Path('build-release'));p.add_argument('--output',type=Path,default=Path('build-function3/ci-report.json'));a=p.parse_args()
    report={'profile':a.profile,'passed':False,'installsDependencies':False};a.output.parent.mkdir(parents=True,exist_ok=True)
    try:
        if a.profile=='source':
            run(['python3' if os.name!='nt' else os.sys.executable,'scripts/project/check_boundaries.py'])
            run(['python3' if os.name!='nt' else os.sys.executable,'scripts/project/frontend_release.py','--check'])
            run([os.sys.executable,'tests/project/test_ci_gates.py'])
            run([os.sys.executable,'tests/project/test_process_identity.py'])
            run([os.sys.executable,'tests/project/test_source_inventory.py'])
            for folder in ['scripts','tests']:
                for path in (ROOT/folder).rglob('*.py'):ast.parse(path.read_text(encoding='utf-8-sig'),filename=str(path))
            for path in (ROOT/'frontend/phase11').rglob('*.js'):
                if 'vendor' not in path.parts:run(['node','--check',str(path)],stdout=subprocess.DEVNULL)
            run(['node','--test','tests/function3/frontend/history_capacity.test.mjs','tests/phase11/frontend/chat_reliability.test.mjs','tests/function3/frontend/unread_watermarks.test.mjs','tests/function3/frontend/workspace_logic.test.mjs'])
        else:
            build=a.build.resolve();cache=build/'CMakeCache.txt'
            if not cache.exists():raise RuntimeError('A provisioned Linux build is required. Configure PostgreSQL/libsodium ON; no silent fallback or installation.')
            values={line.split(':',1)[0]:line.split('=',1)[1] for line in cache.read_text().splitlines() if '=' in line and not line.startswith(('#','//'))}
            if Path(values.get('CMAKE_HOME_DIRECTORY','/missing-ci-source')).resolve()!=ROOT.resolve():raise RuntimeError('Configured build belongs to another source checkout; reconfigure this checkout first')
            required=['WEBSERVER_ENABLE_PHASE11','WEBSERVER_ENABLE_POSTGRES','WEBSERVER_ENABLE_SODIUM','BUILD_TESTING'];report['features']={n:values.get(n,'MISSING') for n in required}
            if any(report['features'][n] not in ('ON','TRUE','1') for n in required):raise RuntimeError('Required business features disabled: '+str(report['features']))
            if not os.environ.get('PHASE11_POSTGRES_CONNECTION'):raise RuntimeError('A real isolated PHASE11_POSTGRES_CONNECTION is required')
            run(['cmake','--build',str(build),'-j','2'])
            commands=json.loads((build/'compile_commands.json').read_text())
            report['compiledAdapters']={}
            for source,macro in [('PostgresStore.cpp','WEBSERVER_HAS_POSTGRES=1'),('SodiumCredentialCodec.cpp','WEBSERVER_HAS_SODIUM=1')]:
                entries=[entry for entry in commands if Path(entry['file']).name==source]
                if not entries or not all(macro in entry.get('command',' '.join(entry.get('arguments',[]))) for entry in entries):raise RuntimeError('Required adapter not compiled: '+source)
                report['compiledAdapters'][source]=True
            report['optionalAiFeatures']={n:values.get(n,'OFF') for n in ['WEBSERVER_ENABLE_CURL_CLIENT','WEBSERVER_ENABLE_ONNX_GENAI','WEBSERVER_ENABLE_GRPC']}
            tests=json.loads(subprocess.check_output(['ctest','--test-dir',str(build),'--show-only=json-v1'],text=True))
            report['discoveredTests']=[t['name'] for t in tests['tests']]
            report['matrix']={
                'communication':[n for n in report['discoveredTests'] if not n.startswith(('phase11','function3'))],
                'businessWithoutPostgres':[n for n in report['discoveredTests'] if n.startswith(('phase11','function3')) and 'postgres' not in n],
                'realPostgres':[n for n in report['discoveredTests'] if 'postgres' in n]}
            if not any('postgres' in n for n in report['discoveredTests']):raise RuntimeError('Real PostgreSQL contract not registered')
            run(['ctest','--test-dir',str(build),'--output-on-failure','-j','1'])
        report['passed']=True
    finally:a.output.write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
