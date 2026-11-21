"""Durable PC reminder scheduling and standards-based Web Push delivery."""

import base64
import calendar
import hashlib
import json
import re
import secrets
import sys
import threading
import time
from datetime import date, datetime, timedelta
from pathlib import Path
from urllib.parse import urlsplit
from activity_classification import task_is_active

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "runtime-libs"))


def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def unb64(value):
    return base64.b64decode(value + "=" * (-len(value) % 4), altchars=b"-_", validate=True)


def rest_day(state, day):
    settings = state.get("reminderSettings") or {}
    if day.isoformat() in (state.get("studySchedule") or {}).get("holidays", []):
        return True
    if settings.get("restDayMode") == "custom":
        return (day.weekday() + 1) % 7 in settings.get("customRestDays", [])
    return day.weekday() in {5, 6}


def exams_today(state, today):
    titles = [str(e.get("title", "考试")) for e in state.get("calendarEvents", []) if e.get("date") == today.isoformat()]
    schedule = state.get("studySchedule") or {}
    try:
        start = date.fromisoformat(state.get("prepStart", ""))
    except (ValueError, TypeError):
        return titles
    if today < start or rest_day(state, today) or (today - start).days > 10000:
        return titles
    count = sum(not rest_day(state, start + timedelta(days=i)) for i in range((today - start).days + 1))
    for key, default, title in [("weekly", 5, "周测"), ("monthly", 25, "月考")]:
        interval = max(1, int(schedule.get(key, default)))
        if count and count % interval == 0:
            titles.append(title)
    months = max(1, int(schedule.get("major", 3)))
    for period in range(1, 200):
        index = start.year * 12 + start.month - 1 + months * period
        year, month = index // 12, index % 12 + 1
        candidate = date(year, month, min(start.day, calendar.monthrange(year, month)[1]))
        for _ in range(366):
            if not rest_day(state, candidate):
                break
            candidate += timedelta(days=1)
        if candidate == today:
            titles.append("大考")
        if candidate >= today:
            break
    return list(dict.fromkeys(titles))


def due_reminders(state, now):
    settings = state.get("reminderSettings") or {}
    if state.get("prepStatus") != "active" or not settings.get("enabled"):
        return []
    today = now.date().isoformat()
    tasks = [t for t in state.get("tasks", []) if task_is_active(t) and t.get("date") and t["date"] <= today]
    results = []

    def add(kind, clock, title, body):
        if not isinstance(clock, str) or not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", clock):
            return
        hour, minute = map(int, clock.split(":"))
        due = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
        # A short recovery window handles a slow network or a recently resumed PC.
        if 0 <= (now - due).total_seconds() < 600:
            results.append({"key": f"{today}:{kind}:{clock}", "title": title, "body": body[:900], "due": due.timestamp(), "kind": kind})

    if settings.get("tasks", True) and tasks:
        body = "；".join(str(t.get("title") or t.get("text") or "未完成任务") for t in tasks[:8])
        for key, default in [("morning", "06:00"), ("noon", "12:00"), ("evening", "18:00")]:
            add("tasks", settings.get(key, default), f"研途 · 剩余 {len(tasks)} 项任务", body)
    if settings.get("review", True) and not rest_day(state, now.date()) and not any(r.get("date") == today for r in state.get("reviews", [])):
        add("review", "21:00", "研途 · 每日复盘", "记录今天的学习收获与下一步安排")
    if settings.get("special", True):
        exams = exams_today(state, now.date())
        if exams:
            add("exam", settings.get("morning", "06:00"), "研途 · 今日考试安排", "；".join(exams))
    return results


def native_plan(state, now=None, revision=None):
    """A bounded offline snapshot using the same rules as the PC scheduler."""
    now = now or datetime.now().astimezone()
    settings = state.get('reminderSettings') or {}
    clocks = {settings.get(key, default) for key, default in [('morning', '06:00'), ('noon', '12:00'), ('evening', '18:00')] if isinstance(settings.get(key, default), str)}
    clocks.add('21:00')
    events = {}
    expires = (now + timedelta(days=7)).timestamp()
    for index in range(8):
        day = now + timedelta(days=index)
        for clock in clocks:
            if not re.fullmatch(r'(?:[01]\d|2[0-3]):[0-5]\d', clock):
                continue
            hour, minute = map(int, clock.split(':'))
            at = day.replace(hour=hour, minute=minute, second=0, microsecond=0)
            if at.timestamp() < now.timestamp() - 600 or at.timestamp() >= expires:
                continue
            for event in due_reminders(state, at):
                events[event['key']] = {'key': event['key'], 'title': event['title'], 'body': event['body'],
                                        'at': int(event['due'] * 1000), 'kind': event['kind']}
    return {'format': 'yantu-native-plan-v1', 'serverTime': int(now.timestamp() * 1000),
            'validUntil': int((now + timedelta(days=7)).timestamp() * 1000), 'revision': revision,
            'enabled': state.get('prepStatus') == 'active' and bool(settings.get('enabled')),
            'events': sorted(events.values(), key=lambda event: (event['at'], event['key']))}


def validate_subscription(value):
    endpoint = value.get("endpoint", "")
    parsed = urlsplit(endpoint)
    host = parsed.hostname or ""
    allowed = host in {"fcm.googleapis.com", "android.googleapis.com", "updates.push.services.mozilla.com"} or any(host.endswith("." + domain) for domain in ("push.services.mozilla.com", "push.apple.com", "notify.windows.com"))
    if parsed.scheme != "https" or parsed.username or parsed.password or parsed.port not in {None, 443} or not allowed or len(endpoint) > 3000 or parsed.fragment:
        raise ValueError("不支持此通知服务地址，请使用支持 Web Push 的安卓浏览器")
    try:
        keys = value["keys"]
        public = unb64(keys["p256dh"])
        auth = unb64(keys["auth"])
        if len(public) != 65 or public[0] != 4 or len(auth) != 16:
            raise ValueError()
        from cryptography.hazmat.primitives.asymmetric import ec
        ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), public)
    except (KeyError, ValueError, TypeError):
        raise ValueError("通知订阅密钥无效，请重新授权") from None
    return {"endpoint": endpoint, "keys": {"p256dh": keys["p256dh"], "auth": keys["auth"]}}


class ReminderService:
    def __init__(self, storage, data, sender=None):
        self.store, self.data = storage, Path(data)
        self.sender = sender or self.send_push
        self.stop = threading.Event()
        self.worker = None
        self.error = None
        self.last_tick = None
        self.key_path = self.data / "push-private.pem"
        self.public_key = None
        with self.store.lock:
            self.store.db.executescript("""
                CREATE TABLE IF NOT EXISTS push_devices (
                    id TEXT PRIMARY KEY, subscription TEXT NOT NULL, name TEXT NOT NULL,
                    origin TEXT NOT NULL, active INTEGER NOT NULL, created REAL NOT NULL, error TEXT);
                CREATE TABLE IF NOT EXISTS reminders (
                    id INTEGER PRIMARY KEY AUTOINCREMENT, event_key TEXT UNIQUE NOT NULL,
                    title TEXT NOT NULL, body TEXT NOT NULL, due REAL NOT NULL, kind TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS push_deliveries (
                    event_id INTEGER NOT NULL, device_id TEXT NOT NULL, status TEXT NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0, next_try REAL NOT NULL,
                    accepted REAL, received REAL, error TEXT, ack TEXT NOT NULL UNIQUE,
                    PRIMARY KEY(event_id, device_id));
            """)
            self.store.db.commit()
        try:
            from cryptography.hazmat.primitives.asymmetric import ec
            from cryptography.hazmat.primitives import serialization
            if self.key_path.exists():
                private = serialization.load_pem_private_key(self.key_path.read_bytes(), password=None)
            else:
                private = ec.generate_private_key(ec.SECP256R1())
                self.key_path.write_bytes(private.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            self.public_key = b64(private.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint))
        except ImportError:
            self.error = "通知组件尚未安装，电脑内提醒记录仍可使用"

    def start(self):
        self.worker = threading.Thread(target=self.run, name="pc-reminders", daemon=True)
        self.worker.start()

    def close(self):
        self.stop.set()
        if self.worker:
            self.worker.join()

    def run(self):
        while not self.stop.is_set():
            try:
                self.tick()
                self.error = None if self.public_key else self.error
            except Exception as error:
                self.error = str(error)[:300]
            self.stop.wait(3)

    def subscribe(self, body, origin):
        if not self.public_key:
            raise ValueError(self.error)
        if urlsplit(origin).scheme != "https" and urlsplit(origin).hostname not in {"localhost", "127.0.0.1"}:
            raise ValueError("手机通知需要 HTTPS 远程入口")
        subscription = validate_subscription(body.get("subscription") or {})
        identity = hashlib.sha256(subscription["endpoint"].encode()).hexdigest()
        with self.store.lock:
            count = self.store.db.execute("SELECT COUNT(*) FROM push_devices WHERE active=1").fetchone()[0]
            existing = self.store.db.execute("SELECT 1 FROM push_devices WHERE id=?", (identity,)).fetchone()
            if count >= 20 and not existing:
                raise ValueError("已达到 20 台通知设备上限，请先移除旧设备")
            self.store.db.execute("INSERT INTO push_devices VALUES (?,?,?,?,1,?,NULL) ON CONFLICT(id) DO UPDATE SET subscription=excluded.subscription,name=excluded.name,origin=excluded.origin,active=1,error=NULL",
                                  (identity, json.dumps(subscription), str(body.get("name") or "安卓手机")[:60], origin, time.time()))
            self.store.db.commit()
        return {"id": identity}

    def unsubscribe(self, identity):
        with self.store.lock:
            self.store.db.execute("UPDATE push_devices SET active=0 WHERE id=?", (identity,))
            self.store.db.execute("UPDATE push_deliveries SET status='cancelled' WHERE device_id=? AND status='queued'", (identity,))
            self.store.db.commit()

    def enqueue(self, key, title, body, due, kind):
        with self.store.lock:
            cursor = self.store.db.execute("INSERT OR IGNORE INTO reminders(event_key,title,body,due,kind) VALUES (?,?,?,?,?)", (key, title, body, due, kind))
            if cursor.rowcount:
                event_id = cursor.lastrowid
                for (device_id,) in self.store.db.execute("SELECT id FROM push_devices WHERE active=1").fetchall():
                    self.store.db.execute("INSERT INTO push_deliveries(event_id,device_id,status,next_try,ack) VALUES (?,?,'queued',?,?)", (event_id, device_id, due, secrets.token_urlsafe(32)))
            else:
                event_id = self.store.db.execute("SELECT id FROM reminders WHERE event_key=?", (key,)).fetchone()[0]
            self.store.db.commit()
        return event_id

    def test(self, delay):
        delay = 30 if delay else 0
        due = time.time() + delay
        event_id = self.enqueue("test:" + secrets.token_hex(12), "研途 · 电脑定时提醒验证", "这条提醒由电脑后台发送，请在手机通知栏查看。", due, "test")
        return {"id": event_id, "due": int(due * 1000), "devices": self.status()["deviceCount"]}

    def receipt(self, token):
        with self.store.lock:
            cursor = self.store.db.execute("UPDATE push_deliveries SET received=COALESCE(received,?),status='received' WHERE ack=? AND attempts>0", (time.time(), token))
            self.store.db.commit()
        return bool(cursor.rowcount)

    def tick(self, now=None):
        now = now or datetime.now().astimezone()
        epoch = now.timestamp()
        state = self.store.snapshot()["state"]
        for event in due_reminders(state, now):
            self.enqueue(event["key"], event["title"], event["body"], event["due"], event["kind"])
        enabled = state.get("prepStatus") == "active" and (state.get("reminderSettings") or {}).get("enabled")
        with self.store.lock:
            if not enabled:
                self.store.db.execute("UPDATE push_deliveries SET status='cancelled' WHERE status='queued' AND event_id IN (SELECT id FROM reminders WHERE kind!='test')")
            self.store.db.execute("UPDATE push_deliveries SET status='expired' WHERE status='queued' AND event_id IN (SELECT id FROM reminders WHERE due<?)", (epoch - 3600,))
            rows = self.store.db.execute("""SELECT d.event_id,d.device_id,d.attempts,d.ack,r.title,r.body,p.subscription,p.origin
                FROM push_deliveries d JOIN reminders r ON r.id=d.event_id JOIN push_devices p ON p.id=d.device_id
                WHERE d.status='queued' AND d.next_try<=? AND r.due<=? AND p.active=1 LIMIT 20""", (epoch, epoch)).fetchall()
            self.store.db.commit()
        for event_id, device_id, attempts, ack, title, body, subscription, origin in rows:
            if self.stop.is_set():
                break
            latest = self.store.snapshot()["state"]
            if latest.get("prepStatus") != "active" or not (latest.get("reminderSettings") or {}).get("enabled"):
                with self.store.lock:
                    kind = self.store.db.execute("SELECT kind FROM reminders WHERE id=?", (event_id,)).fetchone()[0]
                    if kind != "test":
                        self.store.db.execute("UPDATE push_deliveries SET status='cancelled' WHERE event_id=? AND device_id=?", (event_id, device_id))
                        self.store.db.commit()
                        continue
            payload = {"title": title, "body": body, "tag": f"yantu-{event_id}", "eventId": event_id, "ack": ack, "url": origin + "/mobile"}
            with self.store.lock:
                self.store.db.execute("UPDATE push_deliveries SET attempts=attempts+1 WHERE event_id=? AND device_id=?", (event_id, device_id))
                self.store.db.commit()
            try:
                self.sender(json.loads(subscription), payload, origin)
                with self.store.lock:
                    self.store.db.execute("UPDATE push_deliveries SET status=CASE WHEN received IS NULL THEN 'accepted' ELSE 'received' END,accepted=?,error=NULL WHERE event_id=? AND device_id=?", (epoch, event_id, device_id))
                    self.store.db.execute("UPDATE push_devices SET error=NULL WHERE id=?", (device_id,))
                    self.store.db.commit()
            except Exception as error:
                response = getattr(error, "response", None)
                code = getattr(response, "status_code", None)
                message = f"通知服务返回 {code}" if code else type(error).__name__ + ": " + str(error)[:180]
                expired = code in {404, 410}
                permanent = expired or code in {400, 401, 403, 413} or attempts >= 5
                with self.store.lock:
                    self.store.db.execute("UPDATE push_deliveries SET status=?,next_try=?,error=? WHERE event_id=? AND device_id=? AND received IS NULL", ("failed" if permanent else "queued", epoch + min(900, 30 * 2 ** attempts), message, event_id, device_id))
                    self.store.db.execute("UPDATE push_devices SET error=?,active=CASE WHEN ? THEN 0 ELSE active END WHERE id=?", (message, expired, device_id))
                    self.store.db.commit()
        self.last_tick = int(epoch * 1000)

    def send_push(self, subscription, payload, origin):
        from pywebpush import webpush
        import requests
        from remote_access import system_proxies
        subject = origin
        if not origin.startswith("https://"):
            from remote_access import remote_status
            subject = remote_status(self.data).get("url")
            if not subject:
                raise ValueError("电脑系统通知需要先开启 HTTPS 远程入口")
        with requests.Session() as session:
            session.proxies.update(system_proxies())
            response = webpush(subscription_info=subscription, data=json.dumps(payload, ensure_ascii=False),
                               vapid_private_key=str(self.key_path), vapid_claims={"sub": subject},
                               ttl=3600, timeout=15, requests_session=session)
            if response.status_code >= 300:
                response.raise_for_status()

    def status(self):
        with self.store.lock:
            devices = [dict(zip(("id", "name", "active", "created", "error"), row)) for row in self.store.db.execute("SELECT id,name,active,created,error FROM push_devices ORDER BY created DESC")]
            events = []
            for row in self.store.db.execute("SELECT id,title,body,due,kind FROM reminders ORDER BY id DESC LIMIT 50").fetchall():
                item = dict(zip(("id", "title", "body", "due", "kind"), row))
                item["deliveries"] = [dict(zip(("deviceId", "status", "attempts", "accepted", "received", "error"), delivery)) for delivery in self.store.db.execute("SELECT device_id,status,attempts,accepted,received,error FROM push_deliveries WHERE event_id=?", (item["id"],))]
                events.append(item)
        return {"publicKey": self.public_key, "deviceCount": sum(d["active"] for d in devices), "devices": devices, "events": events, "lastTick": self.last_tick, "error": self.error, "scheduler": bool(self.worker and self.worker.is_alive())}
