"""Rebuild only Phase40 tables in the existing disposable contract DB; prove rollback and replay."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import os

ROOT=Path(__file__).resolve().parents[3]
PG='/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
CONNECTION='host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_phase11_test'

def sql(query):return subprocess.check_output([PG,CONNECTION,'-w','-qAtc',query],text=True).strip()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);a=parser.parse_args()
    assert sql('SELECT current_database()')=='function3_phase11_test'
    assert sql('SELECT count(*) FROM speech_profiles')=='2' and sql('SELECT count(*) FROM speech_usage')=='0','Refusing non-default contract fixture'
    file=ROOT/'backend/application/storage/postgres/migrations/022_speech_profiles_and_budget.sql';source=file.read_text()
    assert source.count('BEGIN;')==source.count('COMMIT;')==1 and 'VALUES(22,' in source
    # This DB is explicitly disposable; never drop runtime/production tables or migrate another database.
    sql('BEGIN; DROP TABLE speech_profiles,speech_usage; DELETE FROM phase11_schema_migrations WHERE version=22; COMMIT;')
    injected=source.replace('COMMIT;','SELECT 1/0; COMMIT;')
    failed=subprocess.run([PG,CONNECTION,'-w','-v','ON_ERROR_STOP=1'],input=injected,text=True,capture_output=True)
    assert failed.returncode!=0 and 'division by zero' in failed.stderr
    assert sql("SELECT to_regclass('speech_profiles') IS NULL AND to_regclass('speech_usage') IS NULL")=='t'
    assert sql('SELECT count(*) FROM phase11_schema_migrations WHERE version=22')=='0'
    env={k:v for k,v in os.environ.items() if not k.startswith('WEB_')}
    env.update({'WEB_PHASE11_DATABASE':CONNECTION,'WEB_PHASE11_TOKEN_KEY_FILE':'/home/pikachu/phase11-data/function3/validation/current.token.key',
                'WEB_PHASE11_FILE_ROOT':'/home/pikachu/phase11-data/function3/validation/phase41/schema-guard-files','WEB_FUNCTION3_ENABLE':'1',
                'WEB_SERVER_PORT':'18095','WEB_GRPC_ADDRESS':'127.0.0.1:50065'})
    guard=subprocess.run([str(ROOT/'build-release/webserver')],cwd=ROOT/'build-release',env=env,capture_output=True,text=True,timeout=20)
    assert guard.returncode!=0 and 'through 022' in guard.stdout+guard.stderr,'Missing migration must fail startup before background tasks'
    for _ in range(2):subprocess.run(['python3',str(ROOT/'scripts/function3/validation/isolated_postgres.py'),'migrate','--database','function3_phase11_test'],check=True,stdout=subprocess.DEVNULL)
    assert sql('SELECT count(*) FROM speech_profiles')=='2' and sql('SELECT count(*) FROM phase11_schema_migrations WHERE version=22')=='1'
    report={'database':'function3_phase11_test','passed':True,'forcedFailureRolledBackDDLAndHistory':True,'missingMigrationRefusedStartup':True,'reapplyAfterRollback':True,'secondMigrateSkippedAppliedVersion':True,'sourceSha256':hashlib.sha256(source.encode()).hexdigest()}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n');print('PASS speech migration atomic rollback, clean apply and repeated migration')

if __name__=='__main__':main()
