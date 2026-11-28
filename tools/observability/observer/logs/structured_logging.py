"""A nonblocking producer and bounded background sink for Observer diagnostics."""
import json
import hashlib
from datetime import datetime,timezone,timedelta
import os
from pathlib import Path
import queue
import re
import secrets
import sys
import threading
import time

from tools.observability.observer.logs.log_store import LEVELS, MESSAGES,ERROR_TYPES,normalize
from tools.observability.observer.contracts.events import Invalid
from tools.observability.observer.storage.store import Conflict


def prune_log_files(directory,prefix):
    """Only files owned by this sink; seven UTC dates, never recursive deletion."""
    floor=(datetime.now(timezone.utc)-timedelta(days=6)).strftime("%Y-%m-%d")
    for path in Path(directory).glob(prefix+"-????-??-??.log"):
        if path.name[len(prefix)+1:-4]<floor and not path.is_symlink():
            path.unlink(missing_ok=True)


class DiagnosticLogger:
    def __init__(self, logs, console=False, capacity=512,output_dir=None):
        self.logs, self.console = logs, console
        self.queue = queue.Queue(capacity)
        self.done = threading.Event()
        self.dropped = 0
        self.written = 0
        self.output_dir=Path(output_dir) if output_dir else None
        if self.output_dir:self.output_dir.mkdir(parents=True,exist_ok=True,mode=0o700)
        self.file_dropped=0
        self.worker = threading.Thread(target=self.work, name="observer-diagnostics", daemon=True)
        self.worker.start()

    def emit(self, code, level="ERROR", **fields):
        if code not in MESSAGES or level not in LEVELS:
            return
        frame=sys._getframe(1)
        error=sys.exc_info()[1]
        if error is not None:
            fields.setdefault("errorType",type(error).__name__ if type(error).__name__ in ERROR_TYPES else "Exception")
            trace=sys.exc_info()[2]
            while trace:
                frame=trace.tb_frame
                line=trace.tb_lineno
                trace=trace.tb_next
        else:line=frame.f_lineno
        path=Path(frame.f_code.co_filename)
        root=Path(__file__).resolve().parents[4]
        try:name=path.resolve().relative_to(root).as_posix()
        except ValueError:name=path.name
        if re.fullmatch(r"[a-zA-Z0-9_./-]+",name) and not name.startswith(".") and ".." not in name:
            fields.setdefault("sourceFile",name)
            fields.setdefault("sourceLine",line)
        row = {"schemaVersion": 1, "recordId": secrets.token_hex(16), "timeUnixNano": str(time.time_ns()),
               "level": level, "component": "observer", "code": code, "processId": os.getpid(),
               "threadId": str(threading.get_ident()), **fields}
        try:
            self.queue.put_nowait(row)
        except queue.Full:
            self.dropped += 1

    def work(self):
        while not self.done.is_set() or not self.queue.empty():
            try:
                batch = [self.queue.get(timeout=.1)]
            except queue.Empty:
                continue
            while len(batch)<64:
                try:
                    batch.append(self.queue.get_nowait())
                except queue.Empty:
                    break
            try:
                self.logs.append("observer", batch)
                self.written += len(batch)
                try:
                    self.write_file(batch)
                except OSError:
                    self.file_dropped+=len(batch)
                if self.console:
                    for row in batch:
                        print("NEBULA_LOG "+json.dumps(row, ensure_ascii=False, separators=(",", ":")), file=sys.stderr, flush=True)
            except Exception:
                # Failure reporting must not recursively report its own storage failure.
                self.dropped += len(batch)
            finally:
                for _ in batch:
                    self.queue.task_done()

    def write_file(self,batch):
        if not self.output_dir:return
        prune_log_files(self.output_dir,"observer")
        path=self.output_dir/("observer-"+datetime.now(timezone.utc).strftime("%Y-%m-%d")+".log")
        content="".join("NEBULA_LOG "+json.dumps(row,ensure_ascii=False,separators=(",",":"))+"\n" for row in batch).encode()
        if (path.stat().st_size if path.exists() else 0)+len(content)<=8*1024**2:
            with path.open("ab") as stream:stream.write(content)
            os.chmod(path,0o600)
        else:self.file_dropped+=len(batch)

    def status(self):
        return {"queued": self.queue.qsize(), "written": self.written, "dropped": self.dropped,"fileDropped":self.file_dropped}

    def close(self):
        self.done.set()
        self.worker.join(timeout=2)


class LogTail:
    """Only opt-in, explicitly selected files; ignores legacy free-text lines and oversize records."""
    def __init__(self, logs, files,directories=()):
        self.logs = logs
        self.files = [(source, Path(path), None, 0) for source, path in files]
        self.directories=[(source,Path(path)) for source,path in directories]
        self.rejected = 0
        self.done = threading.Event()
        self.worker = threading.Thread(target=self.work, name="observer-log-tail", daemon=True)
        self.worker.start()

    def tick(self):
        known={path for _,path,_,_ in self.files}
        for source,directory in self.directories:
            if not directory.is_dir():continue
            prune_log_files(directory,"nebula")
            for path in sorted(directory.glob("nebula-????-??-??.log")):
                if path not in known and not path.is_symlink() and len(self.files)<64:
                    self.files.append((source,path,None,0));known.add(path)
        for index, (source, path, identity, offset) in enumerate(self.files):
            try:
                stat = path.stat()
                with path.open("rb") as prefix:
                    current = (stat.st_dev, stat.st_ino, hashlib.sha256(prefix.read(128)).digest())
                if current != identity or stat.st_size < offset:
                    offset = max(0, stat.st_size-256*1024)
                batch = []
                with path.open("rb") as stream:
                    stream.seek(offset)
                    # A partial first line is ignored; only complete, prefixed records are accepted.
                    consumed = 0
                    for _ in range(512):
                        begin = stream.tell()
                        line = stream.readline(8193)
                        if not line:
                            break
                        if not line.endswith(b"\n") and len(line)<8193:
                            stream.seek(begin)
                            break
                        consumed += len(line)
                        if len(line)<=8192 and line.startswith(b"NEBULA_LOG "):
                            try:
                                batch.append(json.loads(line[11:]))
                            except (ValueError, UnicodeError):
                                self.rejected += 1
                        if consumed >= 256*1024:
                            break
                    offset = stream.tell()
                accepted=[]
                for row in batch:
                    try:
                        accepted.append(normalize(row,time.time_ns(),self.logs.store.retention))
                    except Invalid:
                        self.rejected += 1
                for begin in range(0,len(accepted),128):
                    group=accepted[begin:begin+128]
                    try:self.logs.append(source,group)
                    except Conflict:
                        for row in group:
                            try:self.logs.append(source,[row])
                            except Exception:self.rejected+=1
                    except Exception:self.rejected+=len(group)
                self.files[index] = (source, path, current, offset)
            except OSError:
                continue
        managed={p for _,p in self.directories}
        self.files=[entry for entry in self.files if entry[1].parent not in managed or entry[1].exists()]

    def work(self):
        while not self.done.is_set():
            try:self.tick()
            except OSError:self.rejected+=1
            self.done.wait(1)

    def close(self):
        self.done.set()
        self.worker.join(timeout=2)
