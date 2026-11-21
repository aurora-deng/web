"""Yantu application service, owned by the NEBULA local workspace launcher."""

import argparse
import gzip
import hashlib
import hmac
import io
import json
import mimetypes
import os
import re
import secrets
import socket
import sqlite3
import threading
import time
import zipfile
import subprocess
import tempfile
from datetime import datetime
from http import HTTPStatus
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path, PureWindowsPath
from urllib.parse import parse_qs, quote, unquote, urlsplit
from desktop_runtime import DesktopOwnerService, desktop_api

ROOT = Path(__file__).resolve().parent
WEB = ROOT / "target"
COLLECTIONS = {"tasks", "calendarEvents", "timelines", "timelineItems", "mistakes", "notes", "resources", "mindmaps", "papers", "scores", "reviews", "schools", "exams", "moodNotes", "moodWalls", "focusSessions", "reviewAttempts"}
from workspace_service import COLLECTIONS as WORKSPACE_COLLECTIONS
COLLECTIONS |= WORKSPACE_COLLECTIONS
LABELS = {"mistakes": "错题", "notes": "学习笔记", "resources": "电子资源", "mindmaps": "思维导图", "papers": "真题试卷", "moodNotes": "心情墙"}
SAFE_ID = re.compile(r"^[A-Za-z0-9_.-]{1,100}$")
MAX_JSON = 12 * 1024 * 1024
MAX_FILE = 256 * 1024 * 1024


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


def accepts_gzip(value):
    for item in value.split(','):
        coding, *parameters = item.strip().lower().split(';')
        if coding.strip() != 'gzip':
            continue
        quality = 1.0
        for parameter in parameters:
            name, separator, raw = parameter.strip().partition('=')
            if separator and name == 'q':
                try:
                    quality = float(raw)
                except ValueError:
                    return False
        return 0 < quality <= 1
    return False


def safe_name(value):
    name = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "_", str(value)).strip(" .")[:120]
    return name or "未分类"


def safe_file_name(value):
    cleaned = re.sub(r'[\\/:*?"<>|\x00-\x1f]', "_", str(value)).strip(" .") or "附件"
    extension = Path(cleaned).suffix[:20]
    return cleaned[:240 - len(extension)] + extension if len(cleaned) > 240 else cleaned


def valid_file_id(value):
    return isinstance(value, str) and bool(SAFE_ID.fullmatch(value)) and not value.endswith(".") and not PureWindowsPath(value).is_reserved()


class Storage:
    def __init__(self, data):
        self.data = data
        data.mkdir(parents=True, exist_ok=True)
        (self.data / "files").mkdir(exist_ok=True)
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.db = sqlite3.connect(self.data / "prep.sqlite3", check_same_thread=False)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS document (id INTEGER PRIMARY KEY, revision INTEGER NOT NULL, body TEXT NOT NULL);
            INSERT OR IGNORE INTO document VALUES (1, 0, '{}');
            CREATE TABLE IF NOT EXISTS operations (id TEXT PRIMARY KEY, created REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS sessions (token TEXT PRIMARY KEY, expires REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS files (id TEXT PRIMARY KEY, name TEXT NOT NULL, type TEXT NOT NULL, size INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS file_dates (id TEXT PRIMARY KEY, uploaded REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS file_fingerprints (id TEXT PRIMARY KEY, sha256 TEXT NOT NULL);
        """)
        self.db.commit()
        from workspace_service import index_schema
        index_schema(self.db)
        access_path = self.data / "access.json"
        if access_path.exists():
            self.access = json.loads(access_path.read_text(encoding="utf-8"))
        else:
            self.access = {"code": f"{secrets.randbelow(100000000):08d}"}
            access_path.write_bytes(encode(self.access))
        self.failures = {}
        self.privacy_key = secrets.token_bytes(32)
        from data_guard import DataGuard
        self.guard = DataGuard(self, self.data)

    def revision(self):
        with self.lock:
            return self.db.execute("SELECT revision FROM document WHERE id=1").fetchone()[0]

    def snapshot(self, since=None):
        with self.lock:
            revision = self.revision()
            conflicts = self.db.execute("SELECT COUNT(*) FROM sync_conflicts").fetchone()[0] if hasattr(self, "guard") else 0
            result = {"revision": revision, "time": int(time.time() * 1000), "conflictCount": conflicts}
            if since is not None and str(since) == str(revision):
                return {**result, "unchanged": True}
            body = self.db.execute("SELECT body FROM document WHERE id=1").fetchone()[0]
            return {**result, "state": json.loads(body)}

    def apply(self, operations, commit=True, trusted_obsidian=False):
        if not isinstance(operations, list) or len(operations) > 5000:
            raise ValueError("无效的同步操作")
        with self.changed:
            snapshot = self.snapshot()
            state = snapshot["state"]
            bridge = getattr(self, 'obsidian', None)
            if bridge and not trusted_obsidian:
                bridge.validate_client_operations(operations, state)
            changed = False
            applied_operations = []
            try:
                for op in operations:
                    op_id = op.get("opId", "")
                    if not SAFE_ID.fullmatch(op_id):
                        raise ValueError("无效操作编号")
                    if self.db.execute("SELECT 1 FROM operations WHERE id=?", (op_id,)).fetchone():
                        continue
                    retirement = state.get('resourceRetirement')
                    retired = retirement.get('recordIds', []) if isinstance(retirement, dict) else []
                    if not isinstance(retired, list):
                        retired = []
                    if ((op.get('kind') in {'upsert', 'delete'} and op.get('id') in retired)
                            or (op.get('kind') == 'setting' and op.get('key') == 'resourceRetirement')):
                        self.db.execute('INSERT INTO operations VALUES (?, ?)', (op_id, time.time()))
                        changed = True
                        continue
                    safe_op = self.guard.protect_operation(op, state)
                    if safe_op is None:
                        self.db.execute("INSERT INTO operations VALUES (?, ?)", (op_id, time.time()))
                        changed = True
                        continue
                    op = safe_op
                    kind = op.get("kind")
                    if kind == "setting":
                        key = op.get("key", "")
                        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,79}", key) or key in COLLECTIONS or key in {"constructor", "prototype", "__proto__"}:
                            raise ValueError("无效设置")
                        if op.get("remove"):
                            state.pop(key, None)
                        else:
                            if key == 'planCapacities':
                                from planning_service import validate_capacities
                                validate_capacities(op.get('value'))
                            state[key] = op.get("value")
                    elif kind in {"upsert", "delete"}:
                        col = op.get("collection")
                        record_id = op.get("id", "")
                        if col not in COLLECTIONS or not SAFE_ID.fullmatch(record_id):
                            raise ValueError("无效记录")
                        records = state.setdefault(col, [])
                        index = next((i for i, r in enumerate(records) if r.get("id") == record_id), None)
                        if kind == "delete":
                            if index is not None:
                                self.guard.remember(col, record_id, records[index], None)
                                records.pop(index)
                        else:
                            fields = op.get("fields", {})
                            if not isinstance(fields, dict):
                                raise ValueError("无效记录字段")
                            record = dict(records[index]) if index is not None else {"id": record_id}
                            for key in op.get("unset", []):
                                record.pop(key, None)
                            record.update({k: v for k, v in fields.items() if k not in {"id", "__proto__", "constructor", "prototype"}})
                            record["id"] = record_id
                            if bridge and not trusted_obsidian:
                                bridge.normalize_record(col, record, fields, records[index] if index is not None else None, op.get('unset', []))
                            if "fileEdits" in fields:
                                from media_tools import validate_edit
                                edits = fields["fileEdits"]
                                if not isinstance(edits, dict) or len(edits) > 1000:
                                    raise ValueError("附件编辑记录无效或过多")
                                for edit in edits.values():
                                    validate_edit(edit)
                            from workspace_service import validate_record
                            validate_record(col, record)
                            from subject_tools import validate_update
                            validate_update(col, records[index] if index is not None else None, record)
                            self.guard.remember(col, record_id, records[index] if index is not None else None, record)
                            if index is None:
                                records.append(record)
                            else:
                                records[index] = record
                    else:
                        raise ValueError("未知同步操作")
                    self.db.execute("INSERT INTO operations VALUES (?, ?)", (op_id, time.time()))
                    applied_operations.append(op)
                    changed = True
                if changed:
                    from workspace_service import validate_topics
                    validate_topics(state)
                    from mood_wall import normalize_mood
                    normalize_mood(state, json.loads(self.db.execute("SELECT body FROM document WHERE id=1").fetchone()[0]))
                    self.db.execute("UPDATE document SET revision=?, body=? WHERE id=1", (snapshot["revision"] + 1, encode(state).decode("utf-8")))
                    index_revision = self.db.execute('SELECT revision FROM workspace_index_meta WHERE id=1').fetchone()
                    if index_revision and index_revision[0] == snapshot['revision'] and not any(op.get('collection') in {'moodNotes', 'moodWalls'} for op in operations):
                        from workspace_service import index_record
                        touched = {(op.get('collection'), op.get('id')) for op in operations if op.get('collection')}
                        for collection, identity in touched:
                            current = next((record for record in state.get(collection, []) if record['id'] == identity), None)
                            if current:
                                index_record(self.db, collection, current)
                            else:
                                self.db.execute('DELETE FROM workspace_text WHERE collection=? AND record_id=?', (collection, identity))
                        self.db.execute('UPDATE workspace_index_meta SET revision=? WHERE id=1', (snapshot['revision'] + 1,))
                if bridge and not trusted_obsidian:
                    bridge.after_operations(applied_operations)
                if commit:
                    self.db.commit()
            except Exception:
                self.db.rollback()
                raise
            if changed and commit:
                self.changed.notify_all()
            return self.snapshot()

    def issue_session(self):
        token = secrets.token_urlsafe(32)
        digest = hashlib.sha256(token.encode()).hexdigest()
        with self.lock:
            self.db.execute("DELETE FROM sessions WHERE expires < ?", (time.time(),))
            self.db.execute("INSERT INTO sessions VALUES (?, ?)", (digest, time.time() + 30 * 86400))
            self.db.commit()
        return token

    def authorized(self, token):
        digest = hashlib.sha256(token.encode()).hexdigest()
        with self.lock:
            return bool(self.db.execute("SELECT 1 FROM sessions WHERE token=? AND expires>?", (digest, time.time())).fetchone())

    def file_info(self, file_id):
        if not valid_file_id(file_id):
            return None
        with self.lock:
            row = self.db.execute("SELECT name, type, size FROM files WHERE id=?", (file_id,)).fetchone()
            stamp = self.db.execute("SELECT uploaded FROM file_dates WHERE id=?", (file_id,)).fetchone()
            fingerprint = self.db.execute('SELECT sha256 FROM file_fingerprints WHERE id=?', (file_id,)).fetchone()
        from library_sources import public_info
        linked = public_info(self.data, file_id) if row else None
        if row and not linked:
            from data_guard import linked_resource_info
            linked = linked_resource_info(self, file_id)
        return {"id": file_id, "name": row[0], "type": row[1], "size": row[2], **({'sha256': fingerprint[0]} if fingerprint else {}), **(linked or {}), "uploadedAt": datetime.fromtimestamp(stamp[0]).astimezone().isoformat(timespec="seconds") if stamp else None} if row else None

    def install_file(self, temporary, file_id, name, mime, size, digest):
        with self.lock:
            final = self.data / 'files' / file_id
            known_hash = self.db.execute('SELECT sha256 FROM file_fingerprints WHERE id=?', (file_id,)).fetchone()
            if known_hash and known_hash[0] != digest:
                raise ValueError('原附件不可覆盖，请使用新的附件编号上传')
            if final.is_file():
                with final.open('rb') as current:
                    if hashlib.file_digest(current, 'sha256').hexdigest() != digest:
                        raise ValueError('原附件不可覆盖，请使用新的附件编号上传，历史版本中的原件将保留')
                current_info = self.file_info(file_id)
                if current_info:
                    self.db.execute('INSERT OR IGNORE INTO file_fingerprints VALUES(?,?)', (file_id, digest))
                    self.db.commit()
                    return self.file_info(file_id)
            os.replace(temporary, final)
            with self.db:
                self.db.execute('INSERT OR REPLACE INTO files VALUES (?, ?, ?, ?)', (file_id, name, mime, size))
                self.db.execute('INSERT OR IGNORE INTO file_dates VALUES (?, ?)', (file_id, time.time()))
                self.db.execute('INSERT OR REPLACE INTO file_fingerprints VALUES(?,?)', (file_id, digest))
                self.db.execute('DELETE FROM attachment_text WHERE id=?', (file_id,))
            return self.file_info(file_id)


class WorkspaceServices:
    def __init__(self, data, port, desktop_startup=False, hosted=False, restore_limit=None):
        self.data = data
        self.hosted = hosted
        self.storage = Storage(data)
        from assistant_service import AssistantService
        self.assistant = AssistantService(self.storage, data)
        from assistant_drafts import AssistantDraftService
        from assistant_jobs import AssistantDraftJobs
        self.assistant_drafts = AssistantDraftService(self.storage, self.assistant)
        self.assistant_jobs = AssistantDraftJobs(self.assistant_drafts)
        from workspace_service import WorkspaceService
        self.workspace = WorkspaceService(self.storage, data)
        from planning_service import PlanningService
        self.planning = PlanningService(self.storage)
        from obsidian_bridge import ObsidianBridge
        self.obsidian = ObsidianBridge(self.storage, data)
        from exam_service import ExamService
        self.exam = ExamService(self.storage)
        from restore_planner import RestorePlanner
        self.restore = RestorePlanner(self.storage, self.assistant, data, COLLECTIONS, valid_file_id, safe_file_name,
                                      max_unpacked=restore_limit)
        self.storage.guard.export_assistant = self.assistant.export_data
        from notifications import ReminderService
        self.reminders = ReminderService(self.storage, data)
        if not hosted:
            from private_connection import PrivateConnection
            from login_startup import LoginStartup
            from resource_backup import manager_for
            self.private = PrivateConnection(data, port)
            self.desktop = DesktopOwnerService(data, self.storage.guard,
                self.storage.revision,
                lambda: self.private.status() if self.private.config.get('enabled') else read_remote_status(data),
                startup=LoginStartup(ROOT, data, port) if desktop_startup else None)
            self.resource_backup = manager_for(data)
            self.resource_backup.start()
            self.obsidian.start()
            self.reminders.start()
        self.storage.guard.start()

    def close(self):
        self.obsidian.close()
        if not self.hosted:
            self.resource_backup.close()
            self.private.close()
        self.assistant_jobs.close()
        self.assistant.close()
        self.reminders.close()
        self.restore.close()
        self.storage.guard.close()
        self.storage.db.close()


class PrepServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, data, desktop_startup=False, handler=None):
        super().__init__(address, handler or Handler)
        try:
            self.services = WorkspaceServices(data, self.server_port, desktop_startup)
        except Exception:
            super().server_close()
            raise

    def server_close(self):
        super().server_close()
        self.services.close()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "Yantu"
    sys_version = ""

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass

    @property
    def services(self):
        return self.server.services

    @property
    def store(self):
        return self.services.storage

    def remote_status(self):
        private = self.services.private.status() if self.services.private.config.get('enabled') else {}
        return {**private, 'kind': 'private'} if private.get('enabled') else read_remote_status(self.services.data)

    @property
    def is_pc(self):
        host = urlsplit("http://" + self.headers.get("Host", "")).hostname
        forwarded = any(self.headers.get(h) for h in ("Forwarded", "X-Forwarded-For", "X-Forwarded-Proto", "CF-Connecting-IP", "X-Yantu-Relay", "Tailscale-User-Login", "Tailscale-User-Name", "Tailscale-User-Profile-Pic"))
        origin = self.headers.get("Origin")
        foreign = (origin and urlsplit(origin).netloc != self.headers.get("Host")) or self.headers.get("Sec-Fetch-Site") == "cross-site"
        return self.client_address[0] in {"127.0.0.1", "::1"} and host in {"localhost", "127.0.0.1", "::1"} and not forwarded and not foreign

    def cors_origin(self):
        origin = self.headers.get("Origin", "")
        parsed = urlsplit(origin)
        if parsed.scheme == "https" or (parsed.scheme == "http" and parsed.hostname in {"localhost", "127.0.0.1", "::1"}):
            return origin if parsed.netloc and not parsed.path and not parsed.query and not parsed.fragment else None
        return None

    def end_headers(self):
        origin = self.cors_origin()
        if origin:
            self.send_header("Access-Control-Allow-Origin", origin)
            self.send_header("Vary", "Origin")
        super().end_headers()

    def do_OPTIONS(self):
        if not self.cors_origin():
            self.reply(403, {"error": "请使用安全的手机网址"})
            return
        self.reply(204, b"", {"Access-Control-Allow-Methods": "GET, HEAD, POST, PUT, DELETE, OPTIONS", "Access-Control-Allow-Headers": "Authorization, Content-Type, X-File-Name, X-File-Type, Range", "Access-Control-Max-Age": "600"})

    def log_message(self, fmt, *args):
        route = urlsplit(getattr(self, 'path', '')).path
        if route == '/api/events':
            return
        # Keep malformed request and error logs free of query strings and token headers.
        category = "/api/files/*" if route.startswith("/api/files/") else route if route.startswith("/api/") else "static"
        codes = (str(value) for value in args if str(value).isdigit() and len(str(value)) == 3)
        status = next(codes, 'request')
        print(f"[{self.log_date_time_string()}] {self.command or 'HTTP'} {category} {status}", flush=True)

    def reply(self, status, payload, headers=None, mime="application/json; charset=utf-8"):
        body = payload if isinstance(payload, bytes) else encode(payload)
        compressed = status == 200 and urlsplit(self.path).path in {'/api/state', '/api/sync'} and mime.startswith('application/json') and len(body) >= 16384 and accepts_gzip(self.headers.get('Accept-Encoding', ''))
        if compressed:
            body = gzip.compress(body, compresslevel=1, mtime=0)
        self.send_response(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        if compressed:
            self.send_header('Content-Encoding', 'gzip')
        if mime.startswith('application/json'):
            self.send_header('Vary', 'Accept-Encoding')
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "same-origin")
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def require_auth(self):
        bearer = self.headers.get("Authorization", "")
        if bearer.startswith("Bearer ") and self.store.authorized(bearer[7:]):
            return True
        if self.headers.get("Origin") and urlsplit(self.headers["Origin"]).netloc != self.headers.get("Host"):
            self.reply(401, {"error": "请重新填写配对码连接电脑"})
            return False
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
            token = cookie.get(f"nebula_yantu_{self.server.server_port}")
            if token and self.store.authorized(token.value):
                return True
        except Exception:
            pass
        self.reply(401, {"error": "请先连接此电脑"})
        return False

    def same_origin(self):
        origin = self.headers.get("Origin")
        if (origin and urlsplit(origin).netloc != self.headers.get("Host")) or self.headers.get("Sec-Fetch-Site") == "cross-site":
            bearer = self.headers.get("Authorization", "")
            if self.cors_origin() and (urlsplit(self.path).path == "/api/pair" or (bearer.startswith("Bearer ") and self.store.authorized(bearer[7:]))):
                return True
            self.reply(403, {"error": "不允许跨站请求"})
            return False
        return True

    def read_body(self, limit):
        size = int(self.headers.get("Content-Length", "0"))
        if size < 0 or size > limit:
            raise ValueError("文件过大，请分批上传（单个文件上限 256 MB）")
        return self.rfile.read(size)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = unquote(urlsplit(self.path).path)
        try:
            if path.startswith('/api/desktop/') and desktop_api(self, path):
                return
            if path == "/mobile/":
                query = urlsplit(self.path).query
                self.reply(302, {}, {"Location": "/mobile" + ("?" + query if query else "")})
                return
            if path == "/api/health":
                self.reply(200, {"app": "yantu-local", "version": 3, "dataPath": str(self.services.data) if self.is_pc else None})
                return
            if path.startswith("/api/"):
                if not self.require_auth():
                    return
                if path.startswith('/api/obsidian/'):
                    action = path.rsplit('/', 1)[-1]
                    if action == 'status':
                        self.reply(200, self.services.obsidian.status(local=self.is_pc))
                    elif not self.is_pc:
                        self.reply(403, {'error': 'Obsidian 联动配置与来源只允许电脑本机访问'})
                    elif action == 'config':
                        self.reply(200, self.services.obsidian.configuration())
                    elif action == 'conflicts':
                        self.reply(200, self.services.obsidian.conflicts())
                    elif action == 'source':
                        ref = parse_qs(urlsplit(self.path).query).get('id', [''])[0]
                        self.reply(200, self.services.obsidian.source(ref))
                    else:
                        self.reply(404, {'error': '不存在的联动操作'})
                elif path == "/api/state":
                    since = parse_qs(urlsplit(self.path).query).get("since", [None])[0]
                    self.reply(200, self.store.snapshot(since))
                elif path == "/api/time":
                    local = datetime.now().astimezone()
                    self.reply(200, {"time": int(time.time() * 1000), "utcOffsetMinutes": int(local.utcoffset().total_seconds() / 60), "systemTime": local.isoformat()})
                elif path == '/api/native/plan':
                    from notifications import native_plan
                    snapshot = self.store.snapshot()
                    self.reply(200, native_plan(snapshot['state'], revision=snapshot['revision']))
                elif path == "/api/info":
                    urls = []
                    try:
                        for address in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET) if self.is_pc else []:
                            ip = address[4][0]
                            if not ip.startswith(("127.", "169.254.")):
                                url = f"http://{ip}:{self.server.server_port}"
                                if url not in urls:
                                    urls.append(url)
                    except OSError:
                        pass
                    with self.store.lock:
                        file_count, size = self.store.db.execute("SELECT COUNT(*), COALESCE(SUM(size),0) FROM files").fetchone()
                    remote = self.remote_status()
                    self.reply(200, {"urls": urls, "local": self.is_pc, "code": self.store.access["code"] if self.is_pc else None,
                                     "dataPath": str(self.services.data) if self.is_pc else None, "files": file_count, "bytes": size, "revision": self.store.revision(),
                                     "remote": {"url": remote.get("url") if remote.get("active") else None, "active": remote.get("active", False), "kind": remote.get('kind', 'temporary'), "available": (ROOT / "tools" / "cloudflared.exe").is_file(), "error": remote.get("error") if self.is_pc else None}})
                elif path == '/api/private/status':
                    if not self.is_pc:
                        self.reply(403, {'error': '请在电脑本机配置私有连接'})
                    else:
                        self.reply(200, self.services.private.status())
                elif path == "/api/events":
                    self.events()
                elif path == "/api/protection":
                    self.reply(200, {**self.store.guard.status(local=self.is_pc), "local": self.is_pc})
                elif path == '/api/resource-backup':
                    self.reply(200, {**self.services.resource_backup.status(), 'local': self.is_pc})
                elif path == "/api/workspace/capabilities":
                    self.reply(200, self.services.workspace.capabilities())
                elif path == '/api/planning/focus':
                    self.reply(200, self.services.planning.status(self.planning_owner()))
                elif path == '/api/workspace/assistant-job':
                    self.reply(200, self.services.assistant_jobs.get(self.planning_owner(), parse_qs(urlsplit(self.path).query).get('id', [''])[0]))
                elif path == "/api/workspace/file-version":
                    query = parse_qs(urlsplit(self.path).query)
                    self.reply(200, self.services.workspace.fingerprint(query.get('id', [''])[0]))
                elif path == "/api/workspace/cleanup":
                    self.reply(200, self.services.workspace.cleanup_preview())
                elif path == "/api/workspace/search":
                    query = parse_qs(urlsplit(self.path).query)
                    self.reply(200, self.services.workspace.search(query.get('q', [''])[0], query.get('offset', [0])[0], query.get('limit', [50])[0], query.get('collection', [''])[0], query.get('topic', [''])[0]))
                elif path == "/api/recovery":
                    query = parse_qs(urlsplit(self.path).query)
                    if any(key in query for key in ("offset", "limit", "q", "after", "before")):
                        self.reply(200, self.store.guard.record_page(query.get("kind", ["trash"])[0], query.get("collection", [None])[0], query.get("record", [None])[0], query.get("q", [""])[0], query.get("offset", [0])[0], query.get("limit", [50])[0], query.get("after", [None])[0], query.get("before", [None])[0]))
                    else:
                        self.reply(200, {"records": self.store.guard.records(query.get("kind", ["trash"])[0], query.get("collection", [None])[0], query.get("record", [None])[0])})
                elif path == "/api/protection/backup-file":
                    if not self.is_pc:
                        self.reply(403, {"error": "自动备份仅可从电脑本机下载"})
                    else:
                        identity = parse_qs(urlsplit(self.path).query).get("id", [""])[0]
                        backup = next((item for item in self.store.guard.backups() if item["id"] == identity), None)
                        if not backup:
                            raise ValueError("备份不存在")
                        file_path = self.services.data / "backups" / identity
                        self.send_response(200)
                        self.send_header("Content-Type", "application/zip")
                        self.send_header("Content-Length", str(file_path.stat().st_size))
                        self.send_header("Content-Disposition", "attachment; filename=" + identity)
                        self.send_header("Cache-Control", "no-store")
                        self.end_headers()
                        if self.command != "HEAD":
                            with file_path.open("rb") as stream:
                                while chunk := stream.read(65536):
                                    self.wfile.write(chunk)
                elif path == "/api/notifications":
                    result = self.services.reminders.status()
                    result["local"] = self.is_pc
                    state = self.store.snapshot()["state"]
                    result["settings"] = state.get("reminderSettings", {})
                    result["prepStatus"] = state.get("prepStatus", "not-started")
                    self.reply(200, result)
                elif path == "/api/assistant/status":
                    self.reply(200, {**self.services.assistant.public_config(), "local": self.is_pc})
                elif path == "/api/assistant/snapshot":
                    self.reply(200, self.services.assistant.export_data())
                elif path == "/api/assistant/prompt":
                    from assistant_service import SYSTEM_PROMPT, ROOMS
                    self.reply(200, {"prompt": SYSTEM_PROMPT, "rooms": ROOMS})
                elif path == "/api/assistant/attachment":
                    query = parse_qs(urlsplit(self.path).query)
                    result, sources = self.services.assistant.read_file({"file_id": query.get("id", [""])[0], "page": int(query.get("page", ["1"])[0]), "offset": int(query.get("offset", ["0"])[0])})
                    self.reply(200, {**result, "sources": sources})
                elif path in {"/api/media/page", "/api/media/export"}:
                    from media_tools import render_page, export
                    query = parse_qs(urlsplit(self.path).query)
                    info = self.store.file_info(query.get("file", [""])[0])
                    if not info:
                        raise ValueError("附件不存在")
                    page = int(query.get("page", ["1"])[0])
                    if path.endswith("page"):
                        preview, count = render_page(self.services.data, info, page)
                        self.reply(200, preview.read_bytes(), {"X-Pages": str(count), "Access-Control-Expose-Headers": "X-Pages"}, mime="image/png")
                    else:
                        identity = query.get("record", [""])[0]
                        snapshot = self.store.snapshot()["state"]
                        record = next((record for col in COLLECTIONS for record in snapshot.get(col, []) if record.get("id") == identity), None)
                        if not record or (record.get("fileId") != info["id"] and not any(file.get("id") == info["id"] for file in record.get("files", []))):
                            raise ValueError("记录中没有此附件")
                        pdf = query.get("format", ["png"])[0] == "pdf"
                        marks = [mark for mark in snapshot.get("readingMarks", []) if mark.get("fileId") == info["id"] and (not info.get("sha256") or not mark.get("fingerprint") or mark["fingerprint"] == info["sha256"])]
                        body = export(self.services.data, info, record.get("fileEdits", {}), page, pdf, marks)
                        self.reply(200, body, mime="application/pdf" if pdf else "image/png")
                elif path == "/api/assistant/memories":
                    from assistant_service import MEMORY_ROOMS
                    self.reply(200, {"rooms": MEMORY_ROOMS, "memories": self.services.assistant.memories()})
                elif path == "/api/assistant/conversations":
                    self.reply(200, {"conversations": self.services.assistant.conversations()})
                elif path == "/api/assistant/messages":
                    query = parse_qs(urlsplit(self.path).query)
                    self.reply(200, self.services.assistant.messages(query.get("conversation", [""])[0]))
                elif path == "/api/assistant/job":
                    query = parse_qs(urlsplit(self.path).query)
                    self.reply(200, self.services.assistant.job(query.get("id", [""])[0]))
                elif path == "/api/mobile-qr":
                    import qrcode
                    import qrcode.image.svg
                    remote = self.remote_status()
                    origin = remote.get("url") if remote.get("active") else None
                    if not origin:
                        self.reply(409, {"error": "请先在电脑端开启远程访问"})
                    else:
                        output = io.BytesIO()
                        qrcode.make(origin + "/mobile", image_factory=qrcode.image.svg.SvgPathImage).save(output)
                        self.reply(200, output.getvalue(), mime="image/svg+xml")
                elif path == "/api/export":
                    self.export_backup()
                elif path.startswith("/api/files/"):
                    self.serve_file(path.rsplit("/", 1)[-1])
                else:
                    self.reply(404, {"error": "不存在的接口"})
                return
            relative = "index.html" if path in {"/", "/index.html", "/mobile", "/mobile/"} else path.lstrip("/")
            file_path = (WEB / relative).resolve()
            if not file_path.is_relative_to(WEB.resolve()) or not file_path.is_file():
                self.reply(404, b"Not found", mime="text/plain")
                return
            mime = {'.mjs': 'text/javascript', '.wasm': 'application/wasm'}.get(file_path.suffix.lower()) or mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
            self.reply(200, file_path.read_bytes(), mime=mime)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.reply(500, {"error": str(error)})

    def do_POST(self):
        if not self.same_origin():
            return
        path = urlsplit(self.path).path
        try:
            if path.startswith('/api/desktop/') and desktop_api(self, path):
                return
            if path == "/api/pair":
                now = time.time()
                peer = self.headers.get("CF-Connecting-IP", self.client_address[0]) if self.client_address[0] in {"127.0.0.1", "::1"} else self.client_address[0]
                peer = hmac.new(self.store.privacy_key, peer.encode(), hashlib.sha256).hexdigest()
                self.store.failures = {key: [stamp for stamp in values if now - stamp < 60] for key, values in self.store.failures.items() if values and now - values[-1] < 60}
                attempts = [t for t in self.store.failures.get(peer, []) if now - t < 60]
                if len(attempts) >= 10:
                    self.reply(429, {"error": "配对码错误次数过多，请稍后重试"})
                    return
                body = json.loads(self.read_body(2048))
                if not self.is_pc and not secrets.compare_digest(str(body.get("code", "")), self.store.access["code"]):
                    self.store.failures[peer] = attempts + [now]
                    self.reply(403, {"error": "配对码不正确"})
                    return
                token = self.store.issue_session()
                secure = "; Secure" if self.headers.get("X-Forwarded-Proto") == "https" else ""
                cookie_name = f"nebula_yantu_{self.server.server_port}"
                self.reply(200, {"connected": True, "deviceToken": token}, {"Set-Cookie": f"{cookie_name}={token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=2592000{secure}"})
                return
            if path == "/api/push/receipt":
                body = json.loads(self.read_body(2048))
                token = str(body.get("ack", ""))
                if not re.fullmatch(r"[A-Za-z0-9_-]{40,60}", token):
                    raise ValueError("无效的通知回执")
                self.reply(200, {"received": self.services.reminders.receipt(token)})
                return
            if not self.require_auth():
                return
            if path.startswith('/api/obsidian/'):
                if not self.is_pc:
                    self.reply(403, {'error': '请在电脑本机管理 Obsidian 联动'})
                    return
                body = json.loads(self.read_body(MAX_JSON))
                bridge, action = self.services.obsidian, path.rsplit('/', 1)[-1]
                if action == 'config':
                    result = bridge.configure(body)
                elif action == 'preview':
                    result = bridge.preview(body)
                elif action == 'apply':
                    result = bridge.apply(body)
                elif action == 'scan':
                    result = bridge.scan(force=True)
                elif action == 'resolve':
                    result = bridge.resolve(body)
                else:
                    self.reply(404, {'error': '不存在的联动操作'}); return
                self.reply(200, result)
            elif path == "/api/sync":
                body = json.loads(self.read_body(MAX_JSON))
                operations = body.get("operations", [])
                if body.get("clientId") and isinstance(operations, list):
                    for operation in operations:
                        # Old offline queues have no baseline; preserve them for an explicit choice.
                        if "base" not in operation:
                            operation["base"] = {"exists": False, "value": None} if operation.get("kind") == "setting" else {}
                            if operation.get("kind") == "upsert":
                                operation.update({"baseExists": True, "baseRecord": {}})
                with self.store.changed:
                    self.services.planning.validate_client_operations(operations)
                    if any(op.get('collection') == 'assistantDrafts' for op in operations):
                        raise ValueError('助手确认回执只能通过逐条确认或恢复预览处理')
                    result = self.store.apply(operations)
                self.reply(200, result)
            elif path in {'/api/resource-backup/settings', '/api/resource-backup/run'}:
                body = json.loads(self.read_body(4096))
                if not self.is_pc:
                    self.reply(403, {'error': '请在电脑本机管理电子资源备份'})
                else:
                    result = self.services.resource_backup.configure(body) if path.endswith('/settings') else self.services.resource_backup.run_now()
                    self.reply(200, {**result, 'local': True})
            elif path in {'/api/planning/preview', '/api/planning/apply', '/api/planning/focus'}:
                body = json.loads(self.read_body(MAX_JSON))
                if path.endswith('/preview'):
                    self.reply(200, self.services.planning.plan_preview(body))
                elif path.endswith('/apply'):
                    self.reply(200, self.services.planning.plan_apply(body))
                else:
                    self.reply(200, self.services.planning.focus(body, self.planning_owner()))
            elif path in {'/api/private/enable', '/api/private/disable'}:
                body = json.loads(self.read_body(4096))
                if not self.is_pc:
                    self.reply(403, {'error': '请在电脑本机配置私有连接'})
                else:
                    result = self.services.private.enable(body) if path.endswith('enable') else self.services.private.disable()
                    self.reply(200, result)
            elif path.startswith('/api/workspace/'):
                body = json.loads(self.read_body(MAX_JSON))
                workspace = self.services.workspace
                if path == '/api/workspace/assistant-generate':
                    self.reply(200, self.services.assistant_jobs.start(self.planning_owner(), body))
                elif path == '/api/workspace/assistant-cancel':
                    self.reply(200, self.services.assistant_jobs.cancel(self.planning_owner(), body.get('id')))
                elif path == '/api/workspace/assistant-preview':
                    self.reply(200, self.services.assistant_drafts.preview(body.get('draft')))
                elif path == '/api/workspace/assistant-confirm':
                    self.reply(200, self.services.assistant_drafts.confirm(body))
                elif path == '/api/workspace/assistant-undo':
                    self.reply(200, self.services.assistant_drafts.undo(body))
                elif path in {'/api/workspace/exam-save', '/api/workspace/exam-preview', '/api/workspace/exam-confirm'}:
                    action = {'exam-save': self.services.exam.save, 'exam-preview': self.services.exam.preview, 'exam-confirm': self.services.exam.confirm}[path.rsplit('/', 1)[-1]]
                    self.reply(200, action(body))
                elif path == '/api/workspace/knowledge-preview':
                    self.reply(200, workspace.merge_preview(body))
                elif path == '/api/workspace/knowledge-merge':
                    self.reply(200, workspace.merge(body))
                elif path == '/api/workspace/knowledge-undo':
                    self.reply(200, workspace.undo_merge(body))
                elif path == '/api/workspace/cleanup':
                    if not self.is_pc:
                        self.reply(403, {'error': '请在电脑本机清理未引用附件'})
                    else:
                        self.reply(200, workspace.cleanup(body))
                elif path == '/api/workspace/reading-export':
                    from reading_export import export
                    info = self.store.file_info(body.get('fileId', ''))
                    if not info or not info['name'].lower().endswith('.pdf'):
                        raise ValueError('矢量批注导出只适用于 PDF，图片请使用原有批注导出')
                    marks = [item for item in self.store.snapshot()['state'].get('readingMarks', []) if item.get('fileId') == info['id']]
                    from library_sources import resolve_file
                    self.reply(200, export(resolve_file(self.services.data, info['id']), marks), mime='application/pdf')
                else:
                    self.reply(404, {'error': '不存在的工作区操作'})
            elif path.startswith("/api/protection/") or path.startswith("/api/recovery/"):
                body = json.loads(self.read_body(MAX_JSON))
                guard = self.store.guard
                if path == "/api/protection/resolve":
                    self.reply(200, guard.resolve(body.get("id"), body.get("choice")))
                elif path == "/api/recovery/restore":
                    self.reply(200, guard.restore(body.get("id"), body.get("expected")))
                elif path == "/api/recovery/purge":
                    if body.get("confirmation") != "PERMANENT_DELETE":
                        raise ValueError("需要确认永久删除")
                    guard.purge(body.get("id"))
                    self.reply(200, {"deleted": True})
                elif path in {"/api/protection/settings", "/api/protection/backup"}:
                    if not self.is_pc:
                        self.reply(403, {"error": "请在电脑本机管理自动备份"})
                    elif path.endswith("settings"):
                        self.reply(200, guard.configure(body))
                    else:
                        self.reply(200, guard.backup(force=True))
                else:
                    self.reply(404, {"error": "不存在的保护操作"})
            elif path == "/api/reset-study":
                body = json.loads(self.read_body(2048))
                if body.get("confirmation") != "DELETE_ALL_STUDY_DATA":
                    raise ValueError("需要明确确认删除学习数据")
                self.services.obsidian.pause_for_restore()
                self.store.guard.backup(force=True)
                self.services.assistant.clear_data()
                with self.store.changed:
                    snapshot = self.store.snapshot()
                    state = snapshot["state"]
                    for collection in COLLECTIONS:
                        state[collection] = []
                    state.update({"prepStatus": "not-started", "prepStart": "", "lastTaskRefresh": "", "focusTimer": {"minutes": 25, "remaining": 1500, "running": False, "endAt": None}})
                    state["reminderSettings"] = {**(state.get("reminderSettings") or {}), "enabled": False}
                    self.store.db.execute("UPDATE document SET revision=?,body=? WHERE id=1", (snapshot["revision"] + 1, encode(state).decode("utf-8")))
                    for (identity,) in self.store.db.execute("SELECT id FROM files").fetchall():
                        if valid_file_id(identity):
                            (self.services.data / "files" / identity).unlink(missing_ok=True)
                    self.store.db.execute("DELETE FROM files")
                    self.store.db.execute("DELETE FROM file_dates")
                    self.store.db.execute('DELETE FROM file_fingerprints')
                    self.store.db.execute("DELETE FROM recovery_records")
                    self.store.db.execute("DELETE FROM sync_conflicts")
                    self.store.db.execute("UPDATE push_deliveries SET status='cancelled' WHERE status='queued'")
                    self.store.db.commit()
                    self.store.changed.notify_all()
                self.reply(200, self.store.snapshot())
            elif path == "/api/import":
                self.import_backup()
            elif path == '/api/import/preview':
                self.import_backup(preview=True)
            elif path in {'/api/import/confirm', '/api/import/cancel'}:
                body = json.loads(self.read_body(4096))
                if path.endswith('cancel'):
                    self.reply(200, self.services.restore.cancel(body.get('token'), self.restore_owner()))
                else:
                    self.services.obsidian.pause_for_restore()
                    self.reply(200, self.services.restore.commit(body.get('token'), self.restore_owner(), body))
            elif path == "/api/assistant/device-sync":
                body = json.loads(self.read_body(MAX_JSON))
                self.reply(200, self.services.assistant.device_sync(body.get("operations", [])))
            elif path.startswith("/api/assistant/"):
                body = json.loads(self.read_body(65536))
                assistant = self.services.assistant
                if path == "/api/assistant/settings":
                    if not self.is_pc:
                        self.reply(403, {"error": "模型接口与密钥只能在电脑本机设置"})
                    else:
                        self.reply(200, assistant.configure(body))
                elif path == "/api/assistant/chat":
                    self.reply(200, assistant.submit(body))
                elif path == "/api/assistant/cancel":
                    self.reply(200, assistant.cancel(str(body.get("id", ""))))
                elif path == "/api/assistant/memory":
                    self.reply(200, assistant.save_memory(body, confirmed=bool(body.get("confirmed", True))))
                elif path == "/api/assistant/forget":
                    assistant.delete_memory(str(body.get("id", "")))
                    self.reply(200, {"deleted": True})
                elif path == "/api/assistant/delete-conversation":
                    assistant.delete_conversation(str(body.get("id", "")))
                    self.reply(200, {"deleted": True})
                else:
                    self.reply(404, {"error": "不存在的助手接口"})
            elif path == "/api/push/subscribe":
                body = json.loads(self.read_body(8192))
                origin = self.headers.get("Origin") or "http://" + self.headers.get("Host", "")
                self.reply(200, self.services.reminders.subscribe(body, origin))
            elif path == "/api/push/unsubscribe":
                body = json.loads(self.read_body(2048))
                self.services.reminders.unsubscribe(str(body.get("id", "")))
                self.reply(200, {"removed": True})
            elif path == "/api/push/test":
                body = json.loads(self.read_body(2048))
                if not self.is_pc:
                    self.reply(403, {"error": "请从电脑本机发起定时验证"})
                else:
                    self.reply(200, self.services.reminders.test(body.get("delay", 0)))
            elif path == "/api/pair-code" and self.is_pc:
                self.store.access["code"] = f"{secrets.randbelow(100000000):08d}"
                (self.services.data / "access.json").write_bytes(encode(self.store.access))
                self.reply(200, {"code": self.store.access["code"]})
            elif path == "/api/shutdown" and self.is_pc:
                self.read_body(2048)
                self.reply(200, {"stopping": True})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
            elif path in {"/api/remote/start", "/api/remote/stop"}:
                self.read_body(2048)
                if not self.is_pc:
                    self.reply(403, {"error": "请在电脑本机控制远程连接"})
                    return
                from remote_access import start_remote, stop_remote
                result = start_remote(self.services.data, self.server.server_port) if path.endswith("start") else stop_remote(self.services.data)
                self.reply(200 if not result.get("error") else 503, result)
            else:
                self.reply(404, {"error": "不存在的接口"})
        except (ValueError, KeyError, zipfile.BadZipFile) as error:
            self.reply(400, {"error": str(error)})
        except Exception as error:
            self.reply(500, {"error": str(error)})

    def do_PUT(self):
        if not self.same_origin() or not self.require_auth():
            return
        parsed = urlsplit(self.path)
        file_id = parsed.path.rsplit("/", 1)[-1]
        if not parsed.path.startswith("/api/files/") or not valid_file_id(file_id):
            self.reply(400, {"error": "无效的文件编号"})
            return
        temporary = self.services.data / "files" / f"upload-{secrets.token_hex(12)}.tmp"
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 <= size <= MAX_FILE:
                raise ValueError("单个文件上限 256 MB，请分批上传")
            remaining = size
            digest = hashlib.sha256()
            with temporary.open("wb") as output:
                while remaining:
                    chunk = self.rfile.read(min(65536, remaining))
                    if not chunk:
                        raise ValueError("上传中断，请重试")
                    output.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
            name = safe_file_name(parse_qs(parsed.query).get("name", [file_id])[0])
            mime = self.headers.get("Content-Type", "application/octet-stream").split(";")[0]
            metadata = self.store.install_file(temporary, file_id, name, mime, size, digest.hexdigest())
            self.reply(200, metadata)
        except Exception as error:
            self.close_connection = True
            self.reply(400, {"error": str(error)})
        finally:
            temporary.unlink(missing_ok=True)

    def do_DELETE(self):
        if not self.same_origin() or not self.require_auth():
            return
        file_id = urlsplit(self.path).path.rsplit("/", 1)[-1]
        if not self.path.startswith("/api/files/") or not valid_file_id(file_id):
            self.reply(400, {"error": "无效的文件编号"})
            return
        with self.store.lock:
            state = self.store.snapshot()["state"]
            from data_guard import file_ids
            if file_id in file_ids(state):
                self.reply(409, {"error": "附件仍被其他记录、来源引用或草稿使用"})
                return
            if self.store.guard.references(file_id):
                self.reply(409, {"error": "附件已保留在回收站、历史版本或冲突记录中"})
                return
            self.store.db.execute("DELETE FROM files WHERE id=?", (file_id,))
            self.store.db.execute("DELETE FROM file_dates WHERE id=?", (file_id,))
            self.store.db.execute('DELETE FROM file_fingerprints WHERE id=?', (file_id,))
            self.store.db.execute("DELETE FROM attachment_text WHERE id=?", (file_id,))
            self.store.db.commit()
            (self.services.data / "files" / file_id).unlink(missing_ok=True)
        self.reply(200, {"deleted": True})

    def serve_file(self, file_id):
        info = self.store.file_info(file_id)
        from library_sources import resolve_file
        try:
            path = resolve_file(self.services.data, file_id)
        except ValueError:
            self.reply(404, {"error": "资源库原件不可用，请在电脑端检查资源目录；没有修改原记录"})
            return
        if not info or not path.is_file():
            self.reply(404, {"error": "附件未上传到此电脑，可从原设备导入备份"})
            return
        size = path.stat().st_size
        start, end = 0, size - 1
        range_header = self.headers.get("Range", "")
        status = HTTPStatus.OK
        if range_header:
            match = re.fullmatch(r"bytes=(\d*)-(\d*)", range_header)
            if not match or not any(match.groups()):
                self.reply(416, b"", {"Content-Range": f"bytes */{size}"})
                return
            if match[1]:
                start = int(match[1])
                end = min(end, int(match[2])) if match[2] else end
            else:
                start = max(0, size - int(match[2]))
            if start >= size or end < start:
                self.reply(416, b"", {"Content-Range": f"bytes */{size}"})
                return
            status = HTTPStatus.PARTIAL_CONTENT
        self.send_response(status)
        self.send_header("Content-Type", info["type"])
        self.send_header("Content-Length", str(max(0, end - start + 1)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Access-Control-Expose-Headers", "Content-Range, Accept-Ranges, Content-Length, X-File-SHA256")
        if info.get("sha256"):
            self.send_header("X-File-SHA256", info["sha256"])
        self.send_header("X-Content-Type-Options", "nosniff")
        disposition = 'attachment' if parse_qs(urlsplit(self.path).query).get('download') == ['1'] else 'inline'
        self.send_header("Content-Disposition", disposition + "; filename*=UTF-8''" + quote(info["name"]))
        if status == HTTPStatus.PARTIAL_CONTENT:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if self.command == "HEAD":
            return
        with path.open("rb") as source:
            source.seek(start)
            remaining = max(0, end - start + 1)
            while remaining:
                chunk = source.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        revision = -1
        try:
            while True:
                with self.store.changed:
                    current = self.store.revision()
                    if current == revision:
                        self.store.changed.wait(timeout=20)
                    current = self.store.revision()
                if current != revision:
                    self.wfile.write(f"data: {current}\n\n".encode())
                    revision = current
                else:
                    self.wfile.write(b": heartbeat\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass

    def export_backup(self):
        import tempfile
        file_paths = {}
        with tempfile.TemporaryFile() as output, self.store.guard.staged_snapshot() as (frozen, staging):
            snapshot = frozen["snapshot"]
            all_files = {info["id"]: info for info in frozen["infos"]}
            from data_guard import validate_resource_reference
            file_paths.update({identity: validate_resource_reference(identity, info) for identity, info in all_files.items() if info.get('storage') == 'resource-library'})
            with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("数据.json", encode({"version": 2, **snapshot}))
                archive.writestr("助手记忆与对话.json", encode(frozen["assistant"]))
                recovery = frozen["recovery"]
                conflicts = frozen["conflicts"]
                archive.writestr("保护记录.json", encode({"recovery": recovery, "conflicts": conflicts}))
                for col, label in LABELS.items():
                    for record in snapshot["state"].get(col, []):
                        parts = [safe_name(p.strip()) for p in re.split(r">|/|\\|→", str(record.get("path") or record.get("subject") or "未分类")) if p.strip()]
                        prefix = "/".join([label, *parts, safe_name(record.get("title", "记录")) + "-" + safe_name(record["id"])])
                        archive.writestr(prefix + "/记录.json", encode(record))
                        files = list(record.get("files") or [])
                        if record.get("fileId"):
                            files.append({"id": record["fileId"], "name": record.get("title", "试卷") + ".pdf"})
                        for attachment in files:
                            file_id = attachment.get("id", "")
                            if file_id in file_paths or not valid_file_id(file_id):
                                continue
                            info = all_files.get(file_id)
                            path = staging / file_id
                            if info and path.is_file():
                                entry = prefix + "/" + file_id + "-" + safe_file_name(info["name"])
                                archive.write(path, entry)
                                file_paths[file_id] = {**info, "path": entry}
                from data_guard import file_ids
                for file_id in all_files:
                    if file_id in file_paths:
                        continue
                    info = all_files.get(file_id)
                    path = staging / file_id
                    if info and path.is_file():
                        entry = "恢复附件/" + file_id + "-" + safe_file_name(info["name"])
                        archive.write(path, entry)
                        file_paths[file_id] = {**info, "path": entry}
                archive.writestr("附件清单.json", encode(file_paths))
            output.seek(0, io.SEEK_END)
            length = output.tell()
            output.seek(0)
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Length", str(length))
            self.send_header("Content-Disposition", 'attachment; filename="yantu-backup.zip"')
            self.end_headers()
            while chunk := output.read(65536):
                self.wfile.write(chunk)

    def restore_owner(self):
        cookie = SimpleCookie()
        cookie.load(self.headers.get('Cookie', ''))
        token = self.headers.get('Authorization') or (cookie.get('prep_session').value if cookie.get('prep_session') else '')
        return hashlib.sha256(token.encode()).hexdigest()

    def planning_owner(self):
        bearer = self.headers.get('Authorization', '')
        if bearer.startswith('Bearer '):
            token = bearer[7:]
        else:
            cookie = SimpleCookie()
            cookie.load(self.headers.get('Cookie', ''))
            token = cookie.get('prep_session').value if cookie.get('prep_session') else ''
        return hashlib.sha256(('planning-owner:' + token).encode()).hexdigest()

    def import_backup(self, preview=False):
        size = int(self.headers.get("Content-Length", "0"))
        if not 0 < size <= 20 * 1024 ** 3:
            raise ValueError("备份大小无效或超过 20 GB")
        with tempfile.TemporaryFile(dir=self.services.data) as upload:
            remaining = size
            while remaining:
                chunk = self.rfile.read(min(65536, remaining))
                if not chunk:
                    raise ValueError("备份上传中断")
                upload.write(chunk)
                remaining -= len(chunk)
            upload.seek(0)
            if preview:
                self.reply(200, self.services.restore.prepare(upload, self.restore_owner()))
            else:
                self.import_archive(upload)

    def import_archive(self, upload):
        owner = self.restore_owner()
        preview = self.services.restore.prepare(upload, owner)
        try:
            result = self.services.restore.commit(preview['token'], owner, {'revision': preview['revision'], 'strategy': 'use-backup', 'confirmation': 'RESTORE_BACKUP', 'settings': True, 'assistant': True, 'protection': True})
        except Exception:
            self.services.restore.cancel(preview['token'], owner)
            raise
        self.reply(200, result)


def read_remote_status(data):
    from remote_access import remote_status
    return remote_status(data)


if __name__ == "__main__":
    raise SystemExit("Run python -B -m client.yantu.LocalWorkspace.LocalWorkspace from the NEBULA workspace.")
