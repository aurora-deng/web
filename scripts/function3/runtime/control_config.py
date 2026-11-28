"""Load only non-secret Agent control startup settings; never access account data."""
import json
import os
from pathlib import Path
import stat

MAX_BYTES = 4096
MAX_IDS = 64
MAX_USER_ID = (1 << 64) - 1


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate control configuration key")
        result[key] = value
    return result


def _ids(value):
    if not isinstance(value, list) or len(value) > MAX_IDS:
        raise ValueError("control ids must be a bounded list")
    result = []
    for item in value:
        if isinstance(item, bool) or not isinstance(item, (int, str)):
            raise ValueError("control ids must be positive uint64 values")
        text = str(item)
        if not text.isascii() or not text.isdecimal() or not 1 <= len(text) <= 20:
            raise ValueError("invalid control id")
        numeric = int(text)
        if not 1 <= numeric <= MAX_USER_ID or str(numeric) != text or text in result:
            raise ValueError("invalid or duplicate control id")
        result.append(text)
    return ",".join(result)


def load_control_environment(runtime):
    """Absent config preserves legacy startup. Invalid config fails closed.

    This file contains only enablement and existing operator/approver IDs, not
    credentials. No token, private key, environment file or database is read.
    """
    path = Path(runtime) / "agent-control.json"
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    if path.is_symlink():
        raise ValueError("control configuration must not be a symlink")
    try:
        fd = os.open(path, flags)
    except FileNotFoundError:
        return None
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_BYTES:
            raise ValueError("control configuration must be a bounded regular file")
        if os.name != "nt" and (info.st_mode & 0o022 or info.st_uid != os.getuid()):
            raise ValueError("control configuration must be owner-controlled")
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError("control configuration exceeds size limit")
    try:
        data = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (ValueError, UnicodeError):
        raise ValueError("invalid control configuration JSON") from None
    if not isinstance(data, dict) or set(data) != {"schemaVersion", "enabled", "operators", "approvers"}:
        raise ValueError("unexpected control configuration fields")
    if type(data["schemaVersion"]) is not int or data["schemaVersion"] != 1 or type(data["enabled"]) is not bool:
        raise ValueError("unsupported control configuration version or enablement")
    operators, approvers = _ids(data["operators"]), _ids(data["approvers"])
    if data["enabled"] and (not operators or not approvers):
        raise ValueError("enabled control requires explicit operators and approvers")
    return {"WEB_AGENT_CONTROL_ENABLE": "1" if data["enabled"] else "0",
            "WEB_AGENT_CONTROL_USER_IDS": operators,
            "WEB_AGENT_CONTROL_APPROVER_IDS": approvers}
