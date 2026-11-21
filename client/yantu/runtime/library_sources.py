"""Registered external resources with read-only lookup and opt-in recovery.

Resolution checks size and mtime, not a fresh hash of multi-gigabyte originals.
Returned paths are checked at resolution time; consumers must open them read-only.
An explicitly enabled backup policy may recover a missing file before returning it;
existing or modified primary files are never overwritten.
"""

import json
import os
import re
import stat
import threading
from functools import lru_cache
from pathlib import Path, PureWindowsPath


SAFE_ID = re.compile(r"[A-Za-z0-9_.-]{1,100}")
_FORMAT_ERROR = "资源库清单损坏或格式不受支持"
_UNSAFE_PATH = "资源路径不安全，已拒绝读取"
_MISSING_FILE = "资源文件缺失或资源盘未连接"
_REPARSE_POINT = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
_CATALOG_LOCK = threading.Lock()


def _valid_id(value):
    return (isinstance(value, str) and bool(SAFE_ID.fullmatch(value))
            and not value.endswith(".") and not PureWindowsPath(value).is_reserved())


def _component(value):
    return (isinstance(value, str) and bool(value) and value not in {".", ".."}
            and not re.search(r'[\\/:*?"<>|\x00-\x1f\x7f]', value)
            and not value.endswith((".", " "))
            and not PureWindowsPath(value).is_reserved())


def _integer(value, minimum=0):
    return type(value) is int and value >= minimum


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(_FORMAT_ERROR)
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError(_FORMAT_ERROR)


def _validate_manifest(document):
    if not isinstance(document, dict) or document.get("format") != "yantu-resource-library-v1":
        raise ValueError(_FORMAT_ERROR)
    for key in ("primaryRoot", "backupRoot"):
        root = document.get(key)
        if (not isinstance(root, str) or not root or re.search(r"[\x00-\x1f\x7f]", root)
                or not Path(root).is_absolute() or ".." in Path(root).parts):
            raise ValueError(_FORMAT_ERROR)
    entries = document.get("entries")
    if not isinstance(entries, dict):
        raise ValueError(_FORMAT_ERROR)
    validated = {}
    for identity, entry in entries.items():
        if not _valid_id(identity) or not isinstance(entry, dict):
            raise ValueError(_FORMAT_ERROR)
        relative = entry.get("relativePath")
        if not isinstance(relative, str):
            raise ValueError(_FORMAT_ERROR)
        parts = tuple(re.split(r"[\\/]", relative))
        if (Path(relative).is_absolute() or PureWindowsPath(relative).drive
                or not all(_component(part) for part in parts)):
            raise ValueError(_UNSAFE_PATH)
        if (not _component(entry.get("name")) or not isinstance(entry.get("type"), str)
                or re.search(r"[\x00-\x1f\x7f]", entry["type"])
                or not _integer(entry.get("size")) or not _integer(entry.get("mtimeNs"))
                or not isinstance(entry.get("sha256"), str)
                or not re.fullmatch(r"[0-9a-fA-F]{64}", entry["sha256"])):
            raise ValueError(_FORMAT_ERROR)
        if "pageCount" in entry and not _integer(entry["pageCount"], 1):
            raise ValueError(_FORMAT_ERROR)
        info = {"id": identity, "name": entry["name"], "type": entry["type"],
                "size": entry["size"], "sha256": entry["sha256"], "library": True,
                "backupMode": "separate-resource-copy"}
        if "pageCount" in entry:
            info["pageCount"] = entry["pageCount"]
        validated[identity] = {"parts": parts, "info": info, "mtimeNs": entry["mtimeNs"]}
    return Path(os.path.abspath(document["primaryRoot"])), validated


def _no_link_status(path):
    status = path.lstat()
    if stat.S_ISLNK(status.st_mode) or getattr(status, "st_file_attributes", 0) & _REPARSE_POINT:
        raise ValueError(_UNSAFE_PATH)
    return status


def _checked_file(root, parts):
    """Reject links/reparse points, including those in the root's ancestors."""
    try:
        current = Path(root.anchor)
        directories = [current]
        for part in root.parts[1:]:
            current = current / part
            directories.append(current)
        for directory in directories:
            if not stat.S_ISDIR(_no_link_status(directory).st_mode):
                raise ValueError(_UNSAFE_PATH)
        for index, part in enumerate(parts):
            current = current / part
            status = _no_link_status(current)
            if index < len(parts) - 1 and not stat.S_ISDIR(status.st_mode):
                raise ValueError(_UNSAFE_PATH)
        if not stat.S_ISREG(status.st_mode):
            raise ValueError("资源不是普通文件，已拒绝读取")
        resolved_root = root.resolve(strict=True)
        resolved = current.resolve(strict=True)
        if resolved == resolved_root or not resolved.is_relative_to(resolved_root):
            raise ValueError(_UNSAFE_PATH)
        # Recheck the leaf after canonicalization to catch simple replacements.
        final = _no_link_status(resolved)
        if not stat.S_ISREG(final.st_mode) or (status.st_dev, status.st_ino) != (final.st_dev, final.st_ino):
            raise ValueError(_UNSAFE_PATH)
        return resolved, final
    except (FileNotFoundError, NotADirectoryError):
        raise ValueError(_MISSING_FILE) from None
    except (OSError, RuntimeError):
        raise ValueError("资源文件无法读取") from None


class Catalog:
    def __init__(self, data):
        self._manifest = Path(os.path.abspath(data)) / "resource-library.json"
        self._lock = threading.RLock()
        self._signature = None
        self._root = None
        self._backup_root = None
        self._entries = {}

    def _refresh(self):
        # The caller holds the lock; only publish a fully validated snapshot.
        for _ in range(3):
            try:
                before = _no_link_status(self._manifest)
                if not stat.S_ISREG(before.st_mode):
                    raise ValueError(_FORMAT_ERROR)
                signature = (before.st_mtime_ns, before.st_size)
                if signature == self._signature:
                    return
                with self._manifest.open("r", encoding="utf-8-sig") as stream:
                    opened = os.fstat(stream.fileno())
                    document = json.load(stream, object_pairs_hook=_unique_object,
                                         parse_constant=_reject_constant)
                    finished = os.fstat(stream.fileno())
                after = _no_link_status(self._manifest)
                if any((item.st_mtime_ns, item.st_size) != signature
                       for item in (opened, finished, after)):
                    continue
                root, entries = _validate_manifest(document)
            except FileNotFoundError:
                self._signature, self._root, self._backup_root, self._entries = None, None, None, {}
                return
            except (OSError, ValueError, RecursionError):
                raise ValueError(_FORMAT_ERROR) from None
            self._root, self._entries, self._signature = root, entries, signature
            self._backup_root = Path(os.path.abspath(document['backupRoot']))
            return
        raise ValueError("资源库清单正在更新，请稍后重试")

    def info(self, identity):
        if not _valid_id(identity):
            return None
        with self._lock:
            self._refresh()
            entry = self._entries.get(identity)
            return dict(entry["info"]) if entry else None

    def ids(self):
        with self._lock:
            self._refresh()
            return set(self._entries)

    def descriptor(self, identity):
        info = self.info(identity)
        return {**info, "storage": "resource-library", "included": False} if info else None

    def backup_plan(self):
        with self._lock:
            self._refresh()
            return {'primary': self._root, 'backup': self._backup_root, 'signature': self._signature,
                    'entries': {identity: {**entry, 'info': dict(entry['info'])} for identity, entry in self._entries.items()}}

    def backup_signature(self):
        with self._lock:
            self._refresh()
            return self._signature

    def backup_summary(self):
        with self._lock:
            self._refresh()
            return {'available': bool(self._root), 'registeredFiles': len(self._entries),
                    'registeredBytes': sum(entry['info']['size'] for entry in self._entries.values())}

    def resolve(self, identity, allow_restore=True):
        if not _valid_id(identity):
            raise ValueError("资源编号无效")
        with self._lock:
            self._refresh()
            entry = self._entries.get(identity)
            root = self._root
        if entry is None:
            raise ValueError("资源未登记")
        try:
            path, status = _checked_file(root, entry["parts"])
        except ValueError as error:
            if not allow_restore or str(error) != _MISSING_FILE:
                raise
            from resource_backup import manager_for
            if not manager_for(self._manifest.parent).restore_missing(identity):
                raise
            path, status = _checked_file(root, entry["parts"])
        if status.st_size != entry["info"]["size"] or status.st_mtime_ns != entry["mtimeNs"]:
            raise ValueError("资源文件版本已变化，请重新导入确认")
        return path


@lru_cache(maxsize=32)
def _catalog(data):
    return Catalog(data)


def _for_data(data):
    with _CATALOG_LOCK:
        return _catalog(os.path.abspath(data))


def public_info(data, identity):
    return _for_data(data).info(identity)


def resolve_file(data, identity):
    """Prefer legacy files; preserve a missing legacy Path for unregistered IDs."""
    if not _valid_id(identity):
        raise ValueError("资源编号无效")
    root = Path(os.path.abspath(data)) / "files"
    try:
        # Only a genuinely absent legacy attachment permits library fallback.
        _no_link_status(root / identity)
    except FileNotFoundError:
        catalog = _for_data(data)
        return catalog.resolve(identity) if catalog.info(identity) is not None else root / identity
    except OSError:
        raise ValueError("附件文件无法读取") from None
    return _checked_file(root, (identity,))[0]
