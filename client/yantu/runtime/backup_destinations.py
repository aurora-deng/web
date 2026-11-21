"""Explicit, owner-bound secondary copies without touching unrelated files."""
import hashlib
import json
import os
import secrets
import shutil
import subprocess
import time
import zipfile
from pathlib import Path


def disk_identity(path):
    path = Path(path).resolve()
    if os.name != "nt":
        probe = path
        while not probe.exists():
            probe = probe.parent
        return {"disk": "device-" + str(probe.stat().st_dev), "volume": path.anchor}
    drive = path.drive.rstrip(":").upper()
    if len(drive) != 1 or not drive.isalpha():
        return {"disk": None, "volume": "network"}
    script = "Get-Partition -DriveLetter '" + drive + "' -ErrorAction Stop | Select-Object -ExpandProperty DiskNumber | ConvertTo-Json -Compress"
    executable = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
    try:
        result = subprocess.run([str(executable), "-NoProfile", "-NonInteractive", "-Command", script],
                                capture_output=True, text=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        return {"disk": None, "volume": drive + ":"}
    if result.returncode:
        return {"disk": None, "volume": drive + ":"}
    number = json.loads(result.stdout.strip())
    return {"disk": "disk-" + str(number), "volume": drive + ":"}


class BackupDestination:
    marker = ".yantu-backup-owner.json"

    def __init__(self, data, owner, probe=disk_identity):
        self.data, self.owner, self.probe = Path(data).resolve(), owner, probe

    def validate(self, value, allow_same_disk=False, initialize=False):
        if not isinstance(value, str) or len(value) > 1024 or "\x00" in value or not Path(value).is_absolute():
            raise ValueError("请选择完整的备份目录，不接受相对路径")
        folder = Path(value).resolve()
        if folder == Path(folder.anchor) or folder == self.data or folder in self.data.parents or folder.is_relative_to(self.data):
            raise ValueError("第二副本不能放在磁盘根目录、原数据目录或其上下级目录")
        source, target = self.probe(self.data), self.probe(folder)
        same = bool(source["disk"] and source["disk"] == target["disk"])
        unknown = not source["disk"] or not target["disk"]
        if (same or unknown) and not allow_same_disk:
            raise ValueError("未确认不同物理硬盘；请明确接受同盘/无法核验风险，或选择另一块硬盘")
        if initialize:
            folder.mkdir(parents=True, exist_ok=True)
        marker = folder / self.marker
        if not folder.is_dir():
            raise ValueError("第二副本目录不可用，可能尚未连接外接盘")
        if marker.exists():
            if json.loads(marker.read_text(encoding="utf-8")).get("owner") != self.owner:
                raise ValueError("此目录属于其他资料库，不能接管或清理")
        elif initialize:
            if next(folder.iterdir(), None) is not None:
                raise ValueError("请选择空的专用备份目录，避免影响已有文件")
            marker.write_text(json.dumps({"owner": self.owner, "format": 1}), encoding="utf-8")
        else:
            raise ValueError("备份目录标识缺失，停止写入和清理，请重新确认目录")
        return folder, {"samePhysicalDisk": same, "diskUnverified": unknown,
                        "warning": "与主数据同属一块物理硬盘，不能抵御硬盘损坏" if same else "物理硬盘未能核验" if unknown else ""}

    def copy(self, source, config, keep, max_bytes):
        folder, info = self.validate(config["path"], config.get("allowSameDisk", False))
        source = Path(source)
        prefix = "yantu-" + self.owner[:12] + "-"
        target = folder / (prefix + source.name.removeprefix("yantu-"))
        temporary = folder / (target.name + "." + secrets.token_hex(5) + ".tmp")
        try:
            if source.stat().st_size > max_bytes:
                raise ValueError("第二副本超过容量上限，原有有效副本未删除")
            shutil.copyfile(source, temporary)
            with source.open("rb") as a, temporary.open("rb") as b:
                digest = hashlib.file_digest(a, "sha256").hexdigest()
                if digest != hashlib.file_digest(b, "sha256").hexdigest():
                    raise ValueError("第二副本校验失败")
            with zipfile.ZipFile(temporary) as archive:
                if archive.testzip():
                    raise ValueError("第二副本压缩包损坏")
            os.replace(temporary, target)
            copies = sorted((path for path in folder.glob(prefix + "*.zip") if path.is_file()), key=lambda path: (path.stat().st_mtime_ns, path.name), reverse=True)
            total = sum(path.stat().st_size for path in copies)
            # Delete only this installation's copies, after the new ZIP is verified.
            for index, path in reversed(list(enumerate(copies))):
                if path != target and (index >= keep or total > max_bytes):
                    size = path.stat().st_size
                    path.unlink()
                    total -= size
            return {"lastSuccess": time.time(), "sourceId": source.name, "id": target.name,
                    "bytes": target.stat().st_size, "sha256": digest, "error": None, **info}
        finally:
            temporary.unlink(missing_ok=True)
