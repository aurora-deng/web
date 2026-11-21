"""M02 owner-only desktop supervisor; no imports of the live server or config.

Parent integration (not performed here):
  PrepServer.__init__, after assigning guard.export_assistant:
    self.desktop = DesktopOwnerService(DATA, self.storage.guard,
        lambda: self.storage.snapshot()['revision'],
        lambda: self.private.status() if self.private.config.get('enabled')
                else read_remote_status())
  Import DesktopOwnerService, desktop_api from this module in server.py.
  Handler.do_GET, immediately after computing path:
    if desktop_api(self, path): return
  Handler.do_POST, after same_origin() and computing path:
    if desktop_api(self, path): return

The route adapter enforces require_auth AND is_pc even for status. Never make
these endpoints available via a phone relay or add process/path arguments.
GET /api/desktop/status; POST /api/desktop/settings {recoveryEnabled: bool,
loginStartupRequested: bool, confirmation: 'ENABLE_LOGIN_STARTUP'};
POST /api/desktop/prepare-upgrade {confirmation: 'PREPARE_UPGRADE'};
POST /api/desktop/cancel-upgrade {}. Tests default to an intent-only adapter;
the real local host installs the current-user login entry only after confirmation.
For an actual update the parent must call desktop.gate.apply(ticket, callback,
quiesce) in trusted local code. quiesce MUST hold all data writers, background
backup/retention, and supervisor recovery paused for the entire callback.
No JSON endpoint executes an updater. No package/download paths are accepted.
"""

import argparse
import ctypes
import hashlib
import http.client
import json
import math
import os
import queue
import secrets
import socket
import struct
import subprocess
import sys
import threading
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath


class DesktopError(ValueError):
    pass


def read_object(path, default=None):
    try:
        if path.stat().st_size > 65536:
            raise DesktopError('Desktop state is too large')
        value = json.loads(path.read_text(encoding='utf-8-sig'))
        if not isinstance(value, dict):
            raise DesktopError('Desktop state is invalid')
        return value
    except FileNotFoundError:
        return dict(default or {})


def atomic_object(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.' + secrets.token_hex(6) + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8') as stream:
            json.dump(value, stream, separators=(',', ':'), allow_nan=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


class DesktopConfig:
    def __init__(self, data):
        self.path = Path(data) / 'desktop-config.json'
        self.lock = threading.RLock()

    def status(self):
        value = read_object(self.path)
        return {'recoveryEnabled': value.get('recoveryEnabled', True) is True,
                'loginStartupRequested': value.get('loginStartupRequested') is True,
                'loginStartupApplied': False, 'loginStartupSupported': False}

    def configure(self, body, *, is_pc=False):
        if is_pc is not True:
            raise PermissionError('PC-local confirmation is required')
        if not isinstance(body, dict) or set(body) - {
                'recoveryEnabled', 'loginStartupRequested', 'confirmation'}:
            raise DesktopError('Unsupported desktop setting')
        with self.lock, SupervisorLease(self.path.parent, 'desktop-config.lock'):
            value = self.status()
            for name in ('recoveryEnabled', 'loginStartupRequested'):
                if name in body and type(body[name]) is not bool:
                    raise DesktopError('Desktop settings must be booleans')
            if body.get('loginStartupRequested') is True and body.get('confirmation') != 'ENABLE_LOGIN_STARTUP':
                raise DesktopError('Explicit login startup confirmation is required')
            for name in ('recoveryEnabled', 'loginStartupRequested'):
                if name in body:
                    value[name] = body[name]
            atomic_object(self.path, {key: value[key] for key in
                                     ('recoveryEnabled', 'loginStartupRequested')})
            return self.status()


def backup_summary(value):
    value = value if isinstance(value, dict) else {}
    last = value.get('lastSuccess')
    valid = type(last) in (int, float) and math.isfinite(last) and last > 0
    return {'state': 'failed' if value.get('error') else 'verified' if valid else 'unknown',
            'lastSuccess': last if valid else None,
            **{key: value[key] for key in ('externalFiles', 'missingExternalFiles', 'externalResourceNotice') if key in value}}


def remote_summary(value):
    value = value if isinstance(value, dict) else {}
    return {'state': 'failed' if value.get('error') else 'active' if value.get('active') is True
            else 'inactive', 'kind': 'private' if value.get('kind') == 'private' else 'remote'}


def verify_backup(data, result):
    """Verify the learning ZIP, not the separately stored resource originals."""
    from data_guard import external_resource_summary, validate_resource_reference
    if not isinstance(result, dict) or result.get('error') or not result.get('lastSuccess'):
        raise DesktopError('Upgrade blocked: backup did not succeed')
    identity = result.get('id', '')
    if not isinstance(identity, str) or not identity.startswith('yantu-') or not identity.endswith('.zip') or Path(identity).name != identity or '/' in identity or '\\' in identity:
        raise DesktopError('Upgrade blocked: invalid backup identity')
    folder = (Path(data) / 'backups').resolve()
    path = folder / identity
    if path.is_symlink() or path.resolve().parent != folder or not path.is_file():
        raise DesktopError('Upgrade blocked: backup is missing or redirected')
    required = {'\u6570\u636e.json', '\u52a9\u624b\u8bb0\u5fc6\u4e0e\u5bf9\u8bdd.json',
                '\u4fdd\u62a4\u8bb0\u5f55.json', '\u9644\u4ef6\u6e05\u5355.json'}
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) > 100000 or sum(item.file_size for item in archive.infolist()) > 20 * 1024 ** 3:
            raise DesktopError('Upgrade blocked: backup exceeds verification limits')
        if len(names) != len(set(names)) or not required <= set(names) or archive.testzip():
            raise DesktopError('Upgrade blocked: incomplete backup')
        if any(archive.getinfo(name).file_size > 32 * 1024 ** 2 for name in required):
            raise DesktopError('Upgrade blocked: backup metadata exceeds verification limits')
        snapshot = json.loads(archive.read('\u6570\u636e.json'))
        if not isinstance(snapshot, dict) or snapshot.get('version') != 3 or not isinstance(snapshot.get('state'), dict) or type(snapshot.get('revision')) is not int:
            raise DesktopError('Upgrade blocked: invalid study snapshot')
        manifest = json.loads(archive.read('\u9644\u4ef6\u6e05\u5355.json'))
        if not isinstance(manifest, dict):
            raise DesktopError('Upgrade blocked: invalid attachment manifest')
        expected, references = set(required), []
        for identity, info in manifest.items():
            if not isinstance(info, dict):
                raise DesktopError('Upgrade blocked: invalid attachment entry')
            if info.get('storage') is not None or info.get('library') is True or info.get('included') is False:
                try:
                    references.append({**validate_resource_reference(identity, info),
                                       'available': None, 'status': 'not-verified'})
                except ValueError as error:
                    raise DesktopError('Upgrade blocked: invalid external resource reference') from error
                continue
            if not identity or len(identity) > 240 or identity.endswith('.') or PureWindowsPath(identity).is_reserved() or any(character not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-' for character in identity) or info.get('path') != 'files/' + identity:
                raise DesktopError('Upgrade blocked: invalid attachment entry')
            expected.add(info['path'])
            with archive.open(info['path']) as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            if digest != info.get('sha256') or archive.getinfo(info['path']).file_size != info.get('size'):
                raise DesktopError('Upgrade blocked: attachment verification failed')
        if set(names) != expected:
            raise DesktopError('Upgrade blocked: unexpected backup entries')
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'id': path.name, 'sha256': digest, 'revision': snapshot['revision'],
            'externalOriginalsVerified': False,
            **external_resource_summary(references)}


class UpgradeGate:
    def __init__(self, data, backup, revision, clock=time.time):
        self.data, self.backup, self.revision, self.clock = Path(data), backup, revision, clock
        self.lock, self.pending = threading.RLock(), None

    def prepare(self):
        with self.lock:
            self.pending = None
            before = self.revision()
            result = self.backup()
            verified = verify_backup(self.data, result)
            if type(before) is not int or before != self.revision() or before != verified['revision']:
                raise DesktopError('Upgrade blocked: data changed during backup; retry')
            self.pending = {'ticket': secrets.token_urlsafe(32), 'revision': before,
                            'expires': self.clock() + 120, 'backup': verified, 'result': result}
            return self.status()

    def status(self):
        with self.lock:
            if self.pending and self.clock() >= self.pending['expires']:
                self.pending = None
            if not self.pending:
                return {'prepared': False, 'updateExecuted': False}
            return {'prepared': True, 'updateExecuted': False, 'backupVerified': True,
                    **{key: self.pending['backup'][key] for key in
                       ('externalFiles', 'externalResourceNotice', 'externalOriginalsVerified')},
                    **{key: self.pending[key] for key in ('ticket', 'expires', 'revision')}}

    def cancel(self):
        with self.lock:
            self.pending = None
            return self.status()

    def apply(self, ticket, callback, quiesce):
        """Trusted parent only; never reachable from the JSON command dispatcher."""
        with self.lock:
            pending = self.pending
            self.pending = None  # Every attempt consumes the ticket, including failures.
            if not pending or not isinstance(ticket, str) or not secrets.compare_digest(ticket, pending['ticket']) or self.clock() >= pending['expires']:
                raise DesktopError('Upgrade blocked: prepare a fresh backup')
            with quiesce():
                if self.clock() >= pending['expires'] or pending['revision'] != self.revision() or verify_backup(self.data, pending['result']) != pending['backup']:
                    raise DesktopError('Upgrade blocked: backup or data changed')
                return callback()


class DesktopOwnerService:
    """No process control on HTTP. Native tray owns its private stdio channel."""
    def __init__(self, data, guard, revision, remote=lambda: {}, startup=None):
        self.data, self.guard, self.remote = Path(data), guard, remote
        self.startup = startup
        self.config = DesktopConfig(data)
        self.gate = UpgradeGate(data, lambda: guard.backup(force=True), revision)

    def status(self):
        protection = self.guard.status(local=True)
        runtime = read_object(self.data / 'desktop-state.json')
        return {'local': True, 'server': 'running', 'phase': 'running', 'settings': {**self.config.status(), **(self.startup.status() if self.startup else {})},
                'supervisor': {'desired': runtime.get('desired') is True,
                               'attempts': min(3, max(0, int(runtime.get('attempts', 0))))},
                'remote': remote_summary(self.remote()),
                'backup': backup_summary(protection.get('backup')),
                'secondary': backup_summary(protection.get('secondary')),
                'upgrade': self.gate.status()}


def desktop_api(handler, path):
    """Return True when the reserved route was handled, including denials."""
    if not path.startswith('/api/desktop/'):
        return False
    if not handler.require_auth():
        return True
    if handler.is_pc is not True:
        handler.reply(403, {'error': 'Desktop management is PC-local only'})
        return True
    try:
        service = handler.services.desktop
        if handler.command in {'GET', 'HEAD'} and path == '/api/desktop/status':
            result = service.status()
        elif handler.command == 'POST' and path in {
                '/api/desktop/settings', '/api/desktop/prepare-upgrade', '/api/desktop/cancel-upgrade'}:
            if not handler.same_origin():
                return True
            body = json.loads(handler.read_body(4096))
            if not isinstance(body, dict):
                raise DesktopError('Invalid desktop request')
            if path.endswith('/settings'):
                from login_startup import configure
                result = configure(service.config, body, True, getattr(service, 'startup', None))
            elif path.endswith('/prepare-upgrade'):
                if body != {'confirmation': 'PREPARE_UPGRADE'}:
                    raise DesktopError('Upgrade preparation confirmation required')
                result = service.gate.prepare()
            else:
                if body:
                    raise DesktopError('Unsupported cancel request')
                result = service.gate.cancel()
        else:
            handler.reply(404, {'error': 'Unknown desktop operation'})
            return True
        handler.reply(200, result)
    except Exception:
        # Native errors and filesystem paths must not cross the HTTP boundary.
        handler.reply(409, {'error': 'Desktop operation was blocked; check locally'})
    return True


@dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    image: str
    created: int


class WindowsInspector:
    def identity(self, pid):
        if os.name != 'nt' or type(pid) is not int or pid <= 0:
            return None
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.QueryFullProcessImageNameW.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
        kernel.GetProcessTimes.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return None
        try:
            name, size = ctypes.create_unicode_buffer(32768), ctypes.c_uint32(32768)
            stamps = [ctypes.c_uint64() for _ in range(4)]
            if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)) or not kernel.GetProcessTimes(handle, *(ctypes.byref(v) for v in stamps)):
                return None
            return ProcessIdentity(pid, os.path.normcase(str(Path(name.value).resolve())), stamps[0].value)
        finally:
            kernel.CloseHandle(handle)

    def listener(self, port):
        if os.name != 'nt':
            return None
        api = ctypes.WinDLL('iphlpapi', use_last_error=True)
        size = ctypes.c_uint32(0)
        api.GetExtendedTcpTable.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
        code = api.GetExtendedTcpTable(None, ctypes.byref(size), False, socket.AF_INET, 3, 0)
        if code != 122:
            return None
        for _ in range(3):
            buffer = ctypes.create_string_buffer(size.value)
            code = api.GetExtendedTcpTable(buffer, ctypes.byref(size), False, socket.AF_INET, 3, 0)
            if code == 122:
                continue
            if code:
                return None
            raw = buffer.raw
            count = struct.unpack_from('<I', raw)[0]
            pids = {row[5] for row in (struct.unpack_from('<6I', raw, 4 + index * 24) for index in range(count))
                    if row[0] == 2 and socket.ntohs(row[2] & 65535) == port
                    and row[1] in {0, struct.unpack('<I', socket.inet_aton('127.0.0.1'))[0]}}
            return pids.pop() if len(pids) == 1 else None
        return None


class LocalBackend:
    def __init__(self, root, data, port, inspector=None, clock=time.time, sleep=time.sleep):
        self.root, self.data, self.port = Path(root).resolve(), Path(data).resolve(), port
        if type(port) is not int or not 1 <= port <= 65535:
            raise DesktopError('Invalid application port')
        self.image = os.path.normcase(str((self.root / 'pc' / 'yantu_pc_001.exe').resolve()))
        self.inspector = inspector or WindowsInspector()
        self.clock, self.sleep, self.child = clock, sleep, None
        self._session, self._session_identity = None, None

    def call(self, path, body=None, token=None, timeout=3):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=timeout)
        headers = {'Content-Type': 'application/json', 'Connection': 'close'}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        try:
            connection.request('GET' if body is None else 'POST', path,
                               None if body is None else json.dumps(body), headers)
            response = connection.getresponse()
            maximum = (16 if path.split('?', 1)[0] == '/api/state' else 4) * 1024 * 1024
            raw = response.read(maximum + 1)
            if response.status == 401:
                self._session, self._session_identity = None, None
            if response.status != 200 or len(raw) > maximum:
                raise DesktopError('Local API is not ready')
            value = json.loads(raw)
            if not isinstance(value, dict):
                raise DesktopError('Invalid local response')
            return value
        finally:
            connection.close()

    def probe(self):
        try:
            runtime = read_object(self.data / 'runtime.json')
            identity = self.inspector.identity(runtime.get('pid'))
            health = self.call('/api/health')
            owned = (runtime.get('port') == self.port and runtime.get('root')
                     and Path(runtime['root']).resolve() == self.root and identity
                     and identity.pid == runtime.get('pid')
                     and identity.image == self.image and identity.created > 0
                     and self.inspector.listener(self.port) == identity.pid
                     and health.get('app') == 'yantu-local' and health.get('dataPath')
                     and Path(health['dataPath']).resolve() == self.data)
            # Recheck after HTTP, so PID reuse and listener switches fail closed.
            if owned and self.inspector.identity(identity.pid) == identity:
                return {'state': 'running', 'identity': identity}
            return {'state': 'unowned'}
        except ConnectionRefusedError:
            # An existing main process with no HTTP listener must not be duplicated.
            if identity:
                return {'state': 'starting' if identity.image == self.image else 'unowned'}
            return {'state': 'starting' if self.child and self.child.poll() is None else 'stopped'}
        except (OSError, ValueError, http.client.HTTPException, TypeError):
            return {'state': 'unavailable'}

    def assert_owned(self, expected):
        value = self.probe()
        if value.get('identity') != expected or value['state'] != 'running':
            raise DesktopError('Process ownership changed; no process was stopped')

    def token(self, expected):
        self.assert_owned(expected)
        if self._session_identity == expected and self._session:
            return self._session
        token = self.call('/api/pair', {}).get('deviceToken')
        if not isinstance(token, str) or not token:
            raise DesktopError('Local authorization failed')
        self._session, self._session_identity = token, expected
        return token

    def launch(self):
        # Never run an existing launcher, compiler or private-connection proxy.
        if self.probe()['state'] != 'stopped':
            raise DesktopError('Port is occupied or ownership is uncertain')
        if not Path(self.image).is_file():
            raise DesktopError('Existing PC host is missing; no dependency was installed')
        self.data.mkdir(parents=True, exist_ok=True)
        with (self.data / 'desktop-server.log').open('ab') as output:
            self.child = subprocess.Popen([self.image, '--server', '--port', str(self.port),
                                           '--host', '0.0.0.0', '--data-dir', str(self.data)],
                                          stdin=subprocess.DEVNULL, stdout=output, stderr=output,
                                          cwd=self.root, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        # Keep the handle alive until poll confirms exit; never PID-kill a child.

    def shutdown(self, expected):
        token = self.token(expected)
        self.assert_owned(expected)
        self.call('/api/shutdown', {}, token)
        until = self.clock() + 12
        while self.clock() < until:
            if self.inspector.identity(expected.pid) != expected:
                return
            self.sleep(.2)
        raise DesktopError('Server did not exit; restart was blocked (no forced kill)')

    def details(self, expected):
        token = self.token(expected)
        info = self.call('/api/info', token=token)
        protection = self.call('/api/protection', token=token)
        self.assert_owned(expected)
        return {'remote': remote_summary(info.get('remote')),
                'backup': backup_summary(protection.get('backup')),
                'secondary': backup_summary(protection.get('secondary'))}

    def backup(self):
        probe = self.probe()
        if probe['state'] != 'running':
            raise DesktopError('An owned running server is required for backup')
        expected = probe['identity']
        token = self.token(expected)
        result = self.call('/api/protection/backup', {}, token, timeout=300)
        self.assert_owned(expected)
        return result

    def revision(self):
        probe = self.probe()
        if probe['state'] != 'running':
            raise DesktopError('Owned server is unavailable')
        token = self.token(probe['identity'])
        result = self.call('/api/info', token=token)['revision']
        self.assert_owned(probe['identity'])
        return result


class DesktopRuntime:
    MAX_ATTEMPTS = 3
    RETRY_DELAYS = (2, 5, 15)

    def __init__(self, data, backend, clock=time.time, startup=None):
        self.data, self.backend, self.clock = Path(data), backend, clock
        self.startup = startup
        self.config, self.lock = DesktopConfig(data), threading.RLock()
        self.path = self.data / 'desktop-state.json'
        state = read_object(self.path)
        self.desired = state.get('desired') is True
        attempts = state.get('attempts', 0)
        self.attempts = min(3, max(0, attempts)) if type(attempts) is int else 3
        retry = state.get('nextRetry', 0)
        self.next_retry = retry if type(retry) in (int, float) and math.isfinite(retry) else 0
        self.next_retry = min(self.next_retry, self.clock() + 15)
        deadline = state.get('startDeadline', 0)
        if type(deadline) not in (int, float) or not math.isfinite(deadline):
            raise DesktopError('Desktop startup state is invalid')
        self.start_deadline = min(deadline, self.clock() + 15)
        self.error, self.healthy_since = '', None
        self.phase = 'starting' if self.start_deadline > self.clock() else 'idle'
        self.gate = UpgradeGate(data, backend.backup, backend.revision, clock)
        maintenance = state.get('maintenanceUntil', 0)
        if type(maintenance) not in (int, float) or not math.isfinite(maintenance):
            raise DesktopError('Desktop maintenance state is invalid')
        self.maintenance_until = min(maintenance, self.clock() + 600)

    def save(self):
        atomic_object(self.path, {'desired': self.desired, 'attempts': self.attempts,
                                 'nextRetry': self.next_retry, 'maintenanceUntil': self.maintenance_until,
                                 'startDeadline': self.start_deadline})

    def _failed(self, message):
        self.error, self.phase, self.healthy_since = message, 'failed', None
        self.start_deadline = 0
        self.next_retry = self.clock() + self.RETRY_DELAYS[min(max(self.attempts - 1, 0), 2)]
        self.save()

    def _launch(self):
        self.attempts += 1
        self.start_deadline = self.clock() + 15
        self.phase, self.error = 'starting', ''
        self.save()  # Persist the attempt before spawning, including across tray restarts.
        try:
            self.backend.launch()
        except Exception:
            self._failed('Start failed; check the desktop server log')

    def tick(self):
        with self.lock:
            probe = self.backend.probe()
            now = self.clock()
            if probe['state'] == 'running':
                self.phase = 'running'
                self.start_deadline = 0
                if self.healthy_since is None:
                    self.healthy_since = now
                if now - self.healthy_since >= 60 and (self.attempts or self.error):
                    self.attempts, self.next_retry, self.error = 0, 0, ''
                    self.save()
                return
            self.healthy_since = None
            if self.phase == 'starting':
                if now < self.start_deadline:
                    return
                self._failed('Startup timed out; no process was forcibly stopped')
            if probe['state'] in {'unowned', 'unavailable', 'starting'}:
                self.phase = 'blocked'
                self.error = 'Port occupied or ownership uncertain; process control blocked'
                return
            if self.phase == 'running':
                self._failed('Server exited; bounded recovery is pending')
            if not self.desired:
                self.phase = 'stopped'
                return
            if not self.config.status()['recoveryEnabled'] or now < self.maintenance_until:
                self.phase = 'paused'
                return
            if self.attempts >= self.MAX_ATTEMPTS:
                self.phase = 'exhausted'
                return
            if now >= self.next_retry:
                self._launch()

    def status(self):
        with self.lock:
            probe = self.backend.probe()
            details = {'remote': {'state': 'unknown'}, 'backup': {'state': 'unknown'},
                       'secondary': {'state': 'unknown'}}
            if probe['state'] == 'running':
                try:
                    details = self.backend.details(probe['identity'])
                except Exception:
                    self.error = 'Status refresh failed; last operation was not repeated'
            return {'local': True, 'server': probe['state'], 'phase': self.phase,
                    'desired': self.desired, 'attempts': self.attempts,
                    'maxAttempts': self.MAX_ATTEMPTS, 'nextRetry': self.next_retry,
                    'error': self.error, 'settings': {**self.config.status(), **(self.startup.status() if self.startup else {})},
                    'upgrade': self.gate.status(), **details}

    def command(self, action, body=None, *, is_pc=False):
        if is_pc is not True:
            raise PermissionError('Desktop process control is PC-local only')
        body = {} if body is None else body
        if not isinstance(body, dict):
            raise DesktopError('Desktop command body must be an object')
        with self.lock:
            if action == 'status' and not body:
                return self.status()
            if action == 'settings':
                from login_startup import configure
                configure(self.config, body, True, self.startup)
                return self.status()
            if action == 'prepare-upgrade' and body == {'confirmation': 'PREPARE_UPGRADE'}:
                self.maintenance_until = self.clock() + 600
                self.save()
                try:
                    self.gate.prepare()
                except Exception:
                    self.maintenance_until = 0
                    self.save()
                    raise
                return self.status()
            if action == 'cancel-upgrade' and not body:
                self.gate.cancel()
                self.maintenance_until = 0
                self.save()
                return self.status()
            if action not in {'start', 'stop', 'restart'} or body:
                raise DesktopError('Unknown desktop command')
            if self.clock() < self.maintenance_until:
                raise DesktopError('Cancel upgrade preparation before changing server state')
            probe = self.backend.probe()
            if probe['state'] not in {'running', 'stopped'}:
                raise DesktopError('Process ownership is uncertain; no process was changed')
            if action in {'stop', 'restart'}:
                self.desired, self.phase = False, 'stopping'
                self.save()  # A failed shutdown must not enable recovery.
                if probe['state'] == 'running':
                    try:
                        self.backend.shutdown(probe['identity'])
                    except Exception:
                        self._failed('Shutdown failed; restart cancelled, no forced kill')
                        raise
                if self.backend.probe()['state'] != 'stopped':
                    raise DesktopError('Port was taken over; restart cancelled')
                self.phase = 'stopped'
            if action in {'start', 'restart'}:
                self.desired, self.error = True, ''
                self.attempts, self.next_retry = 0, 0
                self.save()
                if action == 'restart' or probe['state'] == 'stopped':
                    self._launch()
                else:
                    self.phase = 'running'
            return self.status()


class SupervisorLease:
    """OS-released exclusive lock; a stale file is not evidence of ownership."""
    def __init__(self, data, name='desktop-supervisor.lock'):
        if name not in {'desktop-supervisor.lock', 'desktop-config.lock'}:
            raise DesktopError('Unsupported desktop lease')
        self.path, self.stream = Path(data) / name, None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.stream = self.path.open('a+b')
        try:
            if os.fstat(self.stream.fileno()).st_size == 0:
                self.stream.write(b'0')
                self.stream.flush()
            self.stream.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.stream.close()
            raise DesktopError('Another local owner operation holds the desktop lease') from None
        return self

    def __exit__(self, *_):
        self.stream.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(self.stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.stream.fileno(), fcntl.LOCK_UN)
        finally:
            self.stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--data-dir', required=True)
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('The desktop supervisor requires Windows')
    messages = queue.Queue(maxsize=32)

    def read_commands():
        try:
            while True:
                line = sys.stdin.buffer.readline(4098)
                if not line:
                    break
                if len(line) > 4096:
                    break
                messages.put(line)
        finally:
            messages.put(None)

    with SupervisorLease(args.data_dir):
        backend = LocalBackend(args.root, args.data_dir, args.port)
        from login_startup import LoginStartup
        runtime = DesktopRuntime(args.data_dir, backend, startup=LoginStartup(args.root, args.data_dir, args.port))
        threading.Thread(target=read_commands, name='desktop-input', daemon=True).start()
        next_tick = 0
        while True:
            if time.monotonic() >= next_tick:
                try:
                    runtime.tick()
                except Exception:
                    runtime.error = 'Recovery paused: local runtime state could not be verified'
                next_tick = time.monotonic() + 1
            try:
                raw = messages.get(timeout=.5)
            except queue.Empty:
                continue
            if raw is None:
                break  # Tray exit never stops the main server or the private proxy.
            identity = None
            try:
                message = json.loads(raw)
                identity = message.get('id')
                if type(identity) is not int or set(message) - {'id', 'action', 'body'}:
                    raise DesktopError('Invalid desktop command')
                result = runtime.command(message.get('action'), message.get('body'), is_pc=True)
                response = {'id': identity, 'ok': True, 'result': result}
            except Exception:
                response = {'id': identity, 'ok': False, 'error': 'Operation blocked; check ownership, backup and desktop state'}
            print(json.dumps(response, ensure_ascii=True, allow_nan=False), flush=True)


if __name__ == '__main__':
    main()
