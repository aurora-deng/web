"""Real HTTP -> business -> private PostgreSQL -> MQ/local delivery -> TTS integration.

Run only against scripts/function3/validation/run_server.py's isolated server.
Uses the standard library and existing binaries; never installs dependencies.
"""
import argparse
import base64
import http.client
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid
import wave
import zlib
import importlib.util
import concurrent.futures

ROOT = Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('phase11_runtime_blackbox',ROOT/'tests/phase11/integration/runtime_blackbox.py')
module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
BrowserClient=module.BrowserClient
module.TIMEOUT=30.0

PG = '/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
CONNECTION = 'host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_runtime_test'


def sql(query):
    return subprocess.check_output([PG, CONNECTION, '-w', '-qAtc', query], text=True).strip()


def expect(client, method, path, data=None, code=200):
    status, value = client.request(method, path, data)
    assert status == code, (method, path, status, value)
    return value


def raw(client, method, path, data=b'', headers=None):
    h = {'Cookie': client.cookie, 'X-CSRF-Token': client.csrf,
         'Origin': 'http://localhost', **(headers or {})}
    connection = http.client.HTTPConnection(client.host, client.port, timeout=30)
    connection.request(method, path, data, h)
    response = connection.getresponse()
    result = response.status, response.read(), dict(response.getheaders())
    connection.close()
    return result


def png():
    def chunk(kind, body):
        return struct.pack('!I', len(body)) + kind + body + struct.pack('!I', zlib.crc32(kind + body))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 2, 2, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b'\x00\xff\x80\x80\xff\x80\x80' * 2)) + chunk(b'IEND', b'')


def audio(seconds=1):
    out = io.BytesIO()
    with wave.open(out, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
        w.writeframes(b'\0\0' * (8000 * seconds))
    return out.getvalue()


def upload(client, name, mime, body, valid=True):
    meta = expect(client, 'POST', '/api/tree/media/uploads', {'name': name, 'mediaType': mime, 'size': str(len(body))}, 201)
    for offset in range(0, len(body), 256*1024):
        status, result, _ = raw(client, 'PATCH', f'/api/files/{meta["id"]}/content', body[offset:offset+256*1024], {'Content-Type': 'application/octet-stream', 'Upload-Offset': str(offset)})
        assert status == 200, (status, result)
    expect(client, 'POST', f'/api/files/{meta["id"]}/complete', {})
    status, result = client.request('POST', f'/api/tree/media/{meta["id"]}/validate', {})
    assert (status == 200) == valid, (status, result)
    return meta['id']


def wait_for(call, predicate, seconds=80):
    deadline = time.monotonic()+seconds
    while time.monotonic() < deadline:
        value = call()
        if predicate(value): return value
        time.sleep(.5)
    raise AssertionError('condition did not become true before deadline: '+repr(value))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=18094)
    parser.add_argument('--allow-isolated-admin', action='store_true', required=True)
    parser.add_argument('--webm-fixture-dir',type=Path)
    args = parser.parse_args()
    assert sql('SELECT current_database()') == 'function3_runtime_test'
    token = uuid.uuid4().hex[:12]
    users = [BrowserClient('127.0.0.1', args.port) for _ in range(3)]
    a, b, admin = users
    for i, user in enumerate(users): user.register_and_login('f3_'+token+'_'+str(i), 'Function3-test-only!')
    # Bootstrap is explicit, Linux-only and restricted to this test database.
    # A previous contract may already have a site admin: grant through test SQL only.
    sql(f"INSERT INTO site_roles VALUES({admin.user_id},'administrator') ON CONFLICT(user_id) DO UPDATE SET role='administrator'")
    assert expect(a, 'GET', '/api/admin/me')['role'] == 'user'
    expect(a, 'GET', '/api/admin/users', code=403)
    expect(a, 'GET', '/api/admin/models', code=403)
    expect(admin, 'GET', '/api/admin/models')
    expect(a,'GET','/api/admin/model-operations/1',code=403)
    job=expect(admin,'POST','/api/admin/models/9223372036854775807/versions/9223372036854775807',{'action':'activate','reason':'无效候选的异步失败测试'},202)
    result=wait_for(lambda:expect(admin,'GET','/api/admin/model-operations/'+job['operationId']),lambda r:r['state']!='pending')
    assert result['state']=='failed'

    expect(BrowserClient('127.0.0.1', args.port), 'GET', '/api/tree/posts', code=401)
    category = expect(a, 'GET', '/api/tree/categories')[0]['id']
    image = upload(a, 'flower.png', 'image/png', png())
    voice = upload(a, 'hello.wav', 'audio/wav', audio())
    if args.webm_fixture_dir:
        upload(a,'browser-short.webm','audio/webm;codecs=opus',(args.webm_fixture_dir/'short.webm').read_bytes())
        upload(a,'browser-long.webm','audio/webm;codecs=opus',(args.webm_fixture_dir/'long.webm').read_bytes(),valid=False)
    upload(a, 'fake.png', 'image/png', b'not-an-image', valid=False)
    upload(a, 'long.wav', 'audio/wav', audio(61), valid=False)
    expect(b, 'POST', f'/api/tree/media/{image}/validate', {}, code=403)
    payload = {'requestId': token, 'categoryId': category, 'body': '神树里的真实测试', 'mood': '开心', 'tags': '生活\n验证', 'fileIds': image+','+voice}
    post = expect(a, 'POST', '/api/tree/posts', payload, 201)
    assert expect(a, 'POST', '/api/tree/posts', payload, 201)['id'] == post['id']
    expect(a, 'POST', '/api/tree/posts', {**payload, 'body': '不同内容'}, code=409)
    expect(b, 'POST', '/api/tree/posts', {**payload, 'requestId': token+'-other'}, code=403)
    expect(a, 'POST', f'/api/tree/posts/{post["id"]}/like', {'liked': 'true'})
    liked = expect(b, 'POST', f'/api/tree/posts/{post["id"]}/like', {'liked': 'true'})
    assert liked['likes'] == '1'
    assert expect(b, 'POST', f'/api/tree/posts/{post["id"]}/like', {'liked': 'true'})['likes'] == '1'
    reply = {'requestId': token+'-reply', 'body': '听见了你的回声'}
    first = expect(b, 'POST', f'/api/tree/posts/{post["id"]}/replies', reply, 201)
    assert expect(b, 'POST', f'/api/tree/posts/{post["id"]}/replies', reply, 201)['id'] == first['id']
    child={'requestId':token+'-child','body':'这是一条有明确对象的回复','parentReplyId':first['id']}
    nested=expect(a,'POST',f'/api/tree/posts/{post["id"]}/replies',child,201)
    assert nested['parentReplyId']==first['id'] and nested['replyToName']
    assert expect(a,'POST',f'/api/tree/posts/{post["id"]}/replies',child,201)['id']==nested['id']
    expect(a,'POST',f'/api/tree/posts/{post["id"]}/replies',{**child,'parentReplyId':'0'},code=409)
    expect(a,'POST',f'/api/tree/posts/{post["id"]}/replies',{**child,'requestId':token+'-missing','parentReplyId':'9223372036854775807'},code=400)
    delivery=expect(a,'GET','/api/me/speech/status')['delivery']
    url = post['media'][0]['url']
    status, content, headers = raw(b, 'GET', url)
    assert status == 200 and content.startswith(b'\x89PNG') and headers['Cache-Control'] == 'private,no-store'
    expect(b, 'PATCH', '/api/me/notification-settings', {'sound': 'true', 'voice': 'zm_009'})
    rank = wait_for(lambda: expect(a, 'GET', '/api/tree/ranking'), lambda r: any(e['postId'] == post['id'] for e in r['entries']))
    announcements = wait_for(lambda: expect(a, 'GET', '/api/announcements'), lambda rows: bool(rows))
    announcement = announcements[-1]['id']
    for selected in ('zf_001', 'zm_009'):
        generated = wait_for(lambda: expect(a, 'GET', f'/api/announcements/{announcement}/audio?voice={selected}'), lambda r: r['state'] == 'ready', 100)
        status, content, _ = raw(a, 'GET', generated['url'])
        assert status == 200 and content.startswith(b'RIFF')
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        claims=list(pool.map(lambda _:expect(b,'POST',f'/api/announcements/{announcement}/claim',{})['claimed'],range(2)))
    assert sorted(claims)==[False,True],claims
    expect(a,'POST','/api/admin/speech/preview',{'voice':'zf_001'},code=403)
    preview=expect(admin,'POST','/api/admin/speech/preview',{'voice':'zf_001'})
    assert preview['state'] in ('pending','ready')
    preview=wait_for(lambda:expect(admin,'GET','/api/admin/speech/preview/zf_001'),lambda r:r['state']=='ready',100)
    status,content,_=raw(admin,'GET',preview['url']);assert status==200 and content.startswith(b'RIFF')
    from http2_business import check_http2
    check_http2(a)
    report = expect(b, 'POST', f'/api/tree/posts/{post["id"]}/reports', {'reason': '测试举报'}, 201)
    expect(a, 'POST', f'/api/admin/tree/posts/{post["id"]}/hide', {'reason': '越权'}, code=403)
    expect(admin, 'POST', f'/api/admin/tree/posts/{post["id"]}/hide', {'reason': '隔离测试'})
    expect(a, 'GET', f'/api/tree/posts/{post["id"]}', code=404)
    assert raw(b, 'GET', url)[0] == 404, 'hidden media old URL remained accessible'
    expect(admin, 'POST', f'/api/admin/reports/{report["id"]}/resolve', {'dismiss': 'false', 'reason': '隔离测试完成'})
    # Session revocation is visible to the real HTTP authorization path.
    live_socket,live_reader=b.open_websocket()
    expect(admin, 'POST', f'/api/admin/users/{b.user_id}', {'action': 'revoke', 'reason': '隔离测试'})
    try:
        for _ in range(20):live_reader.read_json()
        raise RuntimeError('revoked websocket did not close')
    except (AssertionError,ConnectionError):pass
    finally:live_socket.close()
    expect(b, 'GET', '/api/me', code=401)
    print(json.dumps({'passed': True, 'post': post['id'], 'rankVersion': rank['version'], 'announcement': announcement,'delivery':delivery,
                      'coverage': ['auth/CSRF', 'real PNG/WAV decode', 'malformed/long audio rejection', 'media ownership',
                                   'persistent idempotency', 'likes/replies', 'rank/announcement', 'durable delivery/TTS both voices',
                                   'playback claim dedup', 'report/hide/old media URL', 'session revocation']}, ensure_ascii=False))


if __name__ == '__main__': main()
