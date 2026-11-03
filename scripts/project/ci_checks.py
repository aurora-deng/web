"""Run source or provisioned full-business CI, with no dependency installation."""
import argparse
import ast
import json
import os
from pathlib import Path
import subprocess
import re

ROOT=Path(__file__).resolve().parents[2]
def run(args,**kw):return subprocess.run(args,cwd=ROOT,check=True,**kw)

def test_matrix(names):
    def business(name):
        stage=re.match(r'phase(\d+)_',name)
        return name.startswith('function3_') or (stage is not None and int(stage[1])>=11)
    matrix={
        'communication':[n for n in names if not business(n)],
        'businessWithoutPostgres':[n for n in names if business(n) and 'postgres' not in n],
        'realPostgres':[n for n in names if 'postgres' in n],
    }
    required={'phase11_core_tests','function3_workspace_contract','phase11_chat_reliability_tests',
              'phase11_http_auth_tests','phase11_file_tests','phase11_sodium_credentials_tests',
              'phase11_postgres_contract_tests','function3_postgres_contract',
              'function3_runtime_config','function3_business_shutdown','function3_commit_wakeup'}
    missing=required-set(names)
    if missing or any(not lane for lane in matrix.values()):
        raise RuntimeError('Required test lanes/suites missing: '+','.join(sorted(missing)))
    if not any(n.startswith('HttpParserTest.') for n in names) or not any('WebSocket' in n and '.' in n for n in names):
        raise RuntimeError('Required HTTP/WebSocket protocol tests not registered; check installed GTest')
    return matrix
def main():
    p=argparse.ArgumentParser();p.add_argument('profile',choices=['source','full']);p.add_argument('--build',type=Path,default=Path('build-release'));p.add_argument('--output',type=Path,default=Path('build-function3/ci-report.json'));a=p.parse_args()
    report={'profile':a.profile,'passed':False,'installsDependencies':False};a.output.parent.mkdir(parents=True,exist_ok=True)
    try:
        if a.profile=='source':
            run(['python3' if os.name!='nt' else os.sys.executable,'scripts/project/check_boundaries.py'])
            run(['python3' if os.name!='nt' else os.sys.executable,'scripts/project/frontend_release.py','--check'])
            run([os.sys.executable,'scripts/project/check_presentation.py'])
            run([os.sys.executable,'scripts/project/sync_api_contracts.py','--check'])
            run([os.sys.executable,'scripts/project/check_doc_links.py'])
            run([os.sys.executable,'tests/project/test_api_contracts.py'])
            run([os.sys.executable,'tests/project/test_ci_gates.py'])
            run([os.sys.executable,'tests/project/test_process_identity.py'])
            run([os.sys.executable,'tests/project/test_source_inventory.py'])
            run([os.sys.executable,'tests/project/test_snapshot_pruning.py'])
            for folder in ['scripts','tests']:
                for path in (ROOT/folder).rglob('*.py'):ast.parse(path.read_text(encoding='utf-8-sig'),filename=str(path))
            for path in (ROOT/'frontend/phase11').rglob('*.js'):
                if 'vendor' not in path.parts:run(['node','--check',str(path)],stdout=subprocess.DEVNULL)
            run(['node','--test','tests/function3/frontend/history_capacity.test.mjs','tests/phase11/frontend/chat_reliability.test.mjs','tests/function3/frontend/unread_watermarks.test.mjs','tests/function3/frontend/workspace_logic.test.mjs','tests/function3/frontend/chat_scope.test.mjs','tests/function3/frontend/api_contracts.test.mjs'])
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
            report['matrix']=test_matrix(report['discoveredTests'])
            run(['ctest','--test-dir',str(build),'--output-on-failure','-j','1'])
        report['passed']=True
    finally:a.output.write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
