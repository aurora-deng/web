"""Opt-in daily ONNX worker lifecycle; no database, downloads or system services.

TLS probes establish transport readiness only, not model load or authentication.
The caller must obtain deployment approval before executing start/stop on a VM.
"""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import ssl
import stat
import subprocess
import time


def process_identity(proc, binary):
    """Exact executable and Linux start ticks, including names with spaces."""
    try:
        command = (proc / 'cmdline').read_bytes().split(b'\0')[0]
        fields = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
        if fields[0] == 'Z':
            return None
        if command != os.fsencode(binary):
            raise RuntimeError('Worker PID executable mismatch; refusing control')
        return fields[19]
    except FileNotFoundError:
        return None


def cpu_set(value):
    if not value or len(value) > 128:
        raise ValueError('Worker CPU list is required and bounded')
    parts = value.split(',')
    if any(not part.isascii() or not part.isdecimal() for part in parts):
        raise ValueError('Worker CPUs must be comma-separated nonnegative integers')
    values = {int(part) for part in parts}
    if len(values) != len(parts) or len(values) > 64:
        raise ValueError('Worker CPU list contains duplicates or too many CPUs')
    return values


def worker_environment(base, model_root, certificate, private_key, token, port):
    # Only runtime search paths and locale; no DB, cloud, proxy or telemetry data.
    allowed = ('PATH', 'LD_LIBRARY_PATH', 'LANG', 'LC_ALL', 'TZ')
    env = {key: base[key] for key in allowed if key in base}
    env.update({
        'WEB_PRODUCTION_MODE': '1',
        'WEB_PHASE11_ONNX_MODEL_ROOT': str(model_root),
        'WEB_PHASE11_ONNX_WORKERS': '1', 'WEB_PHASE11_ONNX_MODELS': '1',
        'WEB_PHASE11_ONNX_QUEUE': '2',
        'WEB_PHASE11_ONNX_WORKER_ADDRESS': f'127.0.0.1:{port}',
        'WEB_PHASE11_ONNX_RPC_TOKEN': token,
        'WEB_PHASE11_ONNX_WORKER_TLS_CERT': str(certificate),
        'WEB_PHASE11_ONNX_WORKER_TLS_KEY': str(private_key),
    })
    return env


def client_environment(certificate, token, port):
    return {
        'WEB_PHASE11_ONNX_GRPC_TARGET': f'127.0.0.1:{port}',
        'WEB_PHASE11_ONNX_GRPC_TLS_CA': str(certificate),
        'WEB_PHASE11_ONNX_GRPC_TLS_NAME': 'localhost',
        'WEB_PHASE11_ONNX_RPC_TOKEN': token,
        'WEB_PHASE11_ONNX_GRPC_WORKERS': '2',
        'WEB_PHASE11_ONNX_GRPC_QUEUE': '8',
    }


class WorkerController:
    def __init__(self, source, runtime, *, cpus='2,3', port=50061,
                 model_root=Path('/home/pikachu/phase11-models')):
        self.source, self.runtime = Path(source), Path(runtime)
        self.root = self.runtime / 'onnx-worker'
        self.binary = self.source / 'build-release/webserver-onnx-worker'
        self.certificate = self.runtime / 'localhost.crt'
        self.private_key = self.runtime / 'localhost.key'
        self.model_root = Path(model_root)
        self.cpus, self.port = cpu_set(cpus), port
        if not 1 <= port <= 65535:
            raise ValueError('Invalid worker loopback port')

    @contextmanager
    def _locked(self):
        import fcntl  # Keep pure plan/identity helpers testable on Windows.
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.root.is_symlink() or stat.S_IMODE(self.root.stat().st_mode) & 0o077:
            raise RuntimeError('Worker runtime directory must be private and not a symlink')
        fd = os.open(self.root / 'control.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield

    def owned(self):
        path = self.root / 'process.json'
        if not path.exists():
            return None
        if path.is_symlink() or path.stat().st_size > 1024:
            raise RuntimeError('Invalid worker identity record')
        record = json.loads(path.read_text())
        pid = record['pid']
        if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 1:
            raise RuntimeError('Invalid worker PID')
        ticks = process_identity(Path('/proc') / str(pid), self.binary)
        if ticks is None:
            return None
        if ticks != record['ticks']:
            raise RuntimeError('Worker PID start ticks changed; refusing control')
        return pid

    def _token(self):
        path = self.root / 'rpc-token'
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(fd, 'w') as output:
                output.write(secrets.token_hex(32) + '\n')
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(fd, 'r') as handle:
            info = os.fstat(handle.fileno())
            if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) & 0o077:
                raise RuntimeError('Worker token must be a private regular file')
            value = handle.read(130).strip()
        if len(value) != 64 or any(c not in '0123456789abcdef' for c in value):
            raise RuntimeError('Invalid worker token file; no value disclosed')
        return value

    def start(self):
        with self._locked():
            if self.owned():
                # Refuse adoption: caller rollback must never stop a pre-existing worker.
                raise RuntimeError('Daily ONNX worker already running; refusing adoption')
            for path in (self.binary, self.certificate, self.private_key):
                if not path.is_file() or path.is_symlink():
                    raise RuntimeError('Existing worker build and TLS files required')
            if not self.model_root.is_dir():
                raise RuntimeError('Existing approved model root required')
            if stat.S_IMODE(self.private_key.stat().st_mode) & 0o077:
                raise RuntimeError('Worker TLS private key permissions are too broad')
            if not self.cpus.issubset(os.sched_getaffinity(0)):
                raise RuntimeError('Requested worker CPUs are outside allowed affinity')
            taskset, nice = shutil.which('taskset'), shutil.which('nice')
            if not taskset or not nice:
                raise RuntimeError('Existing taskset and nice are required; no installation')
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', self.port))
            token = self._token()
            env = worker_environment(os.environ, self.model_root, self.certificate,
                                     self.private_key, token, self.port)
            command = [taskset, '-c', ','.join(map(str, sorted(self.cpus))),
                       nice, '-n', '5', str(self.binary)]
            fd = os.open(self.root / 'worker.log', os.O_WRONLY | os.O_CREAT |
                         os.O_APPEND | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'ab') as log:
                process = subprocess.Popen(command, cwd=self.source, env=env,
                                           stdout=log, stderr=subprocess.STDOUT,
                                           start_new_session=True)
            try:
                deadline = time.monotonic() + 20
                ticks = None
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError('ONNX worker exited; inspect private log')
                    try:
                        ticks = process_identity(Path('/proc') / str(process.pid), self.binary)
                        if ticks:
                            break
                    except RuntimeError:  # taskset/nice has not exec'd the worker yet.
                        pass
                    time.sleep(.02)
                if not ticks:
                    raise RuntimeError('Worker exec identity timed out')
                record = self.root / 'process.json'
                if record.is_symlink():
                    raise RuntimeError('Worker identity record must not be a symlink')
                temporary = self.root / ('process-' + secrets.token_hex(8) + '.tmp')
                with temporary.open('x') as output:
                    output.write(json.dumps({'pid': process.pid, 'ticks': ticks}))
                temporary.replace(record)
                context = ssl.create_default_context(cafile=str(self.certificate))
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError('ONNX worker exited; inspect private log')
                    try:
                        with socket.create_connection(('127.0.0.1', self.port), timeout=.2) as raw:
                            with context.wrap_socket(raw, server_hostname='localhost'):
                                return client_environment(self.certificate, token, self.port)
                    except ssl.SSLCertVerificationError:
                        raise RuntimeError('Worker TLS certificate validation failed') from None
                    except (OSError, ssl.SSLError):
                        time.sleep(.1)
                raise RuntimeError('Worker TLS transport readiness timed out')
            except BaseException:
                # This Popen owns this exact child; never kill arbitrary ports/PIDs.
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=30)
                    except subprocess.TimeoutExpired:
                        raise RuntimeError('Worker rollback drain timed out; no forced kill') from None
                raise

    def stop(self):
        with self._locked():
            pid = self.owned()
            if pid is None:
                return
            os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + 30
            while self.owned() and time.monotonic() < deadline:
                time.sleep(.1)
            if self.owned():
                raise RuntimeError('Worker drain timed out; no forced kill performed')
