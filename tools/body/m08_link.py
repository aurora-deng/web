"""Own a loopback-only TCP test link with a bounded disconnect switch."""
from contextlib import closing
from select import select
import socket
import socketserver
import threading


class TestLink(socketserver.ThreadingTCPServer):
    daemon_threads = True

    def __init__(self, upstream_port):
        self.upstream = ("127.0.0.1", upstream_port)
        self.guard = threading.Lock()
        self.enabled = True
        self.connections = set()
        self.slots = threading.BoundedSemaphore(6)
        super().__init__(("127.0.0.1", 0), Relay)
        self.worker = threading.Thread(target=self.serve_forever, daemon=True)

    @property
    def endpoint(self):
        return f"127.0.0.1:{self.server_address[1]}"

    def set_enabled(self, enabled):
        with self.guard:
            self.enabled = enabled
            if not enabled:
                for pair in self.connections:
                    for sock in pair:
                        try:
                            sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass

    def __enter__(self):
        self.worker.start()
        return self

    def __exit__(self, *err):
        self.set_enabled(False)
        self.shutdown()
        self.worker.join()
        self.server_close()


class Relay(socketserver.BaseRequestHandler):
    def handle(self):
        switch = self.server
        if not switch.slots.acquire(blocking=False):
            return
        try:
            with switch.guard:
                if not switch.enabled:
                    return
            with closing(socket.create_connection(switch.upstream, timeout=0.8)) as peer:
                pair = (self.request, peer)
                with switch.guard:
                    if not switch.enabled:
                        return
                    switch.connections.add(pair)
                try:
                    peer.settimeout(0.8)
                    self.request.settimeout(0.8)
                    while True:
                        readable, _, _ = select(pair, (), (), 0.15)
                        for source in readable:
                            data = source.recv(65536)
                            if not data:
                                return
                            (peer if source is self.request else self.request).sendall(data)
                finally:
                    with switch.guard:
                        switch.connections.discard(pair)
        except OSError:
            pass
        finally:
            switch.slots.release()
