"""Three-minute recall through real HTTP/WS/PG, restricted to the existing test DB.

Only this run's message timestamps are shifted; no sleeps or production writes.
Exactly 180000/180001 ms is covered by the deterministic C++ service contract.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('recall_runtime', ROOT/'tests/phase11/integration/runtime_blackbox.py')
runtime = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runtime
spec.loader.exec_module(runtime)
PG = '/home/pikachu/phase11-deps/postgresql/18.6/bin/psql'
CONNECTION = 'host=/home/pikachu/phase11-data/function3/pg/run port=55434 user=function3_test_admin dbname=function3_runtime_test'

def sql(query):
    return subprocess.check_output([PG, CONNECTION, '-w', '-X', '-qAtc', query], text=True).strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--allow-isolated-writes', action='store_true', required=True)
    args = parser.parse_args()
    assert args.allow_isolated_writes and sql('SELECT current_database()') == 'function3_runtime_test'
    users = [runtime.BrowserClient('127.0.0.1', 18094) for _ in range(3)]
    for i, client in enumerate(users):
        client.register_and_login(f'recall3_{time.time_ns()}_{i}', 'Isolated-recall-test!')
    owner, member, outsider = users
    status, group = owner.request('POST', '/api/conversations', {
        'kind':'group','title':'Three-minute recall contract','memberIds':str(member.user_id)})
    assert status == 201, (status, group)
    conv = int(group['id'])
    sockets = []
    def connect(client):
        sock, reader = client.open_websocket(); sockets.append(sock)
        return sock, reader
    def command(pair, value, expected):
        sock, reader = pair
        sock.sendall(runtime.masked_frame(json.dumps(value).encode()))
        for _ in range(1500):
            event = reader.read_json()
            if event.get('replyTo') == value['id']:
                assert event['type'] == expected, event
                return event
        raise AssertionError('Reply missing')
    def send(pair, key):
        return command(pair, {'v':1,'type':'chat.send','id':key,'conversationId':conv,'content':'recall-window-content'},'chat.accepted')
    def recall(pair, message, key, expected='chat.recall.accepted'):
        return command(pair, {'v':1,'type':'chat.recall','id':key,'conversationId':conv,'replyTo':str(message['id'])},expected)
    def age(message, seconds):
        # The IDs came from this authenticated test session, never from input.
        sql(f"UPDATE messages SET created_at=clock_timestamp()-interval '{int(seconds)} seconds' WHERE id={int(message['id'])} AND conversation_id={conv}")
    try:
        pair = connect(member)
        within = send(pair, 'third-minute'); age(within,150)
        accepted = recall(pair, within,'recall-third-minute')
        assert accepted['status'] == 'recalled' and accepted['id'] == within['id'], accepted
        age(within,600)
        duplicate = recall(pair,within,'recall-again')
        assert duplicate['status'] == 'already_recalled', duplicate
        assert sql(f"SELECT count(*) FROM outbox_events WHERE event_type='message.recalled' AND aggregate_id={int(within['id'])}") == '1'
        expired = send(pair,'too-late'); age(expired,181)
        rejected = recall(pair,expired,'recall-expired','chat.error')
        assert 'expired' in rejected['status'], rejected
        outsider_pair = connect(outsider)
        recall(outsider_pair,expired,'recall-outsider','chat.error')
        status, page = owner.request('GET',f'/api/conversations/{conv}/messages?before=0&limit=100')
        row = next(m for m in page if str(m['id']) == within['id'])
        assert status == 200 and row['state'] == 'recalled' and not row.get('body') and not row.get('attachments'), row
        old = next(m for m in page if str(m['id']) == expired['id'])
        assert old['state'] == 'active' and old['body'] == 'recall-window-content', old
        print(json.dumps({'passed':True,'database':'function3_runtime_test','port':18094,
              'checks':['150s accepted','181s rejected','already recalled remains idempotent after 600s',
                        'one recall outbox event','nonmember denied','offline history masks recalled body'],
              'productionDatabaseWritten':False}))
    finally:
        for sock in sockets: sock.close()

if __name__ == '__main__': main()
