"""Run a local Python function and upload bounded, measured spans to the isolated Observer."""
import argparse
import inspect
import json
from pathlib import Path
import runpy
import secrets
import sys
import time

from tools.observability.observer.contracts.events import identifier
from tools.observability.observer.storage.store import encode
from tools.observability.laboratory.scenarios import post, read_observer, span


MAX_FUNCTIONS = 128


class FunctionRun:
    def __init__(self, source_file, watched):
        self.source_file = source_file
        self.watched = watched
        self.trace_id = secrets.token_hex(16)
        self.root_id = secrets.token_hex(8)
        self.active = {}
        self.spans = []
        self.entered = 0
        self.omitted = 0

    def record(self, name, ident, parent, started, status):
        wall, monotonic, cpu = started
        cpu = time.thread_time_ns() - cpu
        elapsed = time.perf_counter_ns() - monotonic
        item = span(self.trace_id, name, wall, wall + elapsed, parent=parent, span_id=ident)
        item["status"]["code"] = status
        # CPU accounting may advance in coarse ticks. Preserve the wall-clock
        # trace when the CPU reading cannot fit inside the measured call.
        if 0 <= cpu <= elapsed:
            item["attributes"] = [{"key":"nebula.thread_cpu_ns","value":{"intValue":str(cpu)}}]
        self.spans.append(item)

    @staticmethod
    def started():
        return time.time_ns(), time.perf_counter_ns(), time.thread_time_ns()

    def capture(self, frame, event, _value):
        if event == "return":
            saved = self.active.pop(id(frame),None)
            if saved is not None:
                # A Python profile return event also occurs while unwinding. Leave child status unset.
                self.record(*saved,status=0)
        elif event == "call" and frame.f_code.co_filename == self.source_file:
            name = frame.f_code.co_name
            if not identifier(name) or (self.watched and name not in self.watched):
                return
            if self.entered >= MAX_FUNCTIONS:
                self.omitted += 1
                return
            parent = next(reversed(self.active.values()))[1] if self.active else self.root_id
            self.active[id(frame)] = name,secrets.token_hex(8),parent,self.started()
            self.entered += 1

    def run(self, target, args):
        started = self.started()
        old = sys.getprofile()
        status = 2
        try:
            sys.setprofile(self.capture)
            result = target(*args)
            if inspect.iscoroutine(result) or inspect.isgenerator(result) or inspect.isasyncgen(result):
                if not inspect.isasyncgen(result):
                    result.close()
                raise TypeError("use a synchronous function that executes the async or generator body")
            status = 1
        finally:
            sys.setprofile(old)
            self.record("linux-function-run",self.root_id,None,started,status)


def calculate(limit):
    return sum(number*number for number in range(limit))


def check_order(values):
    return sorted(values)[-1]


def demo_workload():
    return check_order([calculate(limit) for limit in (90000,120000,30000)])


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--run-dir",type=Path,required=True)
    cli.add_argument("--producer",default="nebula-functions")
    modes = cli.add_subparsers(dest="mode",required=True)
    modes.add_parser("demo")
    call = modes.add_parser("call")
    call.add_argument("file",type=Path)
    call.add_argument("function")
    call.add_argument("--args-json",default="[]")
    call.add_argument("--watch",action="append",default=[],help="Additional allowed function name; omit to capture this file")
    args = cli.parse_args()

    session = json.loads((args.run_dir/"session.json").read_text(encoding="utf-8"))
    if session["profile"] not in ('local','metrics'):
        cli.error("this recorder uses the local Observer. Use a configured OTLP SDK for the full stack")
    base = f"http://127.0.0.1:{session['ports']['observer']}"
    credentials = json.loads((args.run_dir/"producers.json").read_text(encoding="utf-8"))
    producer = credentials.get(args.producer,{})
    if producer.get("domain")!="function" or producer.get("simulated") is not False:
        cli.error("configure a real function producer in the lab")
    if args.mode=="demo":
        source = Path(__file__).resolve()
        target, supplied, watched = demo_workload, [], {"calculate","check_order","demo_workload"}
    else:
        source = args.file.resolve()
        supplied = json.loads(args.args_json)
        if not isinstance(supplied,list) or not identifier(args.function) or any(not identifier(name) for name in args.watch):
            cli.error("supply an entry name, an optional JSON array and valid watched function names")
        sys.path.insert(0,str(source.parent))
        target = runpy.run_path(str(source),run_name="nebula_observed_function").get(args.function)
        if not inspect.isfunction(target) or target.__code__.co_filename!=str(source):
            cli.error("entry must be a Python function defined in this local file")
        watched = {args.function,*args.watch} if args.watch else set()

    recording = FunctionRun(str(source),watched)
    failure = None
    try:
        recording.run(target,supplied)
    except Exception as error:
        failure = error
    payload = {"resourceSpans":[{"resource":{"attributes":[
        {"key":"service.name","value":{"stringValue":args.producer}}]},
        "scopeSpans":[{"spans":recording.spans}]}]}
    post(base,"/ingest/v1/function-traces",payload,producer["token"])
    verified = read_observer(base,"/api/v1/traces/"+recording.trace_id)
    if len(verified["spans"])!=len(recording.spans):
        raise RuntimeError("stored function spans did not match this run") from failure
    print(encode({"traceId":recording.trace_id,"spans":len(recording.spans),"omitted":recording.omitted,
                  "status":"failed" if failure else "complete",
                  "path":"/?view=traces&trace="+recording.trace_id}),flush=True)
    if failure is not None:
        raise failure


if __name__=="__main__":
    main()
