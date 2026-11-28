"""Offline lifecycle contracts. All processes, TLS and sockets are mocked."""
from contextlib import nullcontext
from pathlib import Path
import signal
import ssl
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.function3.runtime import onnx_worker as worker


class DailyWorkerContracts(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'build-function3/daily-worker-contracts'
        base.mkdir(parents=True, exist_ok=True)
        temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.controller = worker.WorkerController(self.root / 'source', self.root / 'runtime')
        self.controller.root.mkdir(parents=True)
        nofollow = patch.object(worker.os, 'O_NOFOLLOW', getattr(worker.os, 'O_NOFOLLOW', 0), create=True)
        nofollow.start()
        self.addCleanup(nofollow.stop)

    def test_cpu_bounds(self):
        self.assertEqual(worker.cpu_set('2,3'), {2, 3})
        for value in ('', '-1', '2-3', '2,2', '2;', '２', '1,' , '0' * 129):
            with self.subTest(value=value), self.assertRaises(ValueError):
                worker.cpu_set(value)

    def test_environment_is_allowlisted_and_production_tls_only(self):
        value = worker.worker_environment(
            {'PATH': '/usr/bin', 'LD_LIBRARY_PATH': '/existing/lib',
             'DATABASE_URL': 'synthetic-do-not-forward', 'HTTPS_PROXY': 'synthetic',
             'WEB_PHASE11_DATABASE': 'synthetic', 'API_KEY': 'synthetic',
             'WEB_OBSERVABILITY_OTLP': 'synthetic'},
            '/approved/models', '/fixture/cert', '/fixture/key', 'synthetic-test-token', 50061)
        self.assertEqual(value['WEB_PRODUCTION_MODE'], '1')
        self.assertEqual(value['WEB_PHASE11_ONNX_WORKER_ADDRESS'], '127.0.0.1:50061')
        self.assertEqual(value['WEB_PHASE11_ONNX_MODELS'], '1')
        self.assertEqual(value['WEB_PHASE11_ONNX_WORKERS'], '1')
        for name in ('DATABASE_URL', 'HTTPS_PROXY', 'API_KEY', 'WEB_PHASE11_DATABASE', 'WEB_OBSERVABILITY_OTLP'):
            self.assertNotIn(name, value)
        client = worker.client_environment('/fixture/cert', 'synthetic-test-token', 50061)
        self.assertNotIn('WEB_PHASE11_ONNX_MODEL_ROOT', client)
        self.assertEqual(client['WEB_PHASE11_ONNX_GRPC_TLS_NAME'], 'localhost')

    def test_process_identity_handles_spaces_and_exact_executable(self):
        proc = self.root / 'proc'
        proc.mkdir()
        binary = self.controller.binary
        (proc / 'cmdline').write_bytes(bytes(binary) + b'\0arg\0')
        fields = ['S'] + ['0'] * 18 + ['12345']
        (proc / 'stat').write_text('123 (worker with spaces) ' + ' '.join(fields))
        self.assertEqual(worker.process_identity(proc, binary), '12345')
        with self.assertRaisesRegex(RuntimeError, 'executable mismatch'):
            worker.process_identity(proc, self.root / 'other')
        fields[0] = 'Z'
        (proc / 'stat').write_text('123 (worker) ' + ' '.join(fields))
        self.assertIsNone(worker.process_identity(proc, binary))

    def start_context(self):
        controller = self.controller
        controller._locked = lambda: nullcontext()
        for path in (controller.binary, controller.certificate, controller.private_key):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('synthetic fixture, not a credential')
        controller.model_root = self.root / 'models'
        controller.model_root.mkdir()
        return controller

    def test_start_uses_affinity_nice_and_validated_tls(self):
        controller = self.start_context()
        process = MagicMock(pid=4321)
        process.poll.return_value = None
        with patch.object(controller, 'owned', return_value=None), \
             patch.object(controller, '_token', return_value='synthetic-test-token'), \
             patch.object(worker.os, 'sched_getaffinity', return_value={0, 1, 2, 3}, create=True), \
             patch.object(worker.stat, 'S_IMODE', return_value=0), \
             patch.object(worker.shutil, 'which', side_effect=lambda x: '/usr/bin/' + x), \
             patch.object(worker.socket, 'socket'), \
             patch.object(worker.socket, 'create_connection'), \
             patch.object(worker.ssl, 'create_default_context') as tls, \
             patch.object(worker, 'process_identity', return_value='987'), \
             patch.object(worker.subprocess, 'Popen', return_value=process) as spawn:
            env = controller.start()
        self.assertEqual(env['WEB_PHASE11_ONNX_GRPC_TARGET'], '127.0.0.1:50061')
        self.assertEqual(spawn.call_args.args[0][:6], ['/usr/bin/taskset', '-c', '2,3', '/usr/bin/nice', '-n', '5'])
        self.assertEqual(spawn.call_args.args[0][-1], str(controller.binary))
        tls.assert_called_once_with(cafile=str(controller.certificate))
        self.assertEqual(tls.return_value.wrap_socket.call_args.kwargs['server_hostname'], 'localhost')
        process.terminate.assert_not_called()

    def test_tls_failure_drains_only_own_child(self):
        controller = self.start_context()
        process = MagicMock(pid=4321)
        process.poll.return_value = None
        with patch.object(controller, 'owned', return_value=None), \
             patch.object(controller, '_token', return_value='synthetic-test-token'), \
             patch.object(worker.os, 'sched_getaffinity', return_value={0, 1, 2, 3}, create=True), \
             patch.object(worker.stat, 'S_IMODE', return_value=0), \
             patch.object(worker.shutil, 'which', side_effect=lambda x: '/usr/bin/' + x), \
             patch.object(worker.socket, 'socket'), \
             patch.object(worker.socket, 'create_connection'), \
             patch.object(worker.ssl, 'create_default_context') as tls, \
             patch.object(worker, 'process_identity', return_value='987'), \
             patch.object(worker.subprocess, 'Popen', return_value=process):
            tls.return_value.wrap_socket.side_effect = ssl.SSLCertVerificationError('synthetic')
            with self.assertRaisesRegex(RuntimeError, 'certificate validation failed'):
                controller.start()
        process.terminate.assert_called_once()
        process.wait.assert_called_once_with(timeout=30)
        process.kill.assert_not_called()

    def test_preexisting_worker_is_not_adopted(self):
        self.controller._locked = lambda: nullcontext()
        with patch.object(self.controller, 'owned', return_value=123), \
             patch.object(worker.subprocess, 'Popen') as spawn:
            with self.assertRaisesRegex(RuntimeError, 'refusing adoption'):
                self.controller.start()
        spawn.assert_not_called()

    def test_stop_identity_mismatch_sends_no_signal(self):
        self.controller._locked = lambda: nullcontext()
        with patch.object(self.controller, 'owned', side_effect=RuntimeError('identity mismatch')), \
             patch.object(worker.os, 'kill') as send:
            with self.assertRaisesRegex(RuntimeError, 'identity mismatch'):
                self.controller.stop()
        send.assert_not_called()

    def test_stop_is_graceful_and_idempotent(self):
        self.controller._locked = lambda: nullcontext()
        with patch.object(self.controller, 'owned', side_effect=[123, None, None]), \
             patch.object(worker.os, 'kill') as send:
            self.controller.stop()
        send.assert_called_once_with(123, signal.SIGTERM)
        with patch.object(self.controller, 'owned', return_value=None), \
             patch.object(worker.os, 'kill') as send:
            self.controller.stop()
        send.assert_not_called()


if __name__ == '__main__':
    unittest.main()
