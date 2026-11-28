"""Real PostgreSQL CLI contract, restricted to a caller-provisioned *_test DB.

No production access or dependency installation. With no test configuration this
suite is skipped. All records live in a unique test-owned schema and are removed.
"""
import json
import os
from pathlib import Path
import subprocess
import unittest
import uuid


@unittest.skipUnless(os.environ.get('SITE_ADMIN_TEST_EXECUTABLE') and os.environ.get('PHASE11_POSTGRES_CONNECTION'), 'requires isolated PostgreSQL test DB and built CLI')
class SiteAdminCli(unittest.TestCase):
    def test_explicit_offline_grant_and_unchanged_bootstrap_protection(self):
        executable = Path(os.environ['SITE_ADMIN_TEST_EXECUTABLE']).resolve(strict=True)
        psql = os.environ.get('TEST_PSQL', 'psql')
        connection = os.environ['PHASE11_POSTGRES_CONNECTION']
        def query(sql):
            return subprocess.check_output([psql, '-XAt', '-v', 'ON_ERROR_STOP=1', '-d', connection, '-c', sql], text=True).strip()
        self.assertTrue(query('SELECT current_database()').endswith('_test'), 'refusing non-test database')
        schema = 'personnel_cli_' + uuid.uuid4().hex
        query(f'CREATE SCHEMA {schema}')
        def sql(statement):
            return query(f'SET search_path TO {schema}; ' + statement).removeprefix('SET\n')
        def run(*arguments):
            env = dict(os.environ, WEB_PHASE11_DATABASE=connection + ' options=-csearch_path=' + schema)
            return subprocess.run([str(executable), *arguments], env=env, capture_output=True, text=True)
        try:
            sql("""CREATE TABLE users(id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,username TEXT UNIQUE NOT NULL,disabled BOOLEAN NOT NULL DEFAULT FALSE,ai_account BOOLEAN NOT NULL DEFAULT FALSE);
                CREATE TABLE site_roles(user_id BIGINT PRIMARY KEY REFERENCES users(id),role TEXT NOT NULL);
                CREATE TABLE user_sessions(user_id BIGINT REFERENCES users(id),revoked_at TIMESTAMPTZ);
                CREATE TABLE site_audit(actor_id BIGINT REFERENCES users(id),action TEXT,resource TEXT,reason TEXT);
                INSERT INTO users(username,disabled,ai_account) VALUES('existing_admin',FALSE,FALSE),('new_admin',FALSE,FALSE),('disabled_user',TRUE,FALSE),('service_user',FALSE,TRUE);
                INSERT INTO site_roles VALUES(1,'administrator'); INSERT INTO user_sessions(user_id) VALUES(2);""")
            self.assertNotEqual(run('--bootstrap', 'new_admin', 'must not bypass bootstrap').returncode, 0)
            self.assertEqual(run('--operator-grant-administrator', 'new_admin', 'requires confirmation').returncode, 2)
            for username in ('missing_user', 'disabled_user', 'service_user'):
                self.assertNotEqual(run('--operator-grant-administrator', username, 'invalid target test', '--confirm').returncode, 0)
            self.assertNotEqual(run('--operator-grant-administrator', 'new_admin', '   ', '--confirm').returncode, 0)
            granted = run('--operator-grant-administrator', 'new_admin', '用户明确授权的离线维护', '--confirm')
            self.assertEqual(granted.returncode, 0, granted.stderr)
            result = json.loads(sql("SELECT json_build_object('admins',(SELECT count(*) FROM site_roles WHERE role='administrator'),'audit',(SELECT count(*) FROM site_audit WHERE action='operator_grant_administrator' AND reason LIKE 'offline database-owner maintenance: %'),'revoked',(SELECT count(*) FROM user_sessions WHERE user_id=2 AND revoked_at IS NOT NULL))"))
            self.assertEqual(result, {'admins':2,'audit':1,'revoked':1})
            sql('INSERT INTO user_sessions(user_id) VALUES(2)')
            self.assertEqual(run('--operator-grant-administrator', 'new_admin', 'repeat authorization', '--confirm').returncode, 0)
            self.assertEqual(sql('SELECT count(*) FROM site_audit'), '1')
            self.assertEqual(sql('SELECT count(*) FROM user_sessions WHERE user_id=2 AND revoked_at IS NULL'), '1')
            # First-install bootstrap is still available only with no admin.
            sql('DELETE FROM site_roles')
            self.assertEqual(run('--bootstrap', 'new_admin', 'first administrator test').returncode, 0)
            self.assertEqual(sql("SELECT count(*) FROM site_audit WHERE action='bootstrap_administrator'"), '1')
        finally:
            query(f'DROP SCHEMA {schema} CASCADE')


if __name__ == '__main__':
    unittest.main()
