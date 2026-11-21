"""Read-only comparison of the daily and study-test account databases."""

import argparse
import base64
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import sys
from tempfile import mkdtemp


HOME = Path('/home/pikachu')
PG = HOME / 'phase11-deps/postgresql/18.6/bin/psql'
DATA = HOME / 'phase11-data'
RELEASES = DATA / 'function3/yantu-releases'
DATABASES = {
    'daily': (DATA / 'postgresql/run', 5432, 'pikachu', 'phase11'),
    'isolated': (DATA / 'function3/pg/run', 55434, 'function3_test_admin', 'function3_runtime_test'),
}
SQL = '''
BEGIN ISOLATION LEVEL REPEATABLE READ READ ONLY;
SET LOCAL statement_timeout = '12s';
SET LOCAL TIME ZONE 'UTC';
WITH accounts AS (
  SELECT u.id::text AS id, u.username, u.display_name AS name, u.disabled,
         u.ai_account AS ai, COALESCE(r.role, 'user') AS role,
         COALESCE(m.n, 0) AS messages, COALESCE(c.n, 0) AS conversations,
         COALESCE(p.n, 0) AS posts, COALESCE(t.n, 0) AS replies, COALESCE(f.n, 0) AS files
  FROM users u
  LEFT JOIN site_roles r ON r.user_id = u.id
  LEFT JOIN (SELECT sender_id, count(*) n FROM messages GROUP BY sender_id) m ON m.sender_id = u.id
  LEFT JOIN (SELECT user_id, count(*) n FROM conversation_members GROUP BY user_id) c ON c.user_id = u.id
  LEFT JOIN (SELECT author_id, count(*) n FROM tree_posts GROUP BY author_id) p ON p.author_id = u.id
  LEFT JOIN (SELECT author_id, count(*) n FROM tree_replies GROUP BY author_id) t ON t.author_id = u.id
  LEFT JOIN (SELECT owner_id, count(*) n FROM file_objects WHERE state='ready' GROUP BY owner_id) f ON f.owner_id = u.id
)
SELECT jsonb_build_object('database', current_database(),
  'migration', (SELECT max(version) FROM phase11_schema_migrations),
  'databaseBytes', pg_database_size(current_database()),
  'accounts', COALESCE((SELECT jsonb_agg(to_jsonb(accounts) ORDER BY id::bigint) FROM accounts), '[]'::jsonb));
ROLLBACK;
'''


def database(source):
    socket, port, user, name = DATABASES[source]
    args = [PG, '-X', '-q', '-A', '-t', '-w', '-v', 'ON_ERROR_STOP=1',
            '-h', socket, '-p', str(port), '-U', user, '-d', name, '-f', '-']
    output = subprocess.check_output(args, input=SQL, text=True, encoding='utf-8', timeout=20)
    result = json.loads(output)
    if result['database'] != name:
        raise RuntimeError('The connection reached the wrong database')
    return result


def study_root():
    for release in RELEASES.glob('fb5913a59194-*'):
        pidfile = release / '.gateway-runner.pid'
        if pidfile.is_symlink() or not pidfile.is_file():
            continue
        pid = int(pidfile.read_text(encoding='ascii'))
        args = (Path('/proc') / str(pid) / 'cmdline').read_bytes().decode().split('\0')
        if 'backend.study.AccountWorkspace.LabRunner' not in args or '--work-dir' not in args:
            continue
        work = Path(args[args.index('--work-dir') + 1])
        if (work.parent == RELEASES and work.resolve() == work and work.name.startswith('lab-fb5913a59194-')
                and (work / 'accounts/.nebula-yantu-accounts').is_file()):
            return work / 'accounts'
    raise RuntimeError('The currently running isolated Yantu storage could not be identified')


def study_counts(root, rows):
    key_path = root / 'workspace-scope.key'
    if root.is_symlink() or key_path.is_symlink() or key_path.stat().st_size != 32:
        raise RuntimeError('The Yantu workspace scope key could not be verified')
    key = key_path.read_bytes()
    matched = set()
    for row in rows:
        scope = hashlib.blake2s(row['id'].encode('ascii'), key=key, digest_size=16).hexdigest()
        matched.add(scope)
        folder = root / scope
        path = folder / 'prep.sqlite3'
        if not path.is_file():
            continue
        if folder.is_symlink() or path.is_symlink():
            raise RuntimeError('A study workspace contains an unexpected symbolic link')
        with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=3)) as db:
            db.execute('PRAGMA query_only = ON')
            arrays = dict(db.execute('''SELECT item.key, json_array_length(item.value)
                 FROM document, json_each(document.body) AS item WHERE document.id=1 AND item.type='array' '''))
            files, = db.execute('SELECT count(*) FROM files').fetchone()
            row['study'] = {'notes': arrays.get('notes', 0), 'records': sum(arrays.values()), 'files': files}
    return sum(path.name not in matched for path in root.iterdir()
               if re.fullmatch(r'[0-9a-f]{32}', path.name) and not path.is_symlink() and path.is_dir())


def fixture(row):
    # Names alone never establish ownership or authorize deletion.
    if (re.fullmatch(r'yantu_smoke_[0-9a-f]{8}', row['username']) and row['name'] == 'Yantu test account'):
        return True
    return (re.fullmatch(r'activity_[0-9a-f]{10}_[0-3]', row['username']) is not None
            and row['name'] == row['username'])


def prepare_backups(data):
    root = DATA / 'function3/runtime/backups'
    if root.resolve() != root or not root.is_dir():
        raise RuntimeError('The existing private backup directory could not be verified')
    needed = sum(data[source]['databaseBytes'] for source in DATABASES)
    if shutil.disk_usage(root).free <= needed + 64 * 1024**2:
        raise RuntimeError('Not enough disk space to back up both databases')
    backup = Path(mkdtemp(prefix='account-unify-', dir=root))
    manifest = {}
    for source, (socket, port, user, database_name) in DATABASES.items():
        path = backup / f'{source}.dump'
        subprocess.run([PG.with_name('pg_dump'), '-w', '-h', socket, '-p', str(port), '-U', user, '-d', database_name,
                        '--format=custom', '--lock-wait-timeout=5s', '-f', path], check=True, timeout=300)
        subprocess.run([PG.with_name('pg_restore'), '--list', path], stdout=subprocess.DEVNULL, check=True, timeout=30)
        with path.open('rb') as stream:
            checksum = hashlib.sha256()
            for content in iter(lambda: stream.read(1024 * 1024), b''):
                checksum.update(content)
        manifest[source] = {'database': database_name, 'sha256': checksum.hexdigest(), 'bytes': path.stat().st_size}
    (backup / 'checked.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='ascii')
    return str(backup)


def collect(prepare=False):
    data = {source: database(source) for source in DATABASES}
    try:
        data['unmatchedStudyFolders'] = study_counts(study_root(), data['isolated']['accounts'])
        data['studyInventoryComplete'] = True
    except (OSError, RuntimeError, sqlite3.Error):
        data['studyInventoryComplete'] = False
    if prepare:
        try:
            os.umask(0o077)
            data['backupDirectory'] = prepare_backups(data)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            print(f'NEBULA_BACKUP_PREPARATION_FAILED {type(error).__name__}', flush=True)
            data['backupDirectory'] = None
    payload = json.dumps(data, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    print('NEBULA_ACCOUNT_INVENTORY ' + base64.b64encode(payload).decode('ascii'), flush=True)


def cell(value):
    text = ' '.join(str(value).split())
    # Untrusted display names may use Markdown syntax.
    return ''.join(f'&#{ord(ch)};' if ch in r'&<>|[]`*\_~!:' else ch for ch in text)


def report(data, path):
    daily = data['daily']['accounts']
    isolated = data['isolated']['accounts']
    daily_names = {row['username'] for row in daily}
    overlap = {row['username'] for row in isolated} & daily_names
    candidates = [row for row in isolated if not fixture(row) and not row['ai']]
    tables = ['# NEBULA 私有账号核对清单', '',
              '由日常业务库和研途隔离库只读生成。消息、会话、动态、回复和附件列为记录数量。',
              '两套库的来源 ID 各自分配。用户名相同的记录需要核对归属。', '']
    summary = {'daily': len(daily), 'isolated': len(isolated), 'sameUsername': len(overlap),
               'fixtureCandidates': sum(fixture(row) for row in isolated),
               'remainingIsolatedAccounts': len(candidates),
               'remainingIsolatedWithChatOrCommunity': sum(any(row[count] for count in ('conversations', 'posts', 'replies', 'files'))
                                                          for row in candidates),
               'remainingIsolatedWithStudyData': sum(bool(row.get('study', {}).get('records') or
                                                           row.get('study', {}).get('files')) for row in candidates),
               'studyInventoryComplete': data['studyInventoryComplete'], 'backupDirectory': data.get('backupDirectory'),
               'unmatchedStudyFolders': data.get('unmatchedStudyFolders'),
               'isolatedAccountsWithStudyData': sum(bool(row.get('study', {}).get('records') or
                                                          row.get('study', {}).get('files')) for row in isolated)}
    for source, label, rows in (('daily', '原日常账号', daily), ('isolated', '新版隔离账号', isolated)):
        tables.extend([f"## {label}（{len(rows)} 个，迁移 {data[source]['migration']}）", '',
                       '| 来源 ID | 登录名 | 显示名 | 角色 | 状态 | 会话 | 消息 | 动态 | 回复 | 附件 | 研途笔记/条目/附件 | 核对 |',
                       '|---|---|---|---|---|---:|---:|---:|---:|---:|---|---|'])
        for row in rows:
            study = row.get('study')
            study_text = (f"{study['notes']}/{study['records']}/{study['files']}" if study is not None
                          else ('0/0/0' if source == 'isolated' and data['studyInventoryComplete'] else '—'))
            marks = ['同名待核对'] if row['username'] in overlap else []
            if source == 'isolated' and fixture(row):
                marks.append('符合验收命名')
            if row['ai']:
                marks.append('AI 账号')
            values = (row['id'], row['username'], row['name'], row['role'], '停用' if row['disabled'] else '启用',
                      row['conversations'], row['messages'], row['posts'], row['replies'], row['files'], study_text,
                      '、'.join(marks) or '—')
            tables.append('| ' + ' | '.join(map(cell, values)) + ' |')
        tables.append('')
    if not data['studyInventoryComplete']:
        tables.append('研途记录盘点未完整，缺失计数以 `—` 表示。')
    if data.get('backupDirectory'):
        tables.extend(['', '## 迁移前备份', '',
                       f"两库快照保存在 Linux 的 `{data['backupDirectory']}`，已校验归档目录可读取。"])
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8') as stream:
        stream.write('\n'.join(tables) + '\n')
    print(json.dumps(summary), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, help='Render the private report from the audit payload on stdin')
    parser.add_argument('--prepare-merge', action='store_true', help='Keep checked database backups on the same Linux host')
    args = parser.parse_args()
    if args.report:
        data = json.loads(base64.b64decode(sys.stdin.buffer.read().strip(), validate=True))
        report(data, args.report)
    else:
        if not sys.platform.startswith('linux'):
            parser.error('Database collection runs only on the Linux host')
        collect(args.prepare_merge)


if __name__ == '__main__':
    main()
