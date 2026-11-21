"""Start a fresh, independent NEBULA Yantu workspace."""
import argparse
from pathlib import Path
import ipaddress
import json
import os
import sys
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[3]
RUNTIME = ROOT / 'client' / 'yantu' / 'runtime'
DEFAULT_PORT = 18771
DEFAULT_DATA = ROOT / 'build-function3' / 'nebula-yantu'
MARKER = 'NEBULA_YANTU_ROOT'
FORMAT = 'nebula-yantu-v1\n'
LAN_NETWORKS = tuple(ipaddress.ip_network(net) for net in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16'))


def separate_data_directory(value):
    directory = Path(os.path.abspath(value.expanduser()))
    for folder in (directory, *directory.parents):
        if folder.is_symlink() or (hasattr(folder, 'is_junction') and folder.is_junction()):
            raise ValueError('研途数据目录及父目录不能是链接')
    if directory == directory.parent or directory.is_relative_to(RUNTIME):
        raise ValueError('请选择研途专用子目录')
    directory.mkdir(parents=True, exist_ok=True)
    marker = directory / MARKER
    if marker.is_file():
        if marker.is_symlink() or marker.stat().st_size != len(FORMAT) or marker.read_bytes() != FORMAT.encode():
            raise ValueError('目录标记无效')
        if any(child.is_symlink() or (hasattr(child, 'is_junction') and child.is_junction())
               for child in directory.iterdir()):
            raise ValueError('独立研途目录内含链接')
        return directory
    if next(directory.iterdir(), None) is not None:
        raise ValueError('该目录含其他数据。请使用专用空目录')
    with marker.open('xb') as output:
        output.write(FORMAT.encode())
    return directory


def launcher_arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-dir', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--port', type=int, default=DEFAULT_PORT)
    parser.add_argument('--lan-host', help='Optional RFC1918 address for same-origin phone pairing')
    parser.add_argument('--python-libs', type=Path, help='Existing optional Python libraries directory')
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535 or args.port in (8765, 18090, 18091):
        parser.error('Choose a dedicated port. Start the host Yantu and NEBULA from their own entrypoints.')
    if args.lan_host:
        try:
            lan = ipaddress.ip_address(args.lan_host)
            if not any(lan in network for network in LAN_NETWORKS):
                raise ValueError()
        except ValueError:
            parser.error('--lan-host requires a private IPv4 address')
    return args


def run(args, data):
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(RUNTIME))
    if args.python_libs:
        sys.path.append(str(args.python_libs.resolve(strict=True)))
    import server
    from desktop_runtime import SupervisorLease

    class WorkspaceHandler(server.Handler):
        def parse_request(self):
            if not super().parse_request():
                return False
            host = self.headers.get('Host', '').lower()
            origin = self.headers.get('Origin')
            path = unquote(urlsplit(self.path).path)
            if host not in self.server.workspace_hosts or (origin and origin != 'http://' + host):
                return self.reject('请从此设备的研途地址打开页面')
            if path.startswith('/api/') and self.headers.get('Sec-Fetch-Site') == 'cross-site':
                return self.reject('请在研途页面内操作')
            if path in {'/api/remote/start', '/api/remote/stop', '/api/private/enable', '/api/private/disable'}:
                return self.reject('此研途实例使用独立地址，局域网配对由启动选项控制')
            return True

        def reject(self, message):
            self.close_connection = True
            self.reply(403, {'error': message}, {'Connection': 'close'})
            return False

        def cors_origin(self):
            return None

        def end_headers(self):
            self.send_header('X-Frame-Options', 'DENY')
            super().end_headers()

    with SupervisorLease(data):
        host = args.lan_host or '127.0.0.1'
        instance = server.PrepServer((host, args.port), data, handler=WorkspaceHandler)
        names = (host,) if args.lan_host else (host, 'localhost')
        instance.workspace_hosts = {f'{name}:{args.port}' for name in names}
        runtime = data / 'runtime.json'
        runtime.write_text(json.dumps({'pid': os.getpid(), 'port': args.port, 'kind': FORMAT.strip()}), encoding='utf-8')
        print(f'NEBULA 研途 http://{host}:{args.port}/', flush=True)
        try:
            instance.serve_forever(poll_interval=.5)
        except KeyboardInterrupt:
            pass
        finally:
            instance.server_close()
            runtime.unlink(missing_ok=True)


if __name__ == '__main__':
    options = launcher_arguments()
    try:
        run(options, separate_data_directory(options.data_dir))
    except (OSError, ValueError) as error:
        raise SystemExit(f'研途启动失败：{error}')
