"""Stage backup bytes, preview changes, and restore only after an explicit choice."""

import copy
import hashlib
import json
import os
import re
import secrets
import shutil
import tempfile
import threading
import time
import zipfile
from datetime import datetime
from pathlib import Path, PurePosixPath

from data_guard import (external_resource_summary, file_ids, linked_resource_info,
                        packed, validate_resource_reference)
from workspace_service import upsert

MAX_TOTAL = 20 * 1024 ** 3
MAX_FILE = 256 * 1024 ** 2
MAX_JSON = 12 * 1024 ** 2


def remap(value, mapping, key=''):
    if isinstance(value, list):
        return [remap(item, mapping, '$file' if key == 'files' else key) for item in value]
    if isinstance(value, dict):
        ledger = {'schemaVersion', 'digest', 'draft', 'receipts'} <= set(value)
        if ledger:
            from assistant_drafts import validate_receipt
            validate_receipt(value)
        result = {(mapping.get(name.split(':', 1)[0], name.split(':', 1)[0]) + ':' + name.split(':', 1)[1] if key == 'fileEdits' and ':' in name else name):
                  remap(item, mapping, name if name != 'id' or key != '$file' else 'fileId') for name, item in value.items()}
        if ledger:
            from assistant_drafts import digest
            result['digest'] = digest(result['draft'])
            validate_receipt(result)
        return result
    if isinstance(value, str) and key in {'fileId', 'sourceFileId', 'file_id'}:
        return mapping.get(value, value)
    return value


class RestorePlanner:
    def __init__(self, store, assistant, data, collections, valid_id, safe_name, max_unpacked=None):
        self.store, self.assistant, self.data = store, assistant, Path(data)
        self.collections, self.valid_id, self.safe_name = collections, valid_id, safe_name
        self.max_unpacked = MAX_TOTAL if max_unpacked is None else max_unpacked
        self.jobs, self.lock = {}, threading.RLock()

    def _drop(self, token):
        job = self.jobs.pop(token, None)
        if job:
            job['temporary'].cleanup()

    def close(self):
        with self.lock:
            for token in list(self.jobs):
                self._drop(token)

    def cancel(self, token, owner):
        with self.lock:
            job = self._job(token, owner)
            self._drop(token)
        return {'cancelled': True}

    def _job(self, token, owner):
        job = self.jobs.get(token)
        if not job or job['expires'] < time.time() or job['owner'] != owner:
            raise ValueError('恢复预览已失效，请重新选择备份')
        return job

    def prepare(self, upload, owner):
        with self.lock:
            for token, job in list(self.jobs.items()):
                if job['expires'] < time.time():
                    self._drop(token)
            if len(self.jobs) >= 3:
                raise ValueError('已有三个恢复预览，请先退回或完成后再上传')
            root = self.data / 'restore-staging'
            root.mkdir(exist_ok=True)
            temporary = tempfile.TemporaryDirectory(prefix='preview-', dir=root)
            try:
                job = self._prepare(upload, Path(temporary.name))
                job.update({'temporary': temporary, 'owner': owner, 'expires': time.time() + 1800})
                token = secrets.token_hex(24)
                self.jobs[token] = job
                return {**job['preview'], 'token': token}
            except Exception:
                temporary.cleanup()
                raise

    def _prepare(self, upload, folder):
        from library_sources import Catalog
        catalog = Catalog(self.data)
        with zipfile.ZipFile(upload) as archive:
            entries = archive.infolist()
            if len(entries) > 100000 or len({entry.filename for entry in entries}) != len(entries) or sum(entry.file_size for entry in entries) > self.max_unpacked:
                raise ValueError(f'备份有重复路径、过多条目或解压后超过 {self.max_unpacked // 1024 ** 2} MiB')
            def read_json(name, default=None):
                if name not in archive.namelist() and default is not None:
                    return default
                if archive.getinfo(name).file_size > MAX_JSON:
                    raise ValueError('备份中的数据清单超过 12 MB')
                value = json.loads(archive.read(name).decode('utf-8'))
                json.dumps(value, allow_nan=False)
                return value
            document = read_json('数据.json')
            incoming = document.get('state')
            manifest = read_json('附件清单.json')
            assistant = read_json('助手记忆与对话.json', {})
            protection = read_json('保护记录.json', {})
            if not isinstance(incoming, dict) or not isinstance(manifest, dict) or len(manifest) > 50000 or not isinstance(assistant, dict) or not isinstance(protection, dict):
                raise ValueError('备份数据格式无效')
            mapping, files, same, references = {}, [], 0, []
            for identity, info in manifest.items():
                if not self.valid_id(identity) or not isinstance(info, dict):
                    raise ValueError('备份包含无效附件编号')
                if info.get('storage') is not None or info.get('library') is True or info.get('included') is False:
                    descriptor = validate_resource_reference(identity, info)
                    if 'files/' + identity in archive.namelist():
                        raise ValueError('外部资源声明未包含原件，但 ZIP 中存在同编号原件')
                    available = self._resource_available(descriptor, catalog)
                    self._check_resource_identity(descriptor)
                    references.append({**descriptor, 'available': available,
                                       'status': 'connected' if available else 'missing-reconnect'})
                    files.append({**descriptor, 'oldId': identity, 'stamp': time.time()})
                    continue
                name = str(info.get('path', ''))
                path = PurePosixPath(name)
                if not name or path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
                    raise ValueError('备份附件路径不安全')
                entry = archive.getinfo(name)
                if entry.file_size > MAX_FILE:
                    raise ValueError('备份中存在超过 256 MB 的单个附件')
                digest, length = hashlib.sha256(), 0
                with archive.open(entry) as source, (folder / identity).open('wb') as target:
                    while chunk := source.read(65536):
                        length += len(chunk)
                        if length > MAX_FILE:
                            raise ValueError('备份附件实际大小超限')
                        digest.update(chunk)
                        target.write(chunk)
                fingerprint = digest.hexdigest()
                if length != entry.file_size or info.get('sha256') and info['sha256'] != fingerprint:
                    raise ValueError('备份附件大小或完整性校验失败')
                existing = self.data / 'files' / identity
                if catalog.info(identity) or linked_resource_info(self.store, identity):
                    mapping[identity] = secrets.token_hex(16)
                elif existing.is_file():
                    with existing.open('rb') as stream:
                        equal = hashlib.file_digest(stream, 'sha256').hexdigest() == fingerprint
                    if equal:
                        same += 1
                    else:
                        mapping[identity] = secrets.token_hex(16)
                stamp = datetime.fromisoformat(info['uploadedAt'].replace('Z', '+00:00')).timestamp() if info.get('uploadedAt') else time.time()
                files.append({'oldId': identity, 'id': mapping.get(identity, identity), 'name': self.safe_name(info.get('name', identity)), 'type': str(info.get('type', 'application/octet-stream')),
                              'size': length, 'stamp': stamp, 'sha256': fingerprint})
        incoming, assistant = remap(incoming, mapping), remap(assistant, mapping)
        for row in protection.get('recovery', []):
            if not isinstance(row, list) or len(row) != 6 or row[1] not in self.collections | {'$settings'} or row[3] not in {'trash', 'history', 'conflict'}:
                raise ValueError('备份恢复记录无效')
            row[5] = json.dumps(remap(json.loads(row[5]), mapping), ensure_ascii=False)
        for row in protection.get('conflicts', []):
            if not isinstance(row, list) or len(row) != 3:
                raise ValueError('备份冲突记录无效')
            row[2] = json.dumps(remap(json.loads(row[2]), mapping), ensure_ascii=False)
        snapshot = self.store.snapshot()
        current = snapshot['state']
        changes, operations = [], []
        skipped_settings = []
        for collection, records in incoming.items():
            if collection not in self.collections:
                if collection in {'deviceId', 'planningFocusLease'} or re.search(r'key|token|secret|password|pcurl', collection, re.I):
                    skipped_settings.append(collection)
                elif collection == 'focusTimer' and isinstance(records, dict):
                    records.update(running=False, endAt=None, segmentStartedAt=None, segmentId=None)
                continue
            if not isinstance(records, list) or len(records) > 100000:
                raise ValueError('备份记录列表无效')
            known = {record['id']: record for record in current.get(collection, [])}
            seen = set()
            for record in records:
                if not isinstance(record, dict) or not self.valid_id(record.get('id')) or record['id'] in seen:
                    raise ValueError('备份记录编号无效或重复')
                seen.add(record['id'])
                before = known.get(record['id'])
                kind = 'added' if before is None else 'unchanged' if before == record else 'changed'
                changes.append({'collection': collection, 'id': record['id'], 'title': record.get('title') or record.get('name') or record['id'], 'kind': kind})
                operation = upsert(collection, record)
                operation['unset'] = [key for key in before or {} if key not in record]
                operations.append(operation)
        with self.store.lock:
            available = {item['id'] for item in files} | {row[0] for row in self.store.db.execute('SELECT id FROM files')} | catalog.ids()
        missing = file_ids([incoming, assistant, [json.loads(row[5]) for row in protection.get('recovery', [])], [json.loads(row[2]) for row in protection.get('conflicts', [])]]) - available
        if missing:
            raise ValueError('备份缺少 ' + str(len(missing)) + ' 个被引用的原始附件，未导入任何数据')
        with self.store.lock:
            try:
                for start in range(0, len(operations), 2500):
                    self.store.apply(operations[start:start + 2500], commit=False)
                self.assistant.import_data(assistant, commit=False)
            finally:
                self.store.db.rollback()
        return {'incoming': incoming, 'files': files, 'assistant': assistant, 'protection': protection,
                'preview': {'revision': snapshot['revision'], 'counts': {kind: sum(change['kind'] == kind for change in changes) for kind in ('added', 'changed', 'unchanged')},
                            'changes': changes[:500], 'total': len(changes), 'files': len(files), 'sameFiles': same, 'versionedFiles': len(mapping),
                            **external_resource_summary(references),
                            'assistant': {key: len(assistant.get(key, [])) for key in ('memory', 'conversations', 'messages')}, 'settings': [key for key in incoming if key not in self.collections and key not in skipped_settings], 'skippedSettings': skipped_settings}}

    def _resource_available(self, info, catalog):
        registered = catalog.info(info['id'])
        if not registered:
            return False
        if (registered['size'], registered['sha256'].lower()) != (info['size'], info['sha256']):
            raise ValueError('外部资源引用与本机已登记版本不一致：' + info['id'])
        try:
            catalog.resolve(info['id'], allow_restore=False)
        except ValueError:
            return False
        return True

    def _check_resource_identity(self, info):
        identity = info['id']
        retained = linked_resource_info(self.store, identity)
        if retained and (retained['size'], retained['sha256']) != (info['size'], info['sha256']):
            raise ValueError('外部资源编号与已有引用冲突：' + identity)
        with self.store.lock:
            row = self.store.db.execute('SELECT size FROM files WHERE id=?', (identity,)).fetchone()
            fingerprint = self.store.db.execute('SELECT sha256 FROM file_fingerprints WHERE id=?', (identity,)).fetchone()
        if (row and row[0] != info['size']) or (fingerprint and fingerprint[0].lower() != info['sha256']):
            raise ValueError('外部资源编号与已有附件冲突：' + identity)
        existing = self.data / 'files' / identity
        if existing.exists():
            if existing.is_symlink() or not existing.is_file() or existing.stat().st_size != info['size']:
                raise ValueError('外部资源编号与已有附件冲突：' + identity)
            with existing.open('rb') as stream:
                if hashlib.file_digest(stream, 'sha256').hexdigest() != info['sha256']:
                    raise ValueError('外部资源编号与已有附件冲突：' + identity)

    def commit(self, token, owner, options):
        if options.get('strategy') not in {'keep-current', 'use-backup'} or options.get('confirmation') != 'RESTORE_BACKUP':
            raise ValueError('请选择保留当前或使用备份，并确认恢复')
        with self.lock:
            job = self._job(token, owner)
            if hasattr(self.store, 'obsidian'):
                self.store.obsidian.pause_for_restore()
            with self.store.changed:
                snapshot = self.store.snapshot()
                if snapshot['revision'] != options.get('revision') or snapshot['revision'] != job['preview']['revision']:
                    raise ValueError('预览后资料又有修改，请退回并重新上传预览')
                # This full protection point must succeed before any learning record changes.
                self.store.guard.backup(force=True)
                operations = []
                incoming, strategy = job['incoming'], options['strategy']
                for collection, records in incoming.items():
                    if collection in self.collections:
                        known = {item['id']: item for item in snapshot['state'].get(collection, [])}
                        for record in records:
                            if strategy == 'use-backup' or record['id'] not in known:
                                operation = upsert(collection, record)
                                operation['unset'] = [key for key in known.get(record['id'], {}) if key not in record]
                                operations.append(operation)
                    elif options.get('settings') and collection in job['preview']['settings']:
                        operations.append({'opId': secrets.token_hex(16), 'kind': 'setting', 'key': collection, 'value': records})
                from library_sources import Catalog
                catalog = Catalog(self.data)
                placed, references = [], []
                try:
                    for start in range(0, len(operations), 2500):
                        extra = {'trusted_obsidian': True} if hasattr(self.store, 'obsidian') else {}
                        self.store.apply(operations[start:start + 2500], commit=False, **extra)
                    if options.get('assistant'):
                        assistant = copy.deepcopy(job['assistant'])
                        if strategy == 'keep-current':
                            current_memory = {item['id'] for item in self.assistant.export_data().get('memory', [])}
                            assistant['memory'] = [item for item in assistant.get('memory', []) if item['id'] not in current_memory]
                        self.assistant.import_data(assistant, commit=False)
                    if options.get('protection'):
                        for row in job['protection'].get('recovery', []):
                            self.store.db.execute('INSERT OR IGNORE INTO recovery_records VALUES(?,?,?,?,?,?)', row)
                        for row in job['protection'].get('conflicts', []):
                            self.store.db.execute('INSERT OR IGNORE INTO sync_conflicts VALUES(?,?,?)', row)
                    for info in job['files']:
                        identity = info['id']
                        if info.get('storage') == 'resource-library':
                            descriptor = {key: value for key, value in info.items() if key not in {'oldId', 'stamp'}}
                            descriptor = validate_resource_reference(identity, descriptor)
                            available = self._resource_available(descriptor, catalog)
                            self._check_resource_identity(descriptor)
                            references.append({**descriptor, 'available': available,
                                               'status': 'connected' if available else 'missing-reconnect'})
                            self.store.db.execute('INSERT OR REPLACE INTO resource_library_references VALUES(?,?)', (identity, packed(descriptor)))
                            self.store.db.execute('INSERT OR IGNORE INTO files VALUES(?,?,?,?)', (identity, info['name'], info['type'], info['size']))
                            self.store.db.execute('INSERT OR IGNORE INTO file_dates VALUES(?,?)', (identity, info['stamp']))
                            self.store.db.execute('INSERT OR REPLACE INTO file_fingerprints VALUES(?,?)', (identity, info['sha256']))
                            continue
                        target = self.data / 'files' / identity
                        if target.exists():
                            with target.open('rb') as stream:
                                if hashlib.file_digest(stream, 'sha256').hexdigest() != info['sha256']:
                                    raise ValueError('附件在预览后发生变化，请重新上传预览')
                        if not target.exists():
                            os.replace(Path(job['temporary'].name) / info['oldId'], target)
                            placed.append((target, Path(job['temporary'].name) / info['oldId']))
                        self.store.db.execute('INSERT OR IGNORE INTO files VALUES(?,?,?,?)', (identity, info['name'], info['type'], info['size']))
                        self.store.db.execute('INSERT OR IGNORE INTO file_dates VALUES(?,?)', (identity, info['stamp']))
                        self.store.db.execute('INSERT OR REPLACE INTO file_fingerprints VALUES(?,?)', (identity, info['sha256']))
                    self.store.db.commit()
                    self.store.changed.notify_all()
                except Exception:
                    self.store.db.rollback()
                    for target, staged in reversed(placed):
                        os.replace(target, staged)
                    raise
                result = {**self.store.snapshot(), **external_resource_summary(references)}
            self._drop(token)
        return result
