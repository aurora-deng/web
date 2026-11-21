"""Run the full local workspace against created-on-demand fixture data."""
import http.client
from contextlib import ExitStack, closing
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
LAUNCH = [sys.executable, '-B', '-m', 'client.yantu.LocalWorkspace.LocalWorkspace']


class LocalWorkspaceTest(unittest.TestCase):
    def test_workspaces_do_not_share_data_or_session(self):
        scratch = ROOT / 'build-function3'
        scratch.mkdir(exist_ok=True)
        with ExitStack() as cleanup:
            tmp = cleanup.enter_context(tempfile.TemporaryDirectory(prefix='yantu-test-', dir=scratch))
            root = Path(tmp)
            legacy = root / 'existing-app'
            legacy.mkdir()
            with closing(sqlite3.connect(legacy / 'prep.sqlite3')) as db, db:
                db.execute('CREATE TABLE original(id INTEGER PRIMARY KEY)')
                db.execute('INSERT INTO original VALUES (2718)')
            before = (legacy / 'prep.sqlite3').read_bytes()
            rejected = subprocess.run(LAUNCH + ['--data-dir', str(legacy)], cwd=ROOT, capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)

            def start(directory):
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1', 0))
                    port = listener.getsockname()[1]
                stderr = cleanup.enter_context((root / (directory.name + '.stderr')).open('w+b'))
                process = subprocess.Popen(LAUNCH + ['--data-dir', str(directory), '--port', str(port)],
                                           cwd=ROOT, stdout=subprocess.DEVNULL, stderr=stderr,
                                           creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))

                def request(path, body=None, **headers):
                    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
                    try:
                        if body is None:
                            connection.request('GET', path, headers=headers)
                        else:
                            connection.request('POST', path, json.dumps(body),
                                               {'Content-Type': 'application/json', **headers})
                        result = connection.getresponse()
                        return result.status, json.loads(result.read())
                    finally:
                        connection.close()

                cleanup.callback(self.stop, process)
                for _ in range(60):
                    try:
                        if request('/api/health')[0] == 200:
                            break
                    except OSError:
                        if process.poll() is not None:
                            stderr.seek(0)
                            self.fail(stderr.read().decode(errors='replace'))
                        time.sleep(.1)
                else:
                    self.fail('Fresh workspace did not start')
                return request, process

            first, process_a = start(root / 'fresh-a')
            second, process_b = start(root / 'fresh-b')
            self.assertEqual(first('/api/state')[0], 401)
            self.assertEqual(first('/api/pair', {}, Origin='https://invalid.test')[0], 403)
            self.assertEqual(first('/api/pair', {}, Host='invalid.test')[0], 403)
            self.assertEqual(first('/api/remote/start', {})[0], 403)
            token_a = first('/api/pair', {})[1]['deviceToken']
            token_b = second('/api/pair', {})[1]['deviceToken']
            auth_a, auth_b = 'Bearer ' + token_a, 'Bearer ' + token_b
            self.assertNotEqual(token_a, token_b)
            self.assertEqual(second('/api/state', Authorization=auth_a)[0], 401)
            self.assertEqual(first('/api/state', Cookie='prep_session=' + token_a)[0], 401)
            status, snapshot = first('/api/state', Authorization=auth_a)
            self.assertEqual(status, 200)
            self.assertFalse(snapshot['state'].get('notes'))

            operation = {'opId': 'create-note-a', 'kind': 'upsert', 'collection': 'notes',
                         'id': 'fixture-note', 'fields': {'title': 'isolated test'}, 'unset': [],
                         'base': {}, 'baseExists': False, 'baseRecord': {}}
            status, answer = first('/api/sync', {'clientId': 'fixture-a', 'operations': [operation]},
                                   Authorization=auth_a)
            self.assertEqual(status, 200, answer)
            latest = first('/api/state', Authorization=auth_a)[1]['state']
            self.assertTrue(any(note['id'] == 'fixture-note' for note in latest['notes']), latest)
            self.assertFalse(second('/api/state', Authorization=auth_b)[1]['state'].get('notes'))
            self.assertEqual((legacy / 'prep.sqlite3').read_bytes(), before)

            for request, process, auth in ((first, process_a, auth_a), (second, process_b, auth_b)):
                self.assertEqual(request('/api/shutdown', {}, Authorization=auth)[0], 200)
                self.assertEqual(process.wait(timeout=8), 0)

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
