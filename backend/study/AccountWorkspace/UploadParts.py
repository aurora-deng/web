"""Bounded, retry-safe attachment staging for the companion's request limit."""
import hashlib
import json
import re
import time
from urllib.parse import parse_qs, urlsplit


PACKET = 8 * 1024 * 1024
RANGE = re.compile(r'bytes ([0-9]{1,9})-([0-9]{1,9})/([0-9]{1,9})')


def room_for(store, ignore=None):
    used = store.db.execute('SELECT COALESCE(SUM(size),0) FROM files WHERE id<>?', (ignore or '',)).fetchone()[0]
    uploads = store.data / 'upload-parts'
    now = time.time()
    count = 0
    for file in uploads.glob('*.part'):
        stat = file.stat()
        if now - stat.st_mtime > 86400:
            file.unlink(missing_ok=True)
            file.with_suffix('.json').unlink(missing_ok=True)
        elif file.stem != ignore:
            used += stat.st_size
            count += 1
    return used, count


def receive(handler, runtime, quota):
    parsed = urlsplit(handler.path)
    file_id = parsed.path.rsplit('/', 1)[-1]
    span = RANGE.fullmatch(handler.headers.get('Content-Range', ''))
    length = int(handler.headers.get('Content-Length', '0'))
    if not span or not runtime.valid_file_id(file_id):
        return handler.reply(400, {'error': '附件分段格式无效'})
    start, end, total = map(int, span.groups())
    if not (0 <= start <= end < total <= runtime.MAX_FILE and length == end - start + 1 <= PACKET):
        return handler.reply(400, {'error': '附件分段大小或范围无效'})
    body = handler.rfile.read(length)
    if len(body) != length:
        return handler.reply(400, {'error': '附件分段尚未完整接收'})
    name = runtime.safe_file_name(parse_qs(parsed.query).get('name', [file_id])[0])
    mime = handler.headers.get('Content-Type', 'application/octet-stream').split(';')[0]
    store = handler.store
    try:
        with store.lock:
            existing = store.file_info(file_id)
            if existing:
                with (store.data / 'files' / file_id).open('rb') as installed:
                    installed.seek(start)
                    equal = installed.read(length) == body
                if existing['size'] != total or not equal:
                    return handler.reply(409, {'error': '已有附件内容不同，请使用新的附件编号'})
                return handler.reply(200, {'received': end + 1, 'file': existing if end + 1 == total else None})
            staging = store.data / 'upload-parts'
            staging.mkdir(exist_ok=True)
            part, descriptor = staging / (file_id + '.part'), staging / (file_id + '.json')
            used, incomplete = room_for(store, file_id)
            metadata = {'size': total, 'name': name, 'type': mime}
            present = part.is_file()
            if present:
                if not descriptor.is_file() or json.loads(descriptor.read_text(encoding='utf-8')) != metadata:
                    return handler.reply(409, {'error': '附件信息已变化，请使用新的附件编号'})
                position = part.stat().st_size
            else:
                if incomplete >= 24:
                    return handler.reply(429, {'error': '研途学习库有较多待上传附件，请完成已有上传'})
                position = 0
            if start > position or start < position < end + 1:
                return handler.reply(409, {'error': '附件分段不连续，请重新打开上传队列'})
            if start < position:
                with part.open('rb') as previous:
                    previous.seek(start)
                    if previous.read(length) != body:
                        return handler.reply(409, {'error': '已有附件分段内容不同，请使用新的附件编号'})
            else:
                if used + end + 1 > quota:
                    return handler.reply(413, {'error': '研途学习库附件已达到 512 MiB'})
                if not present:
                    descriptor.write_text(json.dumps(metadata, ensure_ascii=False), encoding='utf-8')
                with part.open('ab') as output:
                    try:
                        output.write(body)
                        output.flush()
                    except OSError:
                        output.truncate(position)
                        raise
            result = None
            if end + 1 == total:
                with part.open('rb') as complete:
                    digest = hashlib.file_digest(complete, 'sha256').hexdigest()
                result = store.install_file(part, file_id, name, mime, total, digest)
                descriptor.unlink(missing_ok=True)
        return handler.reply(200, {'received': end + 1, 'file': result})
    except (OSError, ValueError):
        return handler.reply(503, {'error': '暂时无法保存本次附件分段，请重试'})
