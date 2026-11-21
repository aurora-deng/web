"""Manage only the owner's approved, private Tailscale Serve app endpoint."""

import json
import os
import re
import subprocess
import threading
import time
from pathlib import Path

from privacy_ingress import PrivacyIngress, safe_origin


class PrivateConnection:
    def __init__(self, data, app_port, binary=None):
        self.data, self.app_port = Path(data), app_port
        self.binary = Path(binary) if binary else Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'Tailscale' / 'tailscale.exe'
        self.path = self.data / 'private-connection.json'
        self.ingress, self.lock, self.cached, self.checked = None, threading.RLock(), {}, 0
        self.error = ''
        try:
            self.config = json.loads(self.path.read_text(encoding='utf-8')) if self.path.exists() else {'enabled': False}
            if not isinstance(self.config, dict):
                raise ValueError('Invalid configuration')
        except (OSError, ValueError):
            self.config = {'enabled': False}
            self.error = '私有入口配置无法读取，现有学习数据未改动，请在电脑重新配置'
        if self.config.get('enabled'):
            try:
                self.ingress = PrivacyIngress(self.app_port + 2, self.app_port, self.config['url'])
            except (ValueError, OSError):
                self.error = '私有入口的本机转发未启动，请在电脑检查端口或重新启用'

    def command(self, *arguments):
        if not self.binary.is_file():
            raise ValueError('尚未安装已批准的 Tailscale 电脑版')
        result = subprocess.run([str(self.binary), *arguments], capture_output=True, timeout=25, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        output = result.stdout.decode('utf-8-sig', errors='replace')
        if result.returncode:
            raise ValueError('Tailscale 尚未完成登录、HTTPS 授权或本机控制权限；请先在客户端完成授权')
        return output

    def _save(self):
        temporary = self.path.with_suffix('.tmp')
        temporary.write_text(json.dumps(self.config, ensure_ascii=False), encoding='utf-8')
        temporary.replace(self.path)
        self.checked = 0

    def status(self):
        with self.lock:
            if self.checked and time.monotonic() - self.checked < 15:
                return dict(self.cached)
            result = {'available': self.binary.is_file(), 'enabled': bool(self.config.get('enabled')), 'active': False,
                      'state': 'unavailable', 'url': None, 'networkMode': self.config.get('networkMode', 'not-reviewed'), 'applicationOnly': self.config.get('networkMode') == 'preserve-existing', 'error': self.error}
            try:
                status = json.loads(self.command('status', '--json'))
                result['state'] = status.get('BackendState', 'unknown')
                dns = status.get('Self', {}).get('DNSName', '').rstrip('.')
                if dns:
                    origin = safe_origin('https://' + dns)
                    result['url'] = origin if self.config.get('url') == origin else None
                if result['enabled'] and result['state'] == 'Running' and self.ingress:
                    serve = json.loads(self.command('serve', 'status', '--json'))
                    expected = 'http://127.0.0.1:' + str(self.app_port + 2)
                    result['active'] = any(handler.get('Proxy') == expected for host in serve.get('Web', {}).values() for handler in host.get('Handlers', {}).values()) and result['url'] is not None
            except (ValueError, OSError, subprocess.SubprocessError):
                result['error'] = '私有客户端未就绪，请检查登录和客户端连接'
            self.cached, self.checked = result, time.monotonic()
            return dict(result)

    def enable(self, body):
        if body.get('networkMode') != 'preserve-existing' or body.get('acceptExistingNetwork') is not True:
            raise ValueError('请由所有者确认保留组网现有互通；研途仅限制应用数据与电脑管理权限，不会修改其他服务的规则')
        with self.lock:
            status = json.loads(self.command('status', '--json'))
            if status.get('BackendState') != 'Running':
                raise ValueError('请先在电脑和手机完成 Tailscale 登录与连接')
            origin = safe_origin('https://' + status.get('Self', {}).get('DNSName', '').rstrip('.'))
            existing = json.loads(self.command('serve', 'status', '--json'))
            expected = 'http://127.0.0.1:' + str(self.app_port + 2)
            handlers = [handler for host in existing.get('Web', {}).values() for handler in host.get('Handlers', {}).values()]
            if any(handler.get('Proxy') != expected for handler in handlers) or existing.get('AllowFunnel') or any(not value.get('HTTPS') for value in existing.get('TCP', {}).values()):
                raise ValueError('此设备已有其他 Serve/Funnel 配置，未覆盖；请先由所有者检查现有服务')
            if not self.ingress:
                self.ingress = PrivacyIngress(self.app_port + 2, self.app_port, origin)
            try:
                self.command('serve', '--bg', '--https=443', '--yes', expected)
            except Exception:
                if not self.config.get('enabled'):
                    self.ingress.close()
                    self.ingress = None
                raise
            self.config = {'enabled': True, 'url': origin, 'networkMode': 'preserve-existing', 'existingAccessAccepted': True}
            self.error = ''
            self._save()
            return self.status()

    def disable(self):
        with self.lock:
            if self.config.get('enabled'):
                existing = json.loads(self.command('serve', 'status', '--json'))
                expected = 'http://127.0.0.1:' + str(self.app_port + 2)
                if any(handler.get('Proxy') != expected for host in existing.get('Web', {}).values() for handler in host.get('Handlers', {}).values()):
                    raise ValueError('Serve 配置已被其他程序修改，未停止其他服务；请先由所有者核对')
                self.command('serve', '--https=443', 'off')
            self.config['enabled'] = False
            self._save()
            self.close()
            return self.status()

    def close(self):
        if self.ingress:
            self.ingress.close()
            self.ingress = None


if __name__ == '__main__':
    import argparse
    import sys
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--data-dir', required=True)
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--preserve-existing-accepted', action='store_true')
    args = parser.parse_args()
    if not args.run or not args.preserve_existing_accepted:
        parser.error('Only the owner-approved private ingress can be started')
    connection = PrivateConnection(args.data_dir, args.port)
    runtime = Path(args.data_dir) / 'private-runtime.json'
    try:
        result = connection.enable({'networkMode': 'preserve-existing', 'acceptExistingNetwork': True})
        runtime.write_text(json.dumps({'pid': os.getpid(), 'root': str(Path(__file__).resolve().parent), 'port': args.port}), encoding='utf-8')
        print(json.dumps({'active': result['active'], 'kind': 'private', 'applicationOnly': True}), flush=True)
        while True:
            time.sleep(2)
    finally:
        connection.close()
        if runtime.exists() and json.loads(runtime.read_text(encoding='utf-8')).get('pid') == os.getpid():
            runtime.unlink()
