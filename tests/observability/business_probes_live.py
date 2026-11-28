"""Real, disposable NEBULA -> metrics/OTLP/logs contract; no production writes.

Run after tools.observability.lab --profile local, against its owned run directory.
All accounts and stories below are synthetic. Never accepts a free-form server URL.
"""
import argparse
from contextlib import closing
import http.client
import json
from pathlib import Path
import secrets
import sys
import time
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/project'))
from repro_runtime import browser_fixture, require
sys.path.insert(0, str(ROOT))
from tools.observability.observer.metrics.local_metrics import parse_metrics, metric
from tools.observability.observer.contracts.operation_catalog import FUNCTION_DEFINITIONS, HTTP_OPERATIONS
from tools.observability.observer.http.access_preflight import prometheus_baseline


def request(port, method, path, data=None, headers=None):
    with closing(http.client.HTTPConnection('127.0.0.1', port, timeout=8)) as connection:
        body = None if data is None else json.dumps(data).encode()
        connection.request(method, path, body, {'Content-Type':'application/json', **(headers or {})})
        response = connection.getresponse()
        return response.status, response.read()


def get(port, path):
    status, body = request(port, 'GET', path)
    require(status == 200, f'Query failed: {path.split("?")[0]} ({status})')
    return json.loads(body)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--run-dir', type=Path, required=True)
    cli.add_argument('--output', type=Path, required=True)
    args = cli.parse_args()
    session = json.loads((args.run_dir / 'session.json').read_text())
    require(session['profile'] in ('local','metrics'), 'Use an owned local lab')
    port, observer = session['nebulaPort'], session['ports']['observer']
    require(port != 18090 and observer not in (18090, 18791), 'Daily services are excluded')
    require((args.run_dir / 'lab.pid').is_file(), 'Lab ownership record is required')
    owner = int((args.run_dir / 'lab.pid').read_text())
    command = Path(f'/proc/{owner}/cmdline').read_bytes().split(b'\0')
    require(b'tools.observability.lab' in command and str(args.run_dir.resolve()).encode() in command,
            'The disposable lab owner is no longer running')
    # Establish the owned target's private metrics credential before any writes.
    scrape_token = (args.run_dir / 'scrape-token').read_text().strip()
    code, _ = request(port, 'GET', '/metrics')
    require(code == 401, 'Anonymous metrics scrape unexpectedly allowed')
    code, _ = request(port, 'GET', '/metrics', headers={'Authorization':'Bearer '+scrape_token})
    require(code == 200, 'Owned ops credential was not accepted')
    if session['profile']=='metrics':prometheus_baseline(session['ports']['prometheus'],port)
    browser = browser_fixture(ROOT)
    gateway = session['ports'].get('gateway')
    origin = f'http://127.0.0.1:{gateway}' if gateway else 'http://localhost'
    clients = [browser.BrowserClient('127.0.0.1', port, origin=origin) for _ in range(2)]
    author, friend = clients
    prefix = 'probes_' + secrets.token_hex(6)
    canary = 'PRIVATE-CONTENT-CANARY-' + secrets.token_hex(12)
    started = int(time.time())
    checks = []

    def expect(client, method, path, payload=None, code=200):
        actual, result = client.request(method, path, payload)
        require(actual == code, f'{method} {path.split("?")[0]} returned {actual}, expected {code}')
        return result

    for index, client in enumerate(clients):
        client.register_and_login(prefix + '_' + str(index), secrets.token_urlsafe(24))
    invitation = expect(author, 'POST', '/api/friend-requests', {'receiverId':str(friend.user_id)}, 201)
    expect(friend, 'POST', '/api/friend-requests/' + str(invitation['id']), {'accept':'true'})
    conversation = expect(author, 'POST', '/api/conversations', {'kind':'direct','peerId':str(friend.user_id)}, 201)
    cid = conversation['id']
    socket, reader = author.open_websocket()
    with closing(socket):
        for index in range(3):
            socket.sendall(browser.masked_frame(json.dumps({'v':1,'type':'chat.send','id':prefix+'-'+str(index),
                'conversationId':cid,'content':canary}).encode()))
            require(browser.wait_for_ws(reader, 'chat.accepted')['status']=='inserted', 'Missing durable ACK')
            browser.wait_for_ws(reader, 'chat.message')
    expect(author, 'GET', '/api/conversations')
    history = expect(author, 'GET', f'/api/conversations/{cid}/messages?after=0&limit=100')
    require(len(history) == 3, 'Acknowledged history differs')
    expect(author, 'GET', f'/api/conversations/{cid}/messages?before=99999&limit=20')
    checks.append('real websocket ACK, history and historyBefore')

    category = expect(author, 'GET', '/api/tree/categories')[0]['id']
    post = expect(author, 'POST', '/api/tree/posts', {'requestId':prefix+'-post','body':canary,'categoryId':category}, 201)
    pid = post['id']
    expect(author, 'GET', '/api/tree/posts?limit=24')
    expect(friend, 'GET', '/api/tree/posts/' + pid)
    expect(friend, 'POST', f'/api/tree/posts/{pid}/like', {'liked':'true'})
    expect(friend, 'POST', f'/api/tree/posts/{pid}/replies', {'requestId':prefix+'-reply','body':canary}, 201)
    expect(author, 'GET', f'/api/tree/posts/{pid}/replies')
    # An expected permission rejection must be observable without retaining content/ID.
    expect(friend, 'DELETE', '/api/tree/posts/' + pid, {}, 403)
    expect(author, 'DELETE', '/api/tree/posts/' + pid, {})
    expect(friend, 'GET', '/api/tree/posts/' + pid, code=404)
    checks.append('tree writes, reads, delete permission and final state')

    code, body = request(port, 'GET', '/metrics', headers={'Authorization':'Bearer '+scrape_token})
    require(code == 200, 'Ops scrape rejected')
    parsed = parse_metrics(body)
    observed = {dict(labels).get('function') for family, labels in parsed if family=='nebula_function_seconds_count'}
    require(observed == {row['symbol'] for row in FUNCTION_DEFINITIONS}, 'Binary/catalog mismatch')
    exercised = ['ChatService::conversations','ChatService::history','ChatService::historyBefore','ChatService::sendMessage',
        'AuthService::registerUser','AuthService::login','TreeService::list','TreeService::get','TreeService::publish',
        'TreeService::like','TreeService::reply','TreeService::replies','TreeService::remove',
        'SqlContext::acquire','SqlContext::execute','SqlContext::transact']
    require(all(metric(parsed,'nebula_function_seconds_count',function=name)>0 for name in exercised),
            'An exercised operation has no histogram count')
    require(metric(parsed,'nebula_function_calls_total',function='TreeService::remove',outcome='error')>=1,
            'Rejected deletion did not increment the error outcome')
    # Completion is delivered before a scope's destructor necessarily runs;
    # background health traffic also shares these global gauges. Require an
    # eventual quiescent sample rather than equating an HTTP response with it.
    deadline = time.monotonic() + 5
    while not all(metric(parsed,'nebula_function_in_flight',function=name)==0 for name in exercised):
        require(time.monotonic() < deadline, 'A completed operation remained in flight')
        time.sleep(.05)
        code, body = request(port, 'GET', '/metrics', headers={'Authorization':'Bearer '+scrape_token})
        require(code == 200, 'Ops scrape rejected while waiting for completed scopes')
        parsed = parse_metrics(body)
    checks.append('26 fixed metrics; 16 exercised operations; error and in-flight accounting')

    traces = {}
    for route in ('/api/conversations','/api/tree/posts'):
        trace, parent = secrets.token_hex(16), secrets.token_hex(8)
        begin = time.time_ns()
        code, _ = request(port, 'GET', route, headers={'Cookie':author.cookie,'traceparent':f'00-{trace}-{parent}-01'})
        require(code == 200, 'Real traced probe failed')
        end = time.time_ns()
        payload = {'resourceSpans':[{'resource':{'attributes':[{'key':'service.name','value':{'stringValue':'nebula-probe'}}]},
            'scopeSpans':[{'spans':[{'traceId':trace,'spanId':parent,'name':'probe GET '+route,'kind':3,
                'startTimeUnixNano':str(begin),'endTimeUnixNano':str(end),'status':{'code':1}}]}]}]}
        code, _ = request(observer, 'POST', '/v1/traces', payload)
        require(code == 200, 'Probe OTLP rejected')
        for _ in range(60):
            status, raw = request(observer, 'GET', '/api/v1/traces/'+trace)
            result = json.loads(raw)
            names = {item['name'] for item in result.get('spans',[])}
            if status == 200 and 'PostgreSQL command' in names and result['status']=='complete':
                break
            time.sleep(.2)
        else:
            raise RuntimeError('Probe -> server -> PostgreSQL trace incomplete')
        require(all(item['parent'] is None or item['parent'] in {s['id'] for s in result['spans']} for item in result['spans']),
                'Trace lost a parent')
        require(canary not in raw.decode(), 'Trace retained private content')
        traces[route] = {'traceId':trace,'spans':len(result['spans']),'operations':sorted(names),'status':result['status']}
    checks.append('two complete real cross-process probe -> HTTP -> service -> PostgreSQL traces')

    # Wait for the independent scrape/tailer, rather than writing fake monitor rows.
    for _ in range(50):
        instrument = get(observer, '/api/v1/instrumentation?'+urlencode({'start':started-1,'end':int(time.time())}))
        if len(instrument.get('observedFunctions') or [])==26 and instrument.get('functionOutcomes'):
            break
        time.sleep(.25)
    require(len(instrument.get('observedFunctions') or [])==26, 'Observer did not collect current capability')
    for path in (args.run_dir / 'structured-logs').glob('*.jsonl'):
        require(canary not in path.read_text(), 'Log retained private content')
    catalog = get(observer, '/api/v1/function-catalog')
    require(len(catalog['functions'])==26 and set(catalog['httpOperations'])==HTTP_OPERATIONS, 'UI catalog differs')
    checks.append('independent scrape, catalog and private-content exclusion')
    result = {'passed':True,'environment':'disposable-real-nebula','simulatedBusinessContent':True,
        'dailyServiceChanged':False,'startedAtUnix':started,'finishedAtUnix':int(time.time()),
        'target':port,'observer':observer,'registeredFunctions':26,'exercisedFunctions':exercised,
        'checks':checks,'traces':traces,'version':get(port,'/health/version')}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'passed':True,'checks':len(checks),'registeredFunctions':26,'output':str(args.output)}))


if __name__ == '__main__':
    main()
