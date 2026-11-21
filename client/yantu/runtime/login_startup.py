"""Optional current-user login startup. Nothing is installed on import/read."""
import hashlib
import os
import subprocess
from pathlib import Path


class CurrentUserRun:
    path = r'Software\Microsoft\Windows\CurrentVersion\Run'

    def read(self, name):
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.path) as key:
                value, kind = winreg.QueryValueEx(key, name)
            return value if kind == winreg.REG_SZ else object()
        except FileNotFoundError:
            return None

    def write(self, name, command):
        import winreg
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, self.path, 0, winreg.KEY_SET_VALUE) as key:
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, command)

    def remove(self, name):
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, self.path, 0, winreg.KEY_SET_VALUE) as key:
            winreg.DeleteValue(key, name)


class LoginStartup:
    def __init__(self, root, data, port, registry=None):
        self.root, self.data, self.port = Path(root).resolve(), Path(data).resolve(), port
        self.executable = self.root / 'pc' / 'yantu_pc_006.exe'
        self.registry = registry if registry is not None else CurrentUserRun() if os.name == 'nt' else None
        self.name = 'Yantu_pc_006_' + hashlib.sha256((str(self.root) + '\0' + str(self.data) + '\0' + str(port)).encode()).hexdigest()[:12]
        self.command = subprocess.list2cmdline([str(self.executable), '--data-dir', str(self.data), '--port', str(port)])

    def status(self):
        supported = self.registry is not None and self.executable.is_file() and not self.executable.is_symlink()
        current = self.registry.read(self.name) if self.registry else None
        return {'loginStartupSupported': supported, 'loginStartupApplied': current == self.command,
                'loginStartupConflict': current is not None and current != self.command}

    def configure(self, enabled, confirmation=''):
        if type(enabled) is not bool or enabled and confirmation != 'ENABLE_LOGIN_STARTUP':
            raise ValueError('启用登录启动需要本机明确确认')
        if not self.status()['loginStartupSupported']:
            raise ValueError('电脑版托盘尚未就绪，未添加启动项')
        current = self.registry.read(self.name)
        if current is not None and current != self.command:
            raise ValueError('启动项归属不符，未覆盖或移除其他内容')
        if enabled:
            if current is None:
                self.registry.write(self.name, self.command)
        elif current is not None:
            self.registry.remove(self.name)
        if self.status()['loginStartupApplied'] != enabled:
            raise ValueError('系统未确认启动项变化，请在电脑核对')
        return self.status()


def configure(config, body, is_pc, startup=None):
    before = config.status()
    result = config.configure(body, is_pc=is_pc)
    if startup and 'loginStartupRequested' in body:
        try:
            startup.configure(body['loginStartupRequested'], body.get('confirmation', ''))
        except Exception:
            config.configure({**{key: before[key] for key in ('recoveryEnabled', 'loginStartupRequested')},
                              'confirmation': 'ENABLE_LOGIN_STARTUP'}, is_pc=True)
            raise
    return {**result, **(startup.status() if startup else {})}
