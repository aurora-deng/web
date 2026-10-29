"""Use the installed PostgreSQL in a private Unix-socket-only Function3 test cluster.

No packages, system configuration, formal database, or formal service are changed.
Only this marked directory is managed; no recursive deletion is performed.
"""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('action', choices=('start', 'stop', 'migrate', 'status'))
parser.add_argument('--database', choices=('function3_phase11_test','function3_runtime_test'), default='function3_phase11_test')
parser.add_argument('--root', type=Path, default=Path('/home/pikachu/phase11-data/function3/pg'))
parser.add_argument('--postgres', type=Path, default=Path('/home/pikachu/phase11-deps/postgresql/18.6'))
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[3])
args = parser.parse_args()
root = args.root.resolve()
allowed = Path('/home/pikachu/phase11-data/function3').resolve()
if not root.is_relative_to(allowed) or root == allowed:
    parser.error('isolated cluster must be below /home/pikachu/phase11-data/function3')
marker = root/'FUNCTION3_TEST_CLUSTER'
data, run = root/'data', root/'run'
port, user, database = '55434', 'function3_test_admin', args.database
binary = args.postgres/'bin'

def execute(arguments, capture=False, check=True):
    return subprocess.run([str(x) for x in arguments], check=check, text=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None)

def psql(sql, name='postgres'):
    return execute([binary/'psql', '-w', '-h', run, '-p', port, '-U', user,
                    '-d', name, '-v', 'ON_ERROR_STOP=1', '-Atc', sql], capture=True).stdout.strip()

if root.exists() and not marker.exists():
    parser.error('refusing to manage an unmarked existing directory')
if args.action == 'start':
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(root, 0o700)
    marker.write_text('Function3 disposable test database; never use as formal database\n')
    run.mkdir(exist_ok=True, mode=0o700)
    if not (data/'PG_VERSION').exists():
        execute([binary/'initdb', '-D', data, '-U', user, '--auth-local=trust',
                 '--auth-host=reject', '--encoding=UTF8', '--locale=C.UTF-8'])
    if execute([binary/'pg_ctl', '-D', data, 'status'], capture=True, check=False).returncode:
        options = f"-k {run} -p {port} -c listen_addresses='' -c shared_buffers=64MB -c max_connections=40"
        execute([binary/'pg_ctl', '-D', data, '-l', root/'startup.log', '-o', options, '-w', 'start'])
    if not psql(f"SELECT 1 FROM pg_database WHERE datname='{database}'"):
        psql(f'CREATE DATABASE {database}')
elif args.action == 'stop':
    if marker.exists(): execute([binary/'pg_ctl', '-D', data, '-m', 'fast', '-w', 'stop'])
elif args.action == 'status':
    execute([binary/'pg_ctl', '-D', data, 'status'])
elif args.action == 'migrate':
    migrations = args.source/'backend/application/storage/postgres/migrations'
    applied = set()
    if psql("SELECT to_regclass('public.phase11_schema_migrations')", database):
        applied = set(psql('SELECT version FROM phase11_schema_migrations', database).splitlines())
    for migration in sorted(migrations.glob('*.sql')):
        version = str(int(migration.name.split('_', 1)[0]))
        if version in applied: continue
        execute([binary/'psql', '-w', '-h', run, '-p', port, '-U', user, '-d', database,
                 '-v', 'ON_ERROR_STOP=1', '-f', migration])
        print('Applied', migration.name)
if args.action in ('start', 'migrate'):
    print(f'FUNCTION3_TEST_CONNECTION=host={run} port={port} user={user} dbname={database}')
