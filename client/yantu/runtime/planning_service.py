"""P02/P03 integration service. All writes use Storage.apply and its history.

Integration contract (authenticated routes, same pairing/JSON limits as /api/sync):
  POST /api/planning/preview -> plan_preview(body)
  POST /api/planning/apply   -> plan_apply(body)
  POST /api/planning/focus   -> focus(body)
Advertise 'planning-v1', 'planning-preview', 'focus-lease-v1' in workspace features.
Construct once as PlanningService(storage). No worker, new DB or data path is used.
Call validate_planning_record(collection, record) from shared record validation.
Reject browser /api/sync writes to setting planningFocusLease and to records whose
source is planning-focus (including changing/deleting an existing such record).
These are server-owned, not an offline timer. Exclude the live lease from rescue
restore; rescue the planning-focus-draft- requests for explicit retry only.
Rescue setting: planCapacities. Draft prefixes: planning-draft-, planning-focus-draft-.
"""

import copy
import hashlib
import json
import math
import re
import secrets
import time
from datetime import date, timedelta

from workspace_service import upsert
from activity_classification import ACTIVITY_TYPES, TASK_STATUSES, activity_type, task_is_active, is_obsidian_record

SAFE_ID = re.compile(r'^[A-Za-z0-9_.-]{1,100}$')
LEASE_KEY = 'planningFocusLease'
LEASE_MS = 35000
FEATURES = ['planning-v1', 'planning-preview', 'focus-lease-v1']


def identity(value):
    if not isinstance(value, str) or not SAFE_ID.fullmatch(value):
        raise ValueError('记录编号无效')
    return value


def number(value, lower, upper):
    if type(value) not in (float, int) or not math.isfinite(value) or not lower <= value <= upper:
        raise ValueError('数值超出范围')
    return value


def day(value):
    if not isinstance(value, str) or not re.fullmatch(r'\d{4}-\d{2}-\d{2}', value):
        raise ValueError('日期无效')
    return date.fromisoformat(value)


def validate_capacities(value):
    if not isinstance(value, dict) or not isinstance(value.get('dailyMinutes'), list) or len(value['dailyMinutes']) != 7:
        raise ValueError('请设置周一至周日的可用分钟数')
    for minutes in value['dailyMinutes']:
        number(minutes, 0, 1440)
    number(value.get('bufferMinutes'), 0, 1440)


def validate_planning_record(collection, record):
    """Optional shared validator hook; never rewrites old scores or timers."""
    if 'activityType' in record and record['activityType'] not in ACTIVITY_TYPES:
        raise ValueError('活动类别无效')
    if (collection == 'tasks' and 'taskStatus' in record and record['taskStatus'] not in TASK_STATUSES
            and not (record['taskStatus'] == 'unknown' and record.get('bridgeReadonly') and is_obsidian_record(record))):
        raise ValueError('任务状态无效')
    if collection == 'subjects':
        if not isinstance(record.get('name'), str) or not record['name'].strip() or len(record['name']) > 100:
            raise ValueError('科目名称无效')
        number(record.get('maximum'), 1, 10000)
        if record.get('target') is not None:
            number(record['target'], 0, record['maximum'])
        if record.get('legacyKey') not in (None, '', 'math', 'english', 'politics', 'major'):
            raise ValueError('旧成绩字段无效')
    if collection == 'chapters':
        identity(record.get('subjectId'))
        if not isinstance(record.get('title'), str) or not record['title'].strip() or len(record['title']) > 300:
            raise ValueError('章节标题无效')
        evidence = record.get('evidence', [])
        if not isinstance(evidence, list) or len(evidence) > 1000:
            raise ValueError('章节证据无效')
        for item in evidence:
            if not isinstance(item, dict) or item.get('kind') not in ('read', 'answer', 'retest') or not isinstance(item.get('note', ''), str):
                raise ValueError('章节证据类型无效')
            identity(item.get('id'))
            if not item.get('note', '').strip() and not item.get('attemptId'):
                raise ValueError('请保留实际阅读或作答证据')
            if item['kind'] == 'retest':
                identity(item.get('attemptId'))
    if collection == 'timeBlocks':
        day(record.get('date'))
        number(record.get('durationMinutes'), 1, 1440)
        if record.get('taskId'):
            identity(record['taskId'])
        if record.get('startTime'):
            if not re.fullmatch(r'([01]\d|2[0-3]):[0-5]\d', record['startTime']):
                raise ValueError('开始时间无效')
            hour, minute = map(int, record['startTime'].split(':'))
            if hour * 60 + minute + record['durationMinutes'] > 1440:
                raise ValueError('时间块不能跨过当天午夜，请拆分')
    if collection == 'tasks':
        if 'estimatedMinutes' in record and record['estimatedMinutes'] is not None:
            number(record['estimatedMinutes'], 1, 100000)
        for key in ('startDate', 'deadline'):
            if record.get(key):
                day(record[key])
        if record.get('startDate') and record.get('deadline') and record['startDate'] > record['deadline']:
            raise ValueError('计划开始日不能晚于最终截止日')
    if collection == 'scores' and 'subjectSnapshot' in record:
        subjects, values = record['subjectSnapshot'], record.get('subjectScores')
        if not isinstance(subjects, list) or not 1 <= len(subjects) <= 40 or not isinstance(values, dict):
            raise ValueError('成绩需要保留当时科目和满分')
        ids = set()
        for subject in subjects:
            if not isinstance(subject, dict):
                raise ValueError('成绩科目快照无效')
            key = identity(subject.get('id'))
            if key in ids:
                raise ValueError('成绩科目重复')
            ids.add(key)
            validate_planning_record('subjects', subject)
            if values.get(key) is not None:
                number(values[key], 0, subject['maximum'])
        if set(values) - ids:
            raise ValueError('成绩包含快照之外的科目')


def setting(key, value, op_id=None):
    return {'opId': op_id or secrets.token_hex(16), 'kind': 'setting', 'key': key, 'value': value}


class PlanningService:
    def __init__(self, store, clock=None):
        self.store = store
        self.clock = clock or time.time

    def plan_preview(self, body):
        snapshot = self.store.snapshot()
        state = snapshot['state']
        start = day(body.get('start'))
        order = body.get('order')
        if not isinstance(order, list) or not 1 <= len(order) <= 200 or len(set(map(str, order))) != len(order):
            raise ValueError('请选择 1 至 200 个不重复的任务')
        for key in order:
            identity(key)
        if type(body.get('split')) is not bool:
            raise ValueError('请明确是否允许拆分时间块')
        capacities = state.get('planCapacities')
        validate_capacities(capacities)
        tasks = {item['id']: item for item in state.get('tasks', [])}
        if any(key not in tasks or not task_is_active(tasks[key]) or tasks[key].get('bridgeReadonly') for key in order):
            raise ValueError('任务已删除、完成、取消或只读，请重新选择')
        dates = [(start + timedelta(days=i)).isoformat() for i in range(7)]
        selected = set(order)
        old_blocks = [item for item in state.get('timeBlocks', []) if item.get('taskId') in selected and item.get('date') in dates]
        removed = {item['id'] for item in old_blocks}
        remaining = [item for item in state.get('timeBlocks', []) if item['id'] not in removed]
        used = {stamp: sum(item.get('durationMinutes', 0) for item in remaining if item.get('date') == stamp) for stamp in dates}
        budget = {stamp: max(0, capacities['dailyMinutes'][day(stamp).weekday()] - capacities['bufferMinutes']) for stamp in dates}
        blocks, unscheduled, changes = [], [], []
        for key in order:
            task = tasks[key]
            validate_planning_record('tasks', task)
            estimate = task.get('estimatedMinutes')
            if estimate is None:
                unscheduled.append({'id': key, 'title': task.get('title', key), 'minutes': None, 'reason': '尚未估时'})
                continue
            planned_elsewhere = sum(item.get('durationMinutes', 0) for item in remaining if item.get('taskId') == key and item.get('date', '') >= dates[0])
            minutes = max(0, estimate - planned_elsewhere)
            earliest = task.get('startDate') or task.get('date') or dates[0]
            deadline = task.get('deadline')
            task_blocks = []
            for stamp in dates:
                if minutes <= 0:
                    break
                if stamp < earliest or deadline and stamp > deadline:
                    continue
                free = max(0, budget[stamp] - used[stamp])
                allocation = min(minutes, free) if body['split'] else (minutes if minutes <= free else 0)
                if allocation < 1:
                    continue
                block = {'taskId': key, 'title': task.get('title', key), 'subjectId': task.get('subjectId', ''),
                         'date': stamp, 'durationMinutes': allocation, 'startTime': '', 'source': 'planning-preview',
                         'activityType': activity_type(task)}
                if is_obsidian_record(task):
                    block['obsidianExport'] = True
                task_blocks.append(block)
                blocks.append(block)
                used[stamp] += allocation
                minutes -= allocation
            if minutes:
                unscheduled.append({'id': key, 'title': task.get('title', key), 'minutes': minutes,
                                    'reason': '截止日或可用容量不足；可拆分、延后截止日或减少范围后重新预览'})
            if task_blocks or any(item.get('taskId') == key for item in old_blocks):
                changes.append({'id': key, 'title': task.get('title', key), 'beforeDate': task.get('date', ''),
                                'afterDate': task_blocks[0]['date'] if task_blocks else task.get('date', ''),
                                'before': [item for item in old_blocks if item.get('taskId') == key], 'after': task_blocks})
        preview = {'revision': snapshot['revision'], 'request': {'start': dates[0], 'order': order, 'split': body['split']},
                   'removeIds': sorted(removed), 'blocks': blocks, 'changes': changes, 'unscheduled': unscheduled,
                   'days': [{'date': stamp, 'used': used[stamp], 'budget': budget[stamp],
                             'buffer': capacities['bufferMinutes']} for stamp in dates]}
        preview['digest'] = hashlib.sha256(json.dumps(preview, sort_keys=True, ensure_ascii=False).encode()).hexdigest()
        return preview

    def plan_apply(self, body):
        with self.store.changed:
            if body.get('confirmation') != 'APPLY_PLAN':
                raise ValueError('请先查看并确认重排预览')
            preview = self.plan_preview(body.get('request', {}))
            if body.get('revision') != preview['revision'] or body.get('digest') != preview['digest']:
                raise ValueError('任务或容量已变化，请重新预览')
            stamp = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(self.clock()))
            plan_id = secrets.token_hex(12)
            operations = [{'opId': secrets.token_hex(16), 'kind': 'delete', 'collection': 'timeBlocks', 'id': key} for key in preview['removeIds']]
            operations.extend(upsert('timeBlocks', {**item, 'id': secrets.token_hex(16), 'planId': plan_id}) for item in preview['blocks'])
            tasks = {item['id']: item for item in self.store.snapshot()['state'].get('tasks', [])}
            for change in preview['changes']:
                task = tasks[change['id']]
                history = [*task.get('planChanges', []), {'id': plan_id, 'at': stamp, 'kind': 'reorder',
                           'beforeDate': change['beforeDate'], 'afterDate': change['afterDate'],
                           'beforeMinutes': sum(item['durationMinutes'] for item in change['before']),
                           'afterMinutes': sum(item['durationMinutes'] for item in change['after'])}]
                operations.append(upsert('tasks', {'id': task['id'], 'date': change['afterDate'], 'planChanges': history}))
            return self.store.apply(operations)

    def _settle(self, lease, now, operations, reason=''):
        if lease['status'] != 'running':
            return
        # A lost lease proves no activity after the last acknowledged checkpoint.
        end = lease['checkpointAt'] if now > lease['leaseUntil'] else max(lease['checkpointAt'], now)
        if end > lease['checkpointAt']:
            lease['sequence'] += 1
            segment = {'id': f"{lease['id']}.{lease['epoch']}.{lease['sequence']}", 'source': 'planning-focus',
                       'sessionId': lease['id'], 'ownerId': lease['ownerId'], 'subject': lease.get('subject', '未分类'),
                       'subjectId': lease.get('subjectId', ''), 'sourceId': lease['sourceId'], 'sourceCollection': lease['sourceCollection'],
                       'title': lease['title'], 'startedAt': self._iso(lease['checkpointAt']), 'endedAt': self._iso(end),
                       'seconds': (end - lease['checkpointAt']) / 1000, 'interruptionReason': reason,
                       'activityType': activity_type(lease)}
            if is_obsidian_record(lease):
                segment['obsidianExport'] = True
            operations.append(upsert('focusSessions', segment))
            lease['creditedSeconds'] += segment['seconds']
        lease['checkpointAt'] = end

    @staticmethod
    def _iso(milliseconds):
        from datetime import datetime, timezone
        return datetime.fromtimestamp(milliseconds / 1000, timezone.utc).isoformat(timespec='milliseconds')

    def status(self, owner):
        lease = self.store.snapshot()['state'].get(LEASE_KEY)
        visible = copy.deepcopy(lease)
        if visible:
            visible.pop('ownerId', None)
            visible.pop('ownerLabel', None)
        return {'lease': visible, 'mine': bool(lease and lease.get('ownerId') == owner), 'time': int(self.clock() * 1000)}

    def validate_client_operations(self, operations):
        if not isinstance(operations, list):
            return
        state = self.store.snapshot()['state']
        protected = {record['id'] for record in state.get('focusSessions', []) if record.get('source') == 'planning-focus'}
        for operation in operations:
            if not isinstance(operation, dict):
                raise ValueError('无效同步操作')
            if operation.get('kind') == 'setting' and operation.get('key') == LEASE_KEY:
                raise ValueError('关联专注的所有权必须通过专注操作确认')
            if (operation.get('kind') == 'setting' and operation.get('key') == 'focusTimer'
                    and isinstance(operation.get('value'), dict) and operation['value'].get('running')
                    and state.get(LEASE_KEY, {}).get('status') == 'running'):
                raise ValueError('关联专注仍在运行，请先暂停，不能重复计时')
            if operation.get('collection') == 'focusSessions' and (operation.get('id') in protected or operation.get('fields', {}).get('source') == 'planning-focus'):
                raise ValueError('已确认的关联专注时段不能直接改写，请保留原记录')

    def focus(self, body, owner_id=None):
        with self.store.changed:
            request_id = identity(body.get('requestId'))
            op_id = 'pf-' + request_id
            if len(op_id) > 100:
                raise ValueError('操作编号过长')
            if self.store.db.execute('SELECT 1 FROM operations WHERE id=?', (op_id,)).fetchone():
                return self.store.snapshot()
            owner = identity(owner_id or body.get('ownerId'))
            now = int(self.clock() * 1000)
            snapshot = self.store.snapshot()
            state = snapshot['state']
            current = state.get(LEASE_KEY)
            lease = copy.deepcopy(current)
            action = body.get('action')
            operations = []
            if state.get('prepStatus') in {'paused', 'exited'}:
                if action in {'start', 'resume'}:
                    raise ValueError('备考已暂停或退出，请先继续备考再开始关联专注')
                if lease and lease.get('status') == 'running':
                    lease['status'] = 'paused'
                    lease['checkpointAt'] = now
            if action in ('start', 'resume', 'takeover') and state.get('focusTimer', {}).get('running'):
                raise ValueError('旧专注计时仍在运行，请先暂停，历史记录会保留')
            if action == 'start':
                if lease and lease.get('status') != 'finished':
                    raise ValueError('已有专注，请继续原会话或明确接管')
                collection = body.get('sourceCollection')
                if collection not in ('tasks', 'mistakes', 'resources', 'notes', 'papers', 'chapters'):
                    raise ValueError('专注需要关联任务、章节或学习资料')
                source = next((item for item in state.get(collection, []) if item.get('id') == body.get('sourceId')), None)
                if not source:
                    raise ValueError('关联内容尚未同步或已删除')
                if collection == 'tasks' and not task_is_active(source):
                    raise ValueError('任务已完成或取消，请重新选择')
                subject_id = body.get('subjectId') or source.get('subjectId', '')
                default_names = {'subject-math': '数学', 'subject-english': '英语', 'subject-politics': '政治', 'subject-major': '408'}
                subject = next((item['name'] for item in state.get('subjects', []) if item['id'] == subject_id), default_names.get(subject_id) or source.get('subject') or '未分类')
                lease = {'id': secrets.token_hex(12), 'ownerId': owner, 'ownerLabel': '当前设备',
                         'epoch': secrets.token_hex(8), 'sequence': 0, 'status': 'running', 'sourceId': source['id'],
                         'sourceCollection': collection, 'subjectId': subject_id, 'subject': subject,
                         'activityType': activity_type(source),
                         'title': str(source.get('title') or source.get('name') or source['id'])[:300],
                         'startedAt': now, 'checkpointAt': now, 'leaseUntil': now + LEASE_MS, 'creditedSeconds': 0, 'interruptions': []}
                if is_obsidian_record(source):
                    lease['obsidianExport'] = True
            else:
                if not lease or lease.get('status') == 'finished':
                    raise ValueError('当前没有可继续的专注')
                if body.get('sessionId') != lease['id'] or body.get('epoch') != lease['epoch']:
                    raise ValueError('专注所有权已变化，请刷新后再操作')
                if action == 'takeover':
                    if body.get('confirmation') != 'TAKE_OVER_FOCUS':
                        raise ValueError('接管需要明确确认')
                    self._settle(lease, now, operations, '设备接管')
                    lease.update(ownerId=owner, ownerLabel='当前设备',
                                 epoch=secrets.token_hex(8), status='paused', checkpointAt=now, leaseUntil=now + LEASE_MS)
                    lease['interruptions'].append({'at': self._iso(now), 'kind': 'takeover', 'reason': '明确接管，等待手动继续'})
                else:
                    if owner != lease['ownerId'] or now > lease['leaseUntil']:
                        raise ValueError('专注租约已失效或属于另一设备，请明确接管')
                    if action not in ('heartbeat', 'pause', 'break', 'resume', 'finish'):
                        raise ValueError('专注操作无效')
                    reason = str(body.get('reason', '')).strip()[:1000]
                    if action in ('pause', 'break', 'finish') and not reason:
                        raise ValueError('请记录中断、休息或结束原因')
                    self._settle(lease, now, operations, reason)
                    if action != 'heartbeat':
                        lease['status'] = {'pause': 'paused', 'break': 'break', 'resume': 'running', 'finish': 'finished'}[action]
                        lease['checkpointAt'] = now
                        if reason:
                            lease['interruptions'].append({'at': self._iso(now), 'kind': action, 'reason': reason})
                    lease['leaseUntil'] = now + LEASE_MS
            lease['interruptions'] = lease['interruptions'][-100:]
            operations.append(setting(LEASE_KEY, lease, op_id))
            return self.store.apply(operations)
