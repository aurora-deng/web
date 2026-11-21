"""Normalize shared sticky-note walls under the document transaction lock."""
import math
import secrets
from datetime import datetime

COLORS = {"lemon", "rose", "mint", "sky", "lilac"}


def normalize_mood(state, old_state=None):
    if "moodNotes" not in state and "moodWalls" not in state:
        return
    old_notes = {n["id"]: n for n in (old_state or {}).get("moodNotes", [])}
    walls = {w["id"]: dict(w) for w in state.get("moodWalls", [])}
    notes = state.get("moodNotes", [])
    if not isinstance(notes, list) or len(notes) > 6000:
        raise ValueError("心情便签数量无效")
    counts = {}
    missing = {}
    spillovers = {}
    now = datetime.now().astimezone().isoformat(timespec="seconds")
    for note in notes:
        note["text"] = str(note.get("text") or "")[:6000]
        if not note["text"].strip() and not note.get("files"):
            raise ValueError("便签需要文字或照片")
        created = note.get("createdAt") or now
        try:
            parsed = datetime.fromisoformat(str(created).replace("Z", "+00:00"))
            created = parsed.astimezone().isoformat(timespec="seconds")
        except (ValueError, TypeError):
            created = now
        note["createdAt"] = old_notes.get(note["id"], {}).get("createdAt") or created
        if note.get("color") not in COLORS:
            note["color"] = "lemon"
        for key in ("x", "y"):
            value = float(note.get(key, .1))
            if not math.isfinite(value):
                raise ValueError("便签位置无效")
            note[key] = max(0, min(1, value))
        wall_id = str(note.get("wallId") or "")
        if wall_id not in walls:
            requested = wall_id
            wall_id = missing.setdefault(requested, "wall-" + secrets.token_hex(8))
            walls.setdefault(wall_id, {"id": wall_id, "title": "心情墙", "createdAt": now})
        if counts.get(wall_id, 0) >= 30:
            requested = wall_id
            candidate = spillovers.get(requested)
            if not candidate or counts.get(candidate, 0) >= 30:
                candidate = "wall-" + secrets.token_hex(8)
                spillovers[requested] = candidate
            wall_id = candidate
            walls.setdefault(wall_id, {"id": wall_id, "title": "心情墙", "createdAt": now})
        note["wallId"] = wall_id
        counts[wall_id] = counts.get(wall_id, 0) + 1
    state["moodWalls"] = [wall for key, wall in walls.items() if counts.get(key)]
