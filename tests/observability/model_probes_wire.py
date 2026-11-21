"""Check asynchronous model instrumentation against a real loopback OTLP receiver.

The existing resilience executable uses scripted Providers. This validates the
wire/lifecycle, not a real model's output, quality or inference performance.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--binary', type=Path, required=True)
    cli.add_argument('--scope-binary', type=Path)
    cli.add_argument('--output', type=Path, required=True)
    args = cli.parse_args()
    assert args.binary.name == 'phase11_ai_resilience_tests', 'Use the existing scripted-Provider test executable'
    captured = []
    lock = threading.Lock()

    class Receiver(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            length = int(self.headers.get('Content-Length', '0'))
            assert self.path == '/v1/traces' and 0 < length <= 256 * 1024
            payload = json.loads(self.rfile.read(length))
            with lock:
                assert len(captured) < 128, 'Unexpected exporter volume'
                captured.append(payload)
            self.send_response(200)
            self.send_header('Content-Length', '2')
            self.end_headers()
            self.wfile.write(b'{}')

    server = ThreadingHTTPServer(('127.0.0.1', 0), Receiver)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    def received():
        with lock:
            return [span for payload in captured for resource in payload['resourceSpans']
                    for scope in resource['scopeSpans'] for span in scope['spans']]
    environment = {**os.environ, 'WEB_OBSERVABILITY':'1', 'WEB_OBSERVABILITY_LOG':'off',
                   'WEB_BUSINESS_LOG_LEVEL':'off',
                   'WEB_OBSERVABILITY_OTLP':f'http://127.0.0.1:{server.server_port}'}
    scope_cancel = None
    try:
        result = subprocess.run([str(args.binary.resolve())], timeout=20, capture_output=True,
                                env=environment)
        assert result.returncode == 0, 'Scripted resilience contract failed'
        spans = received()
        if args.scope_binary:
            assert args.scope_binary.name == 'function3_business_observability_contract'
            with lock:
                captured.clear()
            result = subprocess.run([str(args.scope_binary.resolve())], timeout=20, capture_output=True,
                                    env=environment)
            assert result.returncode == 0, 'Synchronous/asynchronous operation contract failed'
            scope_cancel = any(span['name']=='PrivateGenerationService::dispatch' and span['status']['code']==0
                               for span in received())
            assert scope_cancel, 'Cancelled preparation was exported as success'
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    assert spans and all(span['name']=='ResilientModelProvider::generation' for span in spans)
    assert len({(span['traceId'],span['spanId']) for span in spans}) == len(spans), 'Duplicate completion exported'
    statuses = {span['status']['code'] for span in spans}
    assert statuses == {0,1,2}, 'Success, cancellation and failure were not all delivered'
    assert all(int(span['endTimeUnixNano']) >= int(span['startTimeUnixNano']) for span in spans)
    assert any(int(span['endTimeUnixNano'])-int(span['startTimeUnixNano']) >= 5_000_000 for span in spans), \
        'Async timer ended before the delayed callback/watchdog'
    # The wire schema has no model text or request identity field.
    allowed = {'traceId','spanId','parentSpanId','name','kind','startTimeUnixNano','endTimeUnixNano','status'}
    assert all(set(span) <= allowed for span in spans)
    report = {'passed':True,'provider':'scripted-test-only','transport':'real-loopback-OTLP-HTTP',
        'spansReceived':len(spans),'statusCodes':sorted(statuses),'duplicateSpanIds':False,
        'cancelledPreparationIsUnset':scope_cancel,
        'privateContentFields':False,'exactShutdownFlushClaimed':False,'realModelQualityClaimed':False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
