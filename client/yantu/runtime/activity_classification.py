"""Activity/status reads and fail-closed Obsidian AI exclusion; no migrations."""

import math
from datetime import date, datetime, time, timedelta

ACTIVITY_TYPES = ('exam', 'skill', 'project', 'unclassified')
TASK_STATUSES = ('todo', 'in_progress', 'done', 'cancelled')
AI_SETTING_KEYS = frozenset((
    'prepStatus', 'prepStart', 'examYear', 'targetYear', 'examDate',
    'studySchedule', 'reminderSettings', 'lastTaskRefresh', 'subjectProfiles',
    'planCapacities', 'learningPrefs', 'cardPreferences', 'focusTimer',
    'planningFocusLease', 'focusSessions', 'timeBlocks',
))
LINK_KEYS = frozenset(('taskId', 'sourceId', 'originalId', 'reviewId', 'recordId'))


def activity_type(value):
    value = value.get('activityType') if isinstance(value, dict) else value
    return value if isinstance(value, str) and value in ACTIVITY_TYPES else 'unclassified'


def task_status(task):
    status = task.get('taskStatus')
    if status == 'unknown' and task.get('bridgeReadonly') and is_obsidian_record(task):
        return 'unknown'
    return status if isinstance(status, str) and status in TASK_STATUSES else ('done' if task.get('done') else 'todo')


def task_is_active(task):
    return task_status(task) in ('todo', 'in_progress')


def is_obsidian_record(record):
    return isinstance(record, dict) and (
        record.get('source') in ('obsidian', 'obsidianExport')
        or 'bridgeRef' in record or record.get('obsidianExport') is True
    )


def _records(state):
    for value in state.values():
        if isinstance(value, list):
            yield from (item for item in value if isinstance(item, dict))
        elif isinstance(value, dict):
            yield value


def _private(value, blocked):
    if isinstance(value, dict):
        return (is_obsidian_record(value)
                or isinstance(value.get('id'), str) and value['id'] in blocked
                or any(isinstance(value.get(key), str) and value[key] in blocked for key in LINK_KEYS)
                or any(_private(item, blocked) for item in value.values() if isinstance(item, (dict, list))))
    return isinstance(value, list) and any(_private(item, blocked) for item in value)


def private_record_ids(state):
    records, blocked = list(_records(state)), set()
    known = {item['id'] for item in records if isinstance(item.get('id'), str)}
    linked = [*state.get('focusSessions', []), *state.get('timeBlocks', []),
              state.get('planningFocusLease'), state.get('focusTimer')]
    for item in linked:
        if isinstance(item, dict):
            blocked.update(item[key] for key in ('sourceId', 'taskId')
                           if isinstance(item.get(key), str) and item[key] and item[key] not in known)
    # Close over derived tasks, time blocks, focus snapshots and nested sources.
    while True:
        found = {item['id'] for item in records if isinstance(item.get('id'), str) and _private(item, blocked)}
        if found <= blocked:
            return blocked
        blocked.update(found)


def private_attachment_ids(state):
    blocked = private_record_ids(state)
    files = set()
    def collect(value):
        if isinstance(value, dict):
            if isinstance(value.get('fileId'), str) and value['fileId']:
                files.add(value['fileId'])
            files.update(item['id'] for item in value.get('files', []) if isinstance(item, dict) and isinstance(item.get('id'), str))
            for child in value.values():
                if isinstance(child, (dict, list)):
                    collect(child)
        elif isinstance(value, list):
            for child in value:
                collect(child)
    for record in _records(state):
        if record.get('id') in blocked or _private(record, blocked):
            collect(record)
    return files


def ai_safe_state(state):
    """No prompt/tool argument can grant access. Unknown settings stay private."""
    blocked = private_record_ids(state)
    safe = {}
    for key, value in state.items():
        if key.lower().startswith(('obsidian', 'bridge')):
            continue
        if isinstance(value, list):
            safe[key] = [item for item in value if not _private(item, blocked) and not (isinstance(item, dict) and item.get('id') in blocked)]
        elif key in AI_SETTING_KEYS and not _private(value, blocked):
            safe[key] = value
    return safe


def ai_settings(state):
    return {key: value for key, value in ai_safe_state(state).items() if key in AI_SETTING_KEYS}


def _timestamp(value):
    try:
        if type(value) in (int, float):
            return value / 1000 if math.isfinite(value) else None
        if isinstance(value, str):
            return datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()
    except (ValueError, OverflowError, OSError):
        pass
    return None


def session_day_seconds(session, day):
    """Clip credited activity to its real interval, then to local midnight."""
    if isinstance(day, str):
        day = date.fromisoformat(day)
    start, end = _timestamp(session.get('startedAt')), _timestamp(session.get('endedAt'))
    seconds = session.get('seconds')
    if (start is None or end is None or end <= start or type(seconds) not in (int, float)
            or not math.isfinite(seconds) or seconds <= 0):
        return 0
    lower = datetime.combine(day, time.min).timestamp()
    upper = datetime.combine(day + timedelta(days=1), time.min).timestamp()
    # A shorter credited interval must not be counted once on each side of midnight.
    credited_end = min(end, start + seconds)
    return max(0, min(credited_end, upper) - max(start, lower))


def daily_summary(state, day):
    stamp = day.isoformat() if isinstance(day, date) else date.fromisoformat(day).isoformat()
    by_type = dict.fromkeys(ACTIVITY_TYPES, 0)
    seen, events = set(), set()
    for session in state.get('focusSessions', []):
        identity = session.get('id')
        event = session.get('sourceEventId')
        if not identity or identity in seen or event and event in events:
            continue
        seen.add(identity)
        if event:
            events.add(event)
        by_type[activity_type(session)] += session_day_seconds(session, stamp)
    tasks = {task['id']: task for task in state.get('tasks', []) if task.get('id')}
    return {'totalSeconds': sum(by_type.values()), 'examSeconds': by_type['exam'], 'byType': by_type,
            'completedTasks': sum(task_status(task) == 'done' and (task.get('completedDate') or task.get('date')) == stamp for task in tasks.values()),
            'pendingTasks': sum(task_is_active(task) and task.get('date') == stamp for task in tasks.values())}
