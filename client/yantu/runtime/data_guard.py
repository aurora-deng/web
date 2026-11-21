"""Recoverable study edits, explicit sync conflicts and bounded local backups."""

import copy
import hashlib
import json
import os
import re
import secrets
import shutil
import threading
import tempfile
import time
import zipfile
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path, PureWindowsPath


RESOURCE_NOTICE = "外部资源库原件不在学习 ZIP 中；请独立保存资源库副本，换电脑后需重新连接资源库。"


def validate_resource_reference(identity, info):
    """Accept public descriptors only, never paths supplied by an archive."""
    allowed = {'id', 'name', 'type', 'size', 'sha256', 'pageCount',
               'library', 'backupMode', 'storage', 'included'}
    if (not isinstance(identity, str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,100}', identity)
            or identity.endswith('.') or PureWindowsPath(identity).is_reserved()
            or not isinstance(info, dict) or set(info) - allowed
            or info.get('id') != identity or info.get('storage') != 'resource-library'
            or info.get('included') is not False):
        raise ValueError('外部资源引用格式或编号无效，不允许导入磁盘路径')
    name, mime = info.get('name'), info.get('type')
    if (not isinstance(name, str) or not name or name in {'.', '..'}
            or re.search(r'[\\/:*?"<>|\x00-\x1f\x7f]', name)
            or name.endswith(('.', ' ')) or PureWindowsPath(name).is_reserved()
            or not isinstance(mime, str) or not mime
            or re.search(r'[\x00-\x1f\x7f]', mime)
            or ':' in mime or '\\' in mime or mime.startswith('/')
            or type(info.get('size')) is not int or info['size'] < 0
            or not isinstance(info.get('sha256'), str)
            or not re.fullmatch(r'[0-9a-fA-F]{64}', info['sha256'])
            or 'pageCount' in info and (type(info['pageCount']) is not int or info['pageCount'] < 1)
            or 'library' in info and info['library'] is not True
            or 'backupMode' in info and info['backupMode'] != 'separate-resource-copy'):
        raise ValueError('外部资源引用的名称、大小或 SHA-256 无效')
    return {**info, 'sha256': info['sha256'].lower(), 'library': True,
            'backupMode': 'separate-resource-copy'}


def linked_resource_info(store, identity):
    """Public persisted metadata for a restored reference awaiting reconnection."""
    with store.lock:
        row = store.db.execute('SELECT body FROM resource_library_references WHERE id=?', (identity,)).fetchone()
    return validate_resource_reference(identity, json.loads(row[0])) if row else None


def external_resource_summary(references):
    references = list(references)
    return {'externalFiles': len(references),
            'missingExternalFiles': sum(item.get('available') is False for item in references),
            'externalReferences': references,
            'externalResourceNotice': RESOURCE_NOTICE if references else ''}


def packed(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)


def file_ids(value):
    result = set()
    if isinstance(value, dict):
        if isinstance(value.get("fileId"), str):
            result.add(value["fileId"])
        if isinstance(value.get('sourceFileId'), str):
            result.add(value['sourceFileId'])
        for item in value.get("files", []) if isinstance(value.get("files"), list) else []:
            if isinstance(item, dict) and isinstance(item.get("id"), str):
                result.add(item["id"])
        for child in value.values():
            result.update(file_ids(child))
    elif isinstance(value, list):
        for child in value:
            result.update(file_ids(child))
    return result


class DataGuard:
    def __init__(self, store, data):
        self.store, self.data = store, Path(data)
        self.stop = threading.Event()
        self.worker = None
        self.backup_lock = threading.Lock()
        self.export_assistant = lambda: {}
        with store.lock:
            store.db.executescript("""
                CREATE TABLE IF NOT EXISTS recovery_records (
                    id TEXT PRIMARY KEY, collection TEXT NOT NULL, record_id TEXT NOT NULL,
                    kind TEXT NOT NULL, created REAL NOT NULL, body TEXT NOT NULL);
                CREATE INDEX IF NOT EXISTS recovery_lookup ON recovery_records(collection,record_id,kind,created);
                CREATE TABLE IF NOT EXISTS sync_conflicts (
                    id TEXT PRIMARY KEY, created REAL NOT NULL, body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS protection_meta (key TEXT PRIMARY KEY, body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS resource_library_references (id TEXT PRIMARY KEY, body TEXT NOT NULL);
            """)
            store.db.commit()
        row = store.db.execute("SELECT body FROM protection_meta WHERE key='owner'").fetchone()
        self.owner = json.loads(row[0]) if row else secrets.token_hex(16)
        if not row:
            self.meta("owner", self.owner)
        from backup_destinations import BackupDestination
        self.destination = BackupDestination(self.data, self.owner)

    def config(self):
        with self.store.lock:
            row = self.store.db.execute("SELECT body FROM protection_meta WHERE key='config'").fetchone()
        return {"enabled": True, "keep": 7, "maxBytes": 2 * 1024 ** 3,
                "secondary": {"enabled": False, "path": "", "allowSameDisk": False}, **(json.loads(row[0]) if row else {})}

    def configure(self, body):
        keep = max(1, min(30, int(body.get("keep", 7))))
        max_bytes = max(32 * 1024 ** 2, min(20 * 1024 ** 3, int(body.get("maxBytes", 2 * 1024 ** 3))))
        secondary = self.config()["secondary"]
        if "secondary" in body:
            value = body["secondary"]
            if not isinstance(value, dict):
                raise ValueError("第二副本设置无效")
            secondary = {"enabled": bool(value.get("enabled")), "path": str(value.get("path", "")).strip(),
                         "allowSameDisk": value.get("allowSameDisk") is True}
            old = self.config()["secondary"]
            secondary["generation"] = old.get("generation") if all(old.get(key) == item for key, item in secondary.items()) else secrets.token_hex(12)
            if secondary["enabled"]:
                folder, info = self.destination.validate(secondary["path"], secondary["allowSameDisk"], initialize=True)
                secondary.update({"path": str(folder), **info})
        config = {"enabled": bool(body.get("enabled", True)), "keep": keep, "maxBytes": max_bytes, "secondary": secondary}
        self.meta("config", config)
        return self.status()

    def meta(self, key, value):
        with self.store.lock:
            self.store.db.execute("INSERT OR REPLACE INTO protection_meta VALUES (?,?)", (key, packed(value)))
            self.store.db.commit()

    def remember(self, col, record_id, old, new, reason="edit"):
        if old is None or old == new:
            return
        created = time.time()
        if new is None:
            self.store.db.execute("INSERT INTO recovery_records VALUES (?,?,?,?,?,?)",
                                  (secrets.token_hex(16), col, record_id, "trash", created, packed(old)))
        else:
            self.store.db.execute("INSERT INTO recovery_records VALUES (?,?,?,?,?,?)",
                                  (secrets.token_hex(16), col, record_id, "history", created, packed(old)))
            self.store.db.execute("""DELETE FROM recovery_records WHERE collection=? AND record_id=? AND kind='history'
                AND id NOT IN (SELECT id FROM recovery_records WHERE collection=? AND record_id=? AND kind='history'
                ORDER BY created DESC LIMIT 20)""", (col, record_id, col, record_id))

    def conflicts(self):
        with self.store.lock:
            return [{"id": row[0], "created": row[1], **json.loads(row[2])}
                    for row in self.store.db.execute("SELECT id,created,body FROM sync_conflicts ORDER BY created")]

    def protect_operation(self, op, state):
        """Return the safe subset; persist competing values instead of overwriting."""
        if "base" not in op:
            return op
        safe, conflicts = copy.deepcopy(op), {}
        kind = op.get("kind")
        if kind == "setting":
            key = op.get("key")
            current = {"exists": key in state, "value": state.get(key)}
            desired = {"exists": not op.get("remove"), "value": op.get("value")}
            if current != op["base"] and current != desired:
                conflicts[key] = {"base": op["base"], "current": current, "incoming": desired}
                safe = None
        else:
            current = next((r for r in state.get(op.get("collection"), []) if r.get("id") == op.get("id")), None)
            if kind == "delete":
                if current is not None and current != op["base"]:
                    conflicts["$record"] = {"base": op["base"], "current": current, "incoming": None}
                    safe = None
            elif current is None and op.get("baseExists"):
                conflicts["$record"] = {"base": op.get("baseRecord"), "current": None,
                                         "incoming": {**(op.get("baseRecord") or {}), **op.get("fields", {}), "id": op["id"]}}
                for key in op.get("unset", []):
                    conflicts["$record"]["incoming"].pop(key, None)
                safe = None
            else:
                current = current or {}
                for key in set(op.get("fields", {})) | set(op.get("unset", [])):
                    existing = {"exists": key in current, "value": current.get(key)}
                    desired = {"exists": key not in op.get("unset", []), "value": op.get("fields", {}).get(key)}
                    if existing != op["base"].get(key, {"exists": False, "value": None}) and existing != desired:
                        conflicts[key] = {"base": op["base"].get(key), "current": existing, "incoming": desired}
                        safe["fields"].pop(key, None)
                        safe["unset"] = [item for item in safe.get("unset", []) if item != key]
                if not safe.get("fields") and not safe.get("unset") and op.get("baseExists"):
                    safe = None
        if conflicts:
            body = {"operation": op, "fields": conflicts, "title": op.get("baseRecord", {}).get("title") or op.get("baseRecord", {}).get("name") or op.get("id") or op.get("key")}
            self.store.db.execute("INSERT OR IGNORE INTO sync_conflicts VALUES (?,?,?)", (op["opId"], time.time(), packed(body)))
        return safe

    def resolve(self, identity, choice):
        if choice not in {"incoming", "current"}:
            raise ValueError("请选择保留电脑当前版本或待同步版本")
        with self.store.changed:
            row = self.store.db.execute("SELECT body FROM sync_conflicts WHERE id=?", (identity,)).fetchone()
            if not row:
                raise ValueError("冲突已处理，请刷新")
            conflict = json.loads(row[0])
            op = conflict["operation"]
            current_state = self.store.snapshot()["state"]
            current_record = next((r for r in current_state.get(op.get("collection"), []) if r.get("id") == op.get("id")), None)
            for key, values in conflict["fields"].items():
                current = ({"exists": key in current_state, "value": current_state.get(key)} if op["kind"] == "setting" else
                           current_record if key == "$record" else {"exists": key in (current_record or {}), "value": (current_record or {}).get(key)})
                if current != values["current"]:
                    values["current"] = current
                    self.store.db.execute("UPDATE sync_conflicts SET body=? WHERE id=?", (packed(conflict), identity))
                    self.store.db.commit()
                    raise ValueError("当前内容又有更新，已保留全部版本，请重新比较后选择")
            if choice == "incoming":
                action = {"opId": secrets.token_hex(16), "kind": op["kind"]}
                if op["kind"] == "setting":
                    action.update({"key": op["key"], "value": op.get("value"), "remove": op.get("remove", False)})
                else:
                    action.update({"collection": op["collection"], "id": op["id"]})
                    if "$record" in conflict["fields"]:
                        incoming = conflict["fields"]["$record"]["incoming"]
                        action.update({"kind": "delete" if incoming is None else "upsert", "fields": incoming or {}})
                    else:
                        action.update({"fields": {k: v["incoming"]["value"] for k, v in conflict["fields"].items() if v["incoming"]["exists"]},
                                       "unset": [k for k, v in conflict["fields"].items() if not v["incoming"]["exists"]]})
                self.store.apply([action], commit=False)
            # Retain both choices in history, even when the owner keeps the current one.
            self.store.db.execute("INSERT INTO recovery_records VALUES (?,?,?,?,?,?)", (secrets.token_hex(16), op.get("collection", "$settings"), op.get("id", op.get("key", "")), "conflict", time.time(), packed(conflict)))
            self.store.db.execute("DELETE FROM sync_conflicts WHERE id=?", (identity,))
            self.store.db.execute("UPDATE document SET revision=revision+1 WHERE id=1")
            self.store.db.commit()
            self.store.changed.notify_all()
            return self.store.snapshot()

    def records(self, kind="trash", col=None, record_id=None):
        if kind not in {"trash", "history", "conflict"}:
            raise ValueError("无效的历史记录类型")
        sql, args = "SELECT id,collection,record_id,kind,created,body FROM recovery_records WHERE kind=?", [kind]
        for key, value in (("collection", col), ("record_id", record_id)):
            if value:
                sql += " AND " + key + "=?"
                args.append(value)
        with self.store.lock:
            return [dict(zip(("id", "collection", "recordId", "kind", "created", "record"), (*row[:5], json.loads(row[5]))))
                    for row in self.store.db.execute(sql + " ORDER BY created DESC LIMIT 500", args)]

    def record_page(self, kind="trash", col=None, record_id=None, query="", offset=0, limit=50, after=None, before=None):
        if kind not in {"trash", "history", "conflict", "conflicts"}:
            raise ValueError("无效的历史记录类型")
        limit, offset = max(1, min(100, int(limit))), max(0, int(offset))
        if len(str(query)) > 300 or offset > 10000000:
            raise ValueError("筛选范围过大")
        live = kind == "conflicts"
        table = "sync_conflicts" if live else "recovery_records"
        clauses, args = ([] if live else ["kind=?"]), ([] if live else [kind])
        for key, value in (("collection", col), ("record_id", record_id)):
            if value:
                clauses.append(("json_extract(body,'$.operation." + ("collection" if key == "collection" else "id") + "')=?") if live else key + "=?")
                args.append(value)
        if query:
            clauses.append("instr(lower(body),lower(?))>0")
            args.append(str(query))
        for operator, value in ((">=", after), ("<=", before)):
            if value is not None:
                value = float(value)
                if not 0 <= value < 100000000000:
                    raise ValueError("无效的日期范围")
                clauses.append("created" + operator + "?")
                args.append(value)
        where = " WHERE " + " AND ".join(clauses) if clauses else ""
        columns = "id,created,body" if live else "id,collection,record_id,kind,created,body"
        with self.store.lock:
            total = self.store.db.execute("SELECT COUNT(*) FROM " + table + where, args).fetchone()[0]
            rows = self.store.db.execute("SELECT " + columns + " FROM " + table + where + " ORDER BY created DESC,id DESC LIMIT ? OFFSET ?", [*args, limit, offset]).fetchall()
        records = ([{"id": row[0], "created": row[1], **json.loads(row[2])} for row in rows] if live else
                   [dict(zip(("id", "collection", "recordId", "kind", "created", "record"), (*row[:5], json.loads(row[5])))) for row in rows])
        return {"records": records, "total": total, "offset": offset, "limit": limit,
                "hasMore": offset + len(records) < total}

    def restore(self, identity, expected):
        with self.store.changed:
            row = self.store.db.execute("SELECT collection,record_id,kind,body FROM recovery_records WHERE id=?", (identity,)).fetchone()
            if not row or row[2] not in {"trash", "history"}:
                raise ValueError("恢复记录不存在")
            col, record_id, kind, body = row
            current = next((r for r in self.store.snapshot()["state"].get(col, []) if r.get("id") == record_id), None)
            if current != expected:
                raise ValueError("记录已变化，请重新打开比较后恢复")
            record = json.loads(body)
            self.store.apply([{"opId": secrets.token_hex(16), "kind": "upsert", "collection": col, "id": record_id,
                               "fields": record, "unset": [key for key in (current or {}) if key not in record]}], commit=False)
            if kind == "trash":
                self.store.db.execute("DELETE FROM recovery_records WHERE id=?", (identity,))
            self.store.db.commit()
            self.store.changed.notify_all()
            return self.store.snapshot()

    def purge(self, identity):
        with self.store.lock:
            row = self.store.db.execute("SELECT collection,record_id FROM recovery_records WHERE id=? AND kind='trash'", (identity,)).fetchone()
            if not row:
                raise ValueError("回收站记录不存在")
            candidates = set()
            for (body,) in self.store.db.execute("SELECT body FROM recovery_records WHERE collection=? AND record_id=?", row):
                candidates.update(file_ids(json.loads(body)))
            # Explicit per-record purge also discards its historical recovery copies.
            self.store.db.execute("DELETE FROM recovery_records WHERE collection=? AND record_id=?", row)
            self.store.db.commit()
            active = file_ids(self.store.snapshot()["state"])
            for candidate in candidates - active:
                if not self.store.file_info(candidate) or self.references(candidate):
                    continue
                (self.data / "files" / candidate).unlink(missing_ok=True)
                self.store.db.execute("DELETE FROM files WHERE id=?", (candidate,))
                self.store.db.execute("DELETE FROM file_dates WHERE id=?", (candidate,))
                self.store.db.execute('DELETE FROM file_fingerprints WHERE id=?', (candidate,))
                if self.store.db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='attachment_text'").fetchone():
                    self.store.db.execute("DELETE FROM attachment_text WHERE id=?", (candidate,))
            self.store.db.commit()

    def references(self, identity):
        for (body,) in self.store.db.execute("SELECT body FROM recovery_records UNION ALL SELECT body FROM sync_conflicts"):
            if identity in file_ids(json.loads(body)):
                return True
        return False

    def status(self, local=True):
        with self.store.lock:
            counts = dict(self.store.db.execute("SELECT kind,COUNT(*) FROM recovery_records GROUP BY kind"))
            row = self.store.db.execute("SELECT body FROM protection_meta WHERE key='backup'").fetchone()
            secondary = self.store.db.execute("SELECT body FROM protection_meta WHERE key='secondary'").fetchone()
        backups = self.backups()
        config = self.config()
        if not local:
            config["secondary"] = {key: value for key, value in config["secondary"].items() if key != "path"}
        backup_status = json.loads(row[0]) if row else {}
        secondary_status = json.loads(secondary[0]) if secondary else {}
        if not local:
            if backup_status.get("error"):
                backup_status["error"] = "电脑备份未成功，请到电脑端查看详情"
            if secondary_status.get("error"):
                secondary_status["error"] = "第二副本未成功，请到电脑端查看详情"
        return {"config": config, "counts": counts, "conflicts": self.conflicts(), "backups": backups,
                "backupBytes": sum(item["size"] for item in backups), "backup": backup_status,
                "secondary": secondary_status,
                "scheduler": bool(self.worker and self.worker.is_alive())}

    def backups(self):
        folder = self.data / "backups"
        return [{"id": file.name, "size": file.stat().st_size, "created": file.stat().st_mtime}
                for file in sorted(folder.glob("yantu-*.zip"), key=lambda path: (path.stat().st_mtime_ns, path.name), reverse=True)] if folder.exists() else []

    @contextmanager
    def staged_snapshot(self, max_bytes=None):
        from library_sources import Catalog
        catalog = Catalog(self.data)
        folder = self.data / "backups"
        folder.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="snapshot-", dir=folder) as staging:
            staging = Path(staging)
            with self.store.lock:
                snapshot = self.store.snapshot()
                recovery = [list(row) for row in self.store.db.execute("SELECT * FROM recovery_records")]
                conflicts = [list(row) for row in self.store.db.execute("SELECT * FROM sync_conflicts")]
                assistant = self.export_assistant()
                ids = file_ids([snapshot["state"], assistant, [json.loads(row[-1]) for row in recovery], [json.loads(row[-1]) for row in conflicts]])
                infos, references = [], []
                for identity in ids:
                    registered = catalog.descriptor(identity)
                    retained = linked_resource_info(self.store, identity)
                    if registered:
                        info = validate_resource_reference(identity, registered)
                        if retained and (info['sha256'], info['size']) != (retained['sha256'], retained['size']):
                            raise ValueError('已登记资源与保留引用不一致：' + identity)
                        catalog.resolve(identity, allow_restore=False)
                    else:
                        info = retained or self.store.file_info(identity)
                        if info and info.get('library') and not retained:
                            raise ValueError('资源未登记，备份未完成：' + identity)
                    if not info:
                        raise ValueError("存在缺失附件的记录，请先恢复原附件：" + str(identity))
                    infos.append(info)
                    if info.get('storage') == 'resource-library':
                        references.append({**info, 'available': bool(registered),
                                           'status': 'connected' if registered else 'missing-reconnect'})
                ordinary = [info for info in infos if info.get('storage') != 'resource-library']
                if max_bytes is not None and sum(info["size"] for info in ordinary) > max_bytes:
                    raise ValueError("完整附件超过备份容量上限，请提高上限或另存完整备份；未删除已有备份")
                for info in ordinary:
                    source = self.data / "files" / info["id"]
                    if not source.is_file():
                        raise ValueError("附件缺失，备份未完成：" + info["name"])
                    try:
                        os.link(source, staging / info["id"])
                    except OSError:
                        shutil.copy2(source, staging / info["id"])
            # Immutable links keep this consistent while compression runs without the DB lock.
            yield {"snapshot": snapshot, "recovery": recovery, "conflicts": conflicts, "assistant": assistant,
                   "infos": infos, "externalReferences": references}, staging

    def backup(self, force=False):
        config = self.config()
        if not force and not config["enabled"]:
            return None
        if not self.backup_lock.acquire(blocking=False):
            raise ValueError("备份正在进行，请稍后查看")
        folder = self.data / "backups"
        folder.mkdir(exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        target = folder / ("yantu-" + stamp + "-" + secrets.token_hex(3) + ".zip")
        temporary = target.with_suffix(".tmp")
        try:
            with self.staged_snapshot(config["maxBytes"]) as (frozen, staging):
                manifest = {}
                with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as archive:
                    archive.writestr("数据.json", packed({"version": 3, **frozen["snapshot"]}))
                    archive.writestr("助手记忆与对话.json", packed(frozen["assistant"]))
                    archive.writestr("保护记录.json", packed({"recovery": frozen["recovery"], "conflicts": frozen["conflicts"]}))
                    for info in frozen["infos"]:
                        if info.get('storage') == 'resource-library':
                            manifest[info['id']] = validate_resource_reference(info['id'], info)
                            continue
                        source = staging / info["id"]
                        entry = "files/" + info["id"]
                        archive.write(source, entry)
                        with source.open("rb") as stream:
                            digest = hashlib.file_digest(stream, "sha256").hexdigest()
                        manifest[info["id"]] = {**info, "path": entry, "sha256": digest}
                    archive.writestr("附件清单.json", packed(manifest))
            if temporary.stat().st_size > config["maxBytes"]:
                raise ValueError("备份超过容量上限；未删除已有备份")
            with zipfile.ZipFile(temporary) as archive:
                if archive.testzip():
                    raise ValueError("备份完整性检查失败")
            os.replace(temporary, target)
            backups = self.backups()
            total = sum(item["size"] for item in backups)
            for index, item in reversed(list(enumerate(backups))):
                if index and (index >= config["keep"] or total > config["maxBytes"]):
                    (folder / item["id"]).unlink()
                    total -= item["size"]
            result = {"lastSuccess": time.time(), "day": datetime.now().date().isoformat(), "id": target.name, "error": None}
            result.update(external_resource_summary(frozen['externalReferences']))
            self.meta("backup", result)
            if config["secondary"]["enabled"]:
                result["secondary"] = self.replicate(target, config)
            return result
        except Exception as error:
            with self.store.lock:
                row = self.store.db.execute("SELECT body FROM protection_meta WHERE key='backup'").fetchone()
            previous = json.loads(row[0]) if row else {}
            self.meta("backup", {**previous, "lastAttempt": time.time(), "error": str(error)})
            raise
        finally:
            temporary.unlink(missing_ok=True)
            self.backup_lock.release()

    def replicate(self, source, config):
        try:
            result = self.destination.copy(source, config["secondary"], config["keep"], config["maxBytes"])
            result["generation"] = config["secondary"].get("generation")
        except Exception as error:
            with self.store.lock:
                row = self.store.db.execute("SELECT body FROM protection_meta WHERE key='secondary'").fetchone()
            result = {**(json.loads(row[0]) if row else {}), "lastAttempt": time.time(), "error": str(error)}
        self.meta("secondary", result)
        return result

    def tick(self):
        with self.store.lock:
            row = self.store.db.execute("SELECT body FROM protection_meta WHERE key='backup'").fetchone()
        previous = json.loads(row[0]) if row else {}
        if previous.get("day") != datetime.now().date().isoformat() and time.time() - previous.get("lastAttempt", 0) > 300:
            self.backup()
            return
        config = self.config()
        if config["enabled"] and config["secondary"]["enabled"] and previous.get("id"):
            with self.store.lock:
                row = self.store.db.execute("SELECT body FROM protection_meta WHERE key='secondary'").fetchone()
            secondary = json.loads(row[0]) if row else {}
            copied = (secondary.get("sourceId"), secondary.get("generation")) == (previous["id"], config["secondary"].get("generation")) and not secondary.get("error")
            if not copied and time.time() - secondary.get("lastAttempt", 0) > 300 and self.backup_lock.acquire(blocking=False):
                try:
                    self.replicate(self.data / "backups" / previous["id"], config)
                finally:
                    self.backup_lock.release()

    def start(self):
        def run():
            while not self.stop.wait(30):
                try:
                    self.tick()
                except Exception:
                    pass  # The persistent status exposes errors without killing the scheduler.
        self.worker = threading.Thread(target=run, name="yantu-data-protection", daemon=True)
        self.worker.start()

    def close(self):
        self.stop.set()
        if self.worker:
            self.worker.join()
