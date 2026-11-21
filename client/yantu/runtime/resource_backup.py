"""Bounded-memory resource backup archives and verified, on-demand recovery."""

import hashlib
import json
import os
import secrets
import shutil
import stat
import tempfile
import threading
import time
import zipfile
from functools import lru_cache
from pathlib import Path

from library_sources import Catalog, _checked_file, _component, _no_link_status, _unique_object

CHUNK = 256 * 1024
ARCHIVES = 'compressed-resources-v1'
FORMAT = 'yantu-resource-backup-v1'
_MANAGER_LOCK = threading.Lock()


class Interrupted(Exception):
    pass


def _signature(status):
    return status.st_dev, status.st_ino, status.st_size, status.st_mtime_ns


def _read_json(path, limit=4 * 1024 * 1024):
    status = _no_link_status(path)
    if not stat.S_ISREG(status.st_mode) or status.st_size > limit:
        raise ValueError('资源备份设置或索引无效')
    with path.open('r', encoding='utf-8-sig') as stream:
        return json.load(stream, object_pairs_hook=_unique_object)


def _directory(root, parts=(), create=False):
    root = Path(root)
    current = Path(root.anchor)
    if not stat.S_ISDIR(_no_link_status(current).st_mode):
        raise ValueError('资源盘未连接')
    for part in root.parts[1:]:
        current = current / part
        if not stat.S_ISDIR(_no_link_status(current).st_mode):
            raise ValueError('资源库目录不安全或未连接')
    boundary = root.resolve(strict=True)
    for part in parts:
        if not _component(part):
            raise ValueError('资源备份目录无效')
        current = current / part
        if create:
            try:
                current.mkdir(mode=0o700)
            except FileExistsError:
                pass
        if not stat.S_ISDIR(_no_link_status(current).st_mode):
            raise ValueError('资源备份目录不安全')
    resolved = current.resolve(strict=True)
    if resolved != boundary and not resolved.is_relative_to(boundary):
        raise ValueError('资源备份目录越界')
    return resolved


def _atomic_json(path, value):
    parent = _directory(path.parent)
    try:
        _no_link_status(path)
    except FileNotFoundError:
        pass
    temporary = parent / ('.metadata-' + secrets.token_hex(12) + '.tmp')
    try:
        with temporary.open('x', encoding='utf-8') as stream:
            json.dump(value, stream, ensure_ascii=False, separators=(',', ':'), allow_nan=False)
            stream.flush()
            os.fsync(stream.fileno())
        _directory(path.parent)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def _message(error):
    if isinstance(error, ValueError) and all(token not in str(error) for token in (':\\', ':/', '\\', '/')):
        return str(error)
    if isinstance(error, (zipfile.BadZipFile, RuntimeError, EOFError)):
        return '压缩备份损坏或无法读取，原件未被覆盖'
    return '资源盘读写未完成，请在电脑端检查空间、连接和权限；原件未被覆盖'


class ResourceBackup:
    def __init__(self, data):
        self.data = Path(os.path.abspath(data))
        self.catalog = Catalog(self.data)
        self.config_path = self.data / 'resource-backup-config.json'
        self.state_path = self.data / 'resource-backup-status.json'
        self._lock = threading.RLock()
        self._run_lock = threading.Lock()
        self._restore_lock = threading.Lock()
        self._file_locks = {}
        self._stop, self._cancel, self._wake = threading.Event(), threading.Event(), threading.Event()
        self.worker = None
        self._last_scan = 0
        self._last_generation = None
        self._index = {}
        self._index_root = None
        self._status = {'running': False, 'phase': 'idle', 'completed': 0, 'total': 0, 'originalBytes': 0,
                        'archiveBytes': 0, 'rawRetained': 0, 'errorCount': 0, 'errors': [], 'lastError': ''}
        try:
            saved = _read_json(self.state_path)
            self._status.update(saved)
            self._status.update(running=False, currentName='', currentBytes=0)
        except (OSError, ValueError, TypeError):
            pass

    def config(self):
        defaults = {'enabled': False, 'autoRestore': False, 'keepRawCopies': True}
        try:
            value = _read_json(self.config_path, 4096)
        except FileNotFoundError:
            return defaults
        if not isinstance(value, dict) or value.get('format') != FORMAT:
            raise ValueError('资源备份设置无效，尚未改动副本')
        for key in defaults:
            if type(value.get(key)) is not bool:
                raise ValueError('资源备份设置无效，尚未改动副本')
        return {key: value[key] for key in defaults}

    def _plan(self):
        plan = self.catalog.backup_plan()
        primary, backup = plan['primary'], plan['backup']
        if not primary or not backup:
            raise ValueError('尚未登记独立电子资源库')
        try:
            primary, backup = _directory(primary), _directory(backup)
            markers = [_read_json(folder / 'library-marker.json', 4096) for folder in (primary, backup)]
        except (OSError, RuntimeError):
            raise ValueError('资源库未连接或归属标记缺失，尚未改动副本') from None
        if primary == backup or primary.is_relative_to(backup) or backup.is_relative_to(primary):
            raise ValueError('主资源库与备用资源库必须是独立目录')
        if any(folder == Path(folder.anchor) for folder in (primary, backup)) or any(entry['parts'][0] in {ARCHIVES, 'library-marker.json'} for entry in plan['entries'].values()):
            raise ValueError('资源库使用了保留目录，尚未改动副本')
        if any(not isinstance(marker, dict) or marker.get('format') != 'yantu-original-copy-v1' or not marker.get('batch') for marker in markers) or markers[0]['batch'] != markers[1]['batch']:
            raise ValueError('资源库归属标记不一致，尚未改动副本')
        return {**plan, 'primary': primary, 'backup': backup}

    def configure(self, body):
        if not isinstance(body, dict):
            raise ValueError('资源备份设置无效')
        if set(body) - {'enabled', 'autoRestore', 'keepRawCopies', 'confirmation'}:
            raise ValueError('未知的资源备份设置')
        previous = self.config()
        config = {**previous, **{key: body[key] for key in previous if key in body}}
        if any(type(value) is not bool for value in config.values()):
            raise ValueError('资源备份开关无效')
        if config['enabled']:
            self._plan()
        if not config['keepRawCopies'] and (previous['keepRawCopies'] or not self.config_path.exists()) and body.get('confirmation') != 'COMPRESS_RESOURCE_BACKUPS':
            raise ValueError('请确认压缩核验后释放未压缩备用副本；主资源库不删除')
        _atomic_json(self.config_path, {'format': FORMAT, **config})
        if config['enabled']:
            self._cancel.clear()
            self._last_scan = 0
        else:
            self._cancel.set()
        self._wake.set()
        return self.status()

    def _update(self, persist=False, **values):
        with self._lock:
            self._status.update(values)
            if persist:
                _atomic_json(self.state_path, self._status)

    def status(self):
        config = self.config()
        with self._lock:
            status = dict(self._status)
        return {**status, **config, **self.catalog.backup_summary(), 'savedBytes': max(0, status['originalBytes'] - status['archiveBytes']),
                'local': False, 'chunkBytes': CHUNK}

    def _lock_for(self, sha):
        with self._lock:
            return self._file_locks.setdefault(sha, threading.Lock())

    def _checkpoint(self):
        if self._stop.is_set() or self._cancel.is_set():
            raise Interrupted()

    def _archive_parts(self, entry):
        sha = entry['info']['sha256'].lower()
        return ARCHIVES, sha[:2], sha + '.zip'

    def _archive(self, plan, entry):
        return _checked_file(plan['backup'], self._archive_parts(entry))

    def _open_archive(self, path, entry):
        archive = zipfile.ZipFile(path)
        try:
            members = archive.infolist()
            if len(members) != 2 or {item.filename for item in members} != {'metadata.json', 'resource'}:
                raise ValueError('资源压缩包结构无效')
            info, metadata = archive.getinfo('resource'), archive.getinfo('metadata.json')
            if info.file_size != entry['info']['size'] or metadata.file_size > 65536 or any(item.flag_bits & 1 or item.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) for item in members):
                raise ValueError('资源压缩包尺寸或格式无效')
            with archive.open(metadata) as stream:
                document = json.loads(stream.read(65537).decode('utf-8'), object_pairs_hook=_unique_object)
            if not isinstance(document, dict) or document.get('format') != FORMAT or document.get('sha256') != entry['info']['sha256'].lower() or type(document.get('size')) is not int or document['size'] != entry['info']['size']:
                raise ValueError('压缩备份与登记原件不一致')
            return archive
        except BaseException:
            archive.close()
            raise

    def _verify(self, path, entry, destination=None, cancellable=False):
        digest, size = hashlib.sha256(), 0
        with self._open_archive(path, entry) as archive, archive.open('resource') as source:
            while chunk := source.read(CHUNK):
                if cancellable:
                    self._checkpoint()
                size += len(chunk)
                if size > entry['info']['size']:
                    raise ValueError('解压内容超过登记尺寸，未恢复原件')
                digest.update(chunk)
                if destination:
                    destination.write(chunk)
        if size != entry['info']['size'] or digest.hexdigest() != entry['info']['sha256'].lower():
            raise ValueError('资源完整性校验失败，原件及原副本保留')

    def _load_index(self, plan):
        root = _directory(plan['backup'], (ARCHIVES,), create=True)
        if root == self._index_root:
            return
        self._index, self._index_root = {}, root
        try:
            value = _read_json(root / 'index.json')
            if isinstance(value, dict) and value.get('format') == FORMAT and isinstance(value.get('entries'), dict):
                self._index = value['entries']
        except (OSError, ValueError, TypeError):
            pass

    def _remember(self, plan, identity, entry, archive, raw_removed):
        checked, status = self._archive(plan, entry)
        if checked != archive:
            raise ValueError('资源归档位置发生变化，未移除副本')
        self._index[identity] = {'sha256': entry['info']['sha256'].lower(), 'size': entry['info']['size'],
                                 'name': entry['info']['name'], 'relativePath': '/'.join(entry['parts']), 'mtimeNs': entry['mtimeNs'],
                                 'archiveSize': status.st_size, 'archiveMtimeNs': status.st_mtime_ns,
                                 'rawRemoved': raw_removed, 'verifiedAt': time.time()}
        _directory(plan['backup'], (ARCHIVES,))
        _atomic_json(self._index_root / 'index.json', {'format': FORMAT, 'entries': self._index})

    def _cached(self, identity, entry, status):
        cached = self._index.get(identity, {})
        return isinstance(cached, dict) and cached.get('sha256') == entry['info']['sha256'].lower() and cached.get('size') == entry['info']['size'] and (cached.get('archiveSize'), cached.get('archiveMtimeNs')) == (status.st_size, status.st_mtime_ns)

    def _compress(self, plan, entry, source, before):
        parent = _directory(plan['backup'], self._archive_parts(entry)[:-1], create=True)
        if shutil.disk_usage(parent).free < entry['info']['size'] + 32 * 1024 ** 2:
            raise ValueError('备用资源盘空间不足，本次原副本保留')
        fd, name = tempfile.mkstemp(prefix='.resource-', suffix='.tmp', dir=parent)
        temporary = Path(name)
        try:
            digest, count = hashlib.sha256(), 0
            with os.fdopen(fd, 'w+b') as output:
                with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=3, allowZip64=True) as archive:
                    archive.writestr('metadata.json', json.dumps({'format': FORMAT, 'name': entry['info']['name'], 'relativePath': '/'.join(entry['parts']), 'size': entry['info']['size'], 'sha256': entry['info']['sha256'].lower(), 'mtimeNs': entry['mtimeNs']}, ensure_ascii=False))
                    with source.open('rb') as stream, archive.open('resource', 'w', force_zip64=True) as destination:
                        if _signature(os.fstat(stream.fileno())) != _signature(before):
                            raise ValueError('压缩前原副本发生变化，未移除副本')
                        while chunk := stream.read(CHUNK):
                            self._checkpoint()
                            count += len(chunk)
                            if count > entry['info']['size']:
                                raise ValueError('原副本尺寸发生变化，未移除副本')
                            digest.update(chunk)
                            destination.write(chunk)
                            self._update(currentBytes=count)
                            if count % (CHUNK * 16) == 0:
                                time.sleep(.001)
                        if _signature(os.fstat(stream.fileno())) != _signature(before):
                            raise ValueError('压缩期间原副本发生变化，未移除副本')
                output.flush()
                os.fsync(output.fileno())
            if count != entry['info']['size'] or digest.hexdigest() != entry['info']['sha256'].lower():
                raise ValueError('原副本与登记内容不一致，未移除副本')
            self._verify(temporary, entry, cancellable=True)
            _directory(plan['backup'], self._archive_parts(entry)[:-1])
            target = parent / self._archive_parts(entry)[-1]
            try:
                _no_link_status(target)
            except FileNotFoundError:
                pass
            os.replace(temporary, target)
            return target
        finally:
            _directory(plan['backup'], self._archive_parts(entry)[:-1])
            temporary.unlink(missing_ok=True)

    def _one(self, plan, identity, entry, config):
        raw, raw_status = None, None
        try:
            raw, raw_status = _checked_file(plan['backup'], entry['parts'])
        except ValueError as error:
            if '缺失' not in str(error):
                raise
        with self._lock_for(entry['info']['sha256'].lower()):
            verified, raw_verified = False, False
            try:
                archive, archive_status = self._archive(plan, entry)
            except (FileNotFoundError, ValueError) as error:
                if isinstance(error, ValueError) and '不安全' in str(error):
                    raise
                archive, archive_status = None, None
            if archive is not None and not self._cached(identity, entry, archive_status):
                try:
                    self._verify(archive, entry, cancellable=True)
                    verified = True
                except (ValueError, zipfile.BadZipFile, RuntimeError, EOFError):
                    archive = None
            if archive is None:
                source, before = (raw, raw_status) if raw else _checked_file(plan['primary'], entry['parts'])
                archive = self._compress(plan, entry, source, before)
                verified = True
                raw_verified = source == raw
            removed = raw is None
            if raw and not config['keepRawCopies']:
                try:
                    primary, primary_status = _checked_file(plan['primary'], entry['parts'])
                    primary_ok = primary_status.st_size == entry['info']['size'] and primary_status.st_mtime_ns == entry['mtimeNs']
                except ValueError:
                    primary_ok = False
                if primary_ok:
                    if not verified:
                        self._verify(archive, entry, cancellable=True)
                    if not raw_verified:
                        digest, count = hashlib.sha256(), 0
                        with raw.open('rb') as stream:
                            while chunk := stream.read(CHUNK):
                                self._checkpoint()
                                count += len(chunk)
                                digest.update(chunk)
                        if count != entry['info']['size'] or digest.hexdigest() != entry['info']['sha256'].lower():
                            raise ValueError('备用原件已发生变化，未移除副本')
                    self._checkpoint()
                    if self.catalog.backup_signature() != plan['signature']:
                        raise Interrupted()
                    checked, current = _checked_file(plan['backup'], entry['parts'])
                    if checked != raw or _signature(current) != _signature(raw_status):
                        raise ValueError('备用原件刚发生变化，未移除副本')
                    if not raw.is_relative_to(plan['backup']) or raw.is_relative_to(plan['primary']):
                        raise ValueError('备用原件边界不安全，未移除副本')
                    # Only this verified duplicate is removed; primary files are never unlinked.
                    self._remember(plan, identity, entry, archive, False)
                    raw.unlink()
                    removed = True
            self._remember(plan, identity, entry, archive, removed)
            return archive.stat().st_size, not removed

    def run_pass(self):
        if not self._run_lock.acquire(blocking=False):
            raise ValueError('资源归档正在进行')
        try:
            config, plan = self.config(), self._plan()
            if not config['enabled']:
                return
            self._checkpoint()
            self._load_index(plan)
            self._update(persist=True, running=True, phase='compressing', completed=0, total=len(plan['entries']),
                         originalBytes=0, archiveBytes=0, rawRetained=0, errorCount=0, errors=[], lastError='', startedAt=time.time())
            completed, original_bytes, archive_bytes, retained, errors = 0, 0, 0, 0, []
            for identity, entry in plan['entries'].items():
                self._checkpoint()
                self._update(currentName=entry['info']['name'], currentBytes=0, currentSize=entry['info']['size'])
                try:
                    size, raw_retained = self._one(plan, identity, entry, config)
                    completed += 1
                    original_bytes += entry['info']['size']
                    archive_bytes += size
                    retained += raw_retained
                except Interrupted:
                    raise
                except Exception as error:
                    errors.append({'id': identity, 'name': entry['info']['name'], 'error': _message(error)})
                self._update(persist=True, completed=completed, originalBytes=original_bytes, archiveBytes=archive_bytes,
                             rawRetained=retained, errorCount=len(errors), errors=errors[-10:], lastError=errors[-1]['error'] if errors else '')
            self._last_scan, self._last_generation = time.monotonic(), plan['signature']
            self._update(persist=True, running=False, phase='partial' if errors or retained and not config['keepRawCopies'] else 'complete',
                         currentName='', currentBytes=0, finishedAt=time.time())
        except Interrupted:
            self._update(persist=True, running=False, phase='paused', currentName='', currentBytes=0)
        except Exception as error:
            self._last_scan = time.monotonic()
            try:
                self._last_generation = self.catalog.backup_signature()
            except (OSError, ValueError):
                self._last_generation = None
            self._update(persist=True, running=False, phase='error', lastError=_message(error))
            raise
        finally:
            self._run_lock.release()

    def has_backup(self, identity):
        plan = self._plan()
        entry = plan['entries'].get(identity)
        if not entry:
            return False
        try:
            archive, _ = self._archive(plan, entry)
        except (OSError, ValueError):
            return False
        with self._open_archive(archive, entry):
            return True

    def restore_missing(self, identity):
        if not self.config()['autoRestore']:
            return False
        try:
            plan = self._plan()
            entry = plan['entries'].get(identity)
            if not entry:
                return False
            with self._restore_lock, self._lock_for(entry['info']['sha256'].lower()):
                try:
                    existing, status = _checked_file(plan['primary'], entry['parts'])
                    if (status.st_size, status.st_mtime_ns) != (entry['info']['size'], entry['mtimeNs']):
                        raise ValueError('主资源已发生变化，未覆盖现有原件')
                    return True
                except ValueError as error:
                    if '缺失' not in str(error):
                        raise
                archive, _ = self._archive(plan, entry)
                parent = _directory(plan['primary'], entry['parts'][:-1], create=True)
                if shutil.disk_usage(parent).free < entry['info']['size'] + 32 * 1024 ** 2:
                    raise ValueError('主资源盘空间不足，压缩备份保留，未恢复原件')
                fd, name = tempfile.mkstemp(prefix='.restore-', suffix='.tmp', dir=parent)
                temporary = Path(name)
                try:
                    with os.fdopen(fd, 'wb') as output:
                        self._verify(archive, entry, destination=output)
                        output.flush()
                        os.fsync(output.fileno())
                    os.utime(temporary, ns=(entry['mtimeNs'], entry['mtimeNs']))
                    _directory(plan['primary'], entry['parts'][:-1])
                    target = parent / entry['parts'][-1]
                    try:
                        os.link(temporary, target)
                    except FileExistsError:
                        checked, status = _checked_file(plan['primary'], entry['parts'])
                        if (status.st_size, status.st_mtime_ns) != (entry['info']['size'], entry['mtimeNs']):
                            raise ValueError('主资源刚发生变化，未覆盖现有原件')
                    except OSError:
                        if os.name != 'nt':
                            raise
                        os.rename(temporary, target)
                    restored, status = _checked_file(plan['primary'], entry['parts'])
                    if status.st_size != entry['info']['size'] or status.st_mtime_ns != entry['mtimeNs']:
                        raise ValueError('恢复后的原件属性不一致，压缩备份保留')
                    self._update(persist=True, lastRestore={'id': identity, 'name': entry['info']['name'], 'at': time.time()}, restoreError='')
                    return True
                finally:
                    _directory(plan['primary'], entry['parts'][:-1])
                    temporary.unlink(missing_ok=True)
        except Exception as error:
            self._update(persist=True, restoreError=_message(error))
            raise ValueError(_message(error)) from None

    def run_now(self):
        if not self.config()['enabled']:
            raise ValueError('请先开启自动压缩')
        self._last_scan = 0
        self._wake.set()
        return self.status()

    def start(self):
        if self.worker and self.worker.is_alive():
            return
        self._stop.clear()
        self._cancel.clear()
        def run():
            while not self._stop.is_set():
                try:
                    config = self.config()
                    generation = self.catalog.backup_signature()
                    retry_after = 300 if self._status['phase'] in {'error', 'partial'} else 3600
                    if config['enabled'] and (not self._last_scan or time.monotonic() - self._last_scan > retry_after or generation != self._last_generation):
                        self.run_pass()
                except Exception:
                    pass
                self._wake.wait(30)
                self._wake.clear()
        self.worker = threading.Thread(target=run, name='yantu-resource-backup', daemon=True)
        self.worker.start()

    def close(self):
        self._stop.set()
        self._cancel.set()
        self._wake.set()
        if self.worker:
            self.worker.join()


@lru_cache(maxsize=32)
def _manager(data):
    return ResourceBackup(data)


def manager_for(data):
    with _MANAGER_LOCK:
        return _manager(os.path.abspath(data))
