"""R06 exam records and atomic, explicitly confirmed result publication.

Parent integration (no HTTP/server changes are made here):
  validate_record('examSessions', record) -> validate_session(record)
  POST /api/workspace/exam-save    -> ExamService(store).save(body)
  POST /api/workspace/exam-preview -> ExamService(store).preview(body)
  POST /api/workspace/exam-confirm -> ExamService(store).confirm(body)
Use existing authenticated JSON routes, their size limit, and error handling.
save requires {session, base}; base is the entire previously read record or null.
preview requires {id, base}. confirm additionally requires confirmation=
'CONFIRM_EXAM_RESULTS', createScore:bool, mistakeItemIds:[], reviewItemIds:[].
save/confirm return the ordinary Storage snapshot, consumed by PrepLocal.accept.
The store must provide snapshot(), apply(operations), and a reentrant changed lock.
Exam reviewAttempts use kind='exam', phase='initial' or 'retest'. Exclude initial
attempts from repeated-error counts. Existing mistakes are never overwritten.
"""

import copy
import hashlib
import json
import math
import re
from datetime import date, datetime, timezone

SAFE_ID = re.compile(r'^[A-Za-z0-9_.-]{1,100}$')
CAUSES = {'知识不清', '条件遗漏', '推理错误', '计算错误', '审题错误', '用时不足', '待核对'}
SOURCE_KEYS = ('sourceKind', 'sourceId', 'paperId', 'setId', 'sourceSnapshot', 'subjectSnapshot', 'files', 'createdAt', 'date')
ITEM_SOURCE_KEYS = ('id', 'sourceId', 'sourceCollection', 'sourceSnapshot', 'title', 'content', 'solution', 'anchors', 'files', 'knowledgeIds', 'questionType')


def identity(value):
    return isinstance(value, str) and bool(SAFE_ID.fullmatch(value))


def number(value, maximum=10000, positive=False):
    return type(value) in (int, float) and math.isfinite(value) and (value > 0 if positive else value >= 0) and value <= maximum


def _text(record, key, maximum, required=False):
    value = record.get(key, '')
    if not isinstance(value, str) or len(value) > maximum or (required and not value.strip()):
        raise ValueError('考试文字无效或超限: ' + key)


def _files(values):
    if not isinstance(values, list) or len(values) > 1000:
        raise ValueError('考试原件引用无效')
    for value in values:
        if not isinstance(value, dict) or not identity(value.get('id')):
            raise ValueError('考试原件编号无效')


def validate_session(record):
    if not isinstance(record, dict) or not identity(record.get('id')) or record.get('schemaVersion') != 1:
        raise ValueError('考试会话编号或版本无效')
    try:
        if len(json.dumps(record, ensure_ascii=False, allow_nan=False).encode('utf-8')) > 8 * 1024 * 1024:
            raise ValueError('考试会话超过 8 MB，请减少题量或文字')
    except (TypeError, OverflowError) as error:
        raise ValueError('考试会话必须是有效 JSON') from error
    _text(record, 'title', 300, True)
    for key in ('createdAt', 'updatedAt', 'submittedAt'):
        _text(record, key, 80)
    if record.get('date'):
        if not isinstance(record['date'], str) or not re.fullmatch(r'\d{4}-\d{2}-\d{2}', record['date']):
            raise ValueError('考试日期无效')
        date.fromisoformat(record['date'])
    if record.get('sourceKind') not in {'papers', 'questionSets'} or not identity(record.get('sourceId')) or not isinstance(record.get('sourceSnapshot'), dict):
        raise ValueError('请选择真实试卷或题组作为题源')
    for key in ('paperId', 'setId'):
        if record.get(key) and not identity(record[key]):
            raise ValueError('关联编号无效')
    if record.get('status') not in {'paused', 'running', 'submitted', 'confirmed'}:
        raise ValueError('考试状态无效')
    subjects = record.get('subjectSnapshot')
    if not isinstance(subjects, list) or not 1 <= len(subjects) <= 40:
        raise ValueError('考试需要科目与满分快照')
    known = {}
    for subject in subjects:
        if not isinstance(subject, dict) or not identity(subject.get('id')) or subject['id'] in known:
            raise ValueError('考试科目编号无效或重复')
        _text(subject, 'name', 100, True)
        if not number(subject.get('maximum'), positive=True) or subject['maximum'] < 1:
            raise ValueError('科目满分无效')
        if subject.get('legacyKey') not in (None, '', 'math', 'english', 'politics', 'major'):
            raise ValueError('旧成绩字段无效')
        if subject.get('target') is not None and not number(subject['target'], subject['maximum']):
            raise ValueError('科目目标无效')
        known[subject['id']] = subject
    items = record.get('items')
    if not isinstance(items, list) or not 1 <= len(items) <= 100:
        raise ValueError('整卷需要 1 至 100 道题')
    if type(record.get('position')) is not int or not 0 <= record['position'] < len(items):
        raise ValueError('当前题目位置无效')
    ids, maxima = set(), {key: 0 for key in known}
    for item in items:
        if not isinstance(item, dict) or not identity(item.get('id')) or item['id'] in ids or not identity(item.get('sourceId')):
            raise ValueError('逐题编号无效或重复')
        ids.add(item['id'])
        if item.get('subjectId') not in known or not isinstance(item.get('sourceSnapshot'), dict):
            raise ValueError('题目科目或题源快照缺失')
        if item.get('sourceCollection') not in {'papers', 'questionSets', 'mistakes', 'notes', 'resources'}:
            raise ValueError('题目来源类型无效')
        if item.get('status') not in {'unanswered', 'answering', 'answered', 'flagged', 'skipped'}:
            raise ValueError('逐题作答状态无效')
        for key, limit in (('title', 300), ('content', 200000), ('solution', 200000), ('answer', 200000), ('wrongThought', 20000), ('correctionReason', 20000), ('nextAction', 20000), ('questionType', 100)):
            _text(item, key, limit)
        maximum, awarded = item.get('maximum'), item.get('awarded')
        if maximum is not None and not number(maximum, known[item['subjectId']]['maximum'], positive=True):
            raise ValueError('逐题满分无效')
        if awarded is not None and (maximum is None or not number(awarded, maximum)):
            raise ValueError('得分需在零分至本题满分之间，空白表示未核分')
        maxima[item['subjectId']] += maximum or 0
        if not number(item.get('elapsedSeconds'), 10 * 365 * 86400):
            raise ValueError('逐题用时无效')
        if type(item.get('revealed', False)) is not bool:
            raise ValueError('答案揭示状态无效')
        causes = item.get('causeKinds', [])
        if not isinstance(causes, list) or any(not isinstance(cause, str) or cause not in CAUSES for cause in causes) or len(causes) != len(set(causes)):
            raise ValueError('逐题错因无效')
        if not isinstance(item.get('anchors', []), list) or not isinstance(item.get('knowledgeIds', []), list):
            raise ValueError('题目引用无效')
        if len(item.get('anchors', [])) > 1000 or len(item.get('knowledgeIds', [])) > 500 or any(not identity(key) for key in item.get('knowledgeIds', [])):
            raise ValueError('题目引用过多或编号无效')
        from workspace_service import validate_record
        for anchor in item.get('anchors', []):
            if not isinstance(anchor, dict) or not identity(anchor.get('id')):
                raise ValueError('题目区域引用无效')
            validate_record('sourceAnchors', anchor)
        _files(item.get('files', []))
    if any(maxima[key] > known[key]['maximum'] + .005 for key in known):
        raise ValueError('逐题满分合计超过本次科目满分，请核对')
    _files(record.get('files', []))
    receipt = record.get('finalization')
    if receipt is not None and not isinstance(receipt, dict):
        raise ValueError('确认记录无效')


def summarize(record):
    """Null remains missing; a subject is complete only with full paper coverage."""
    entries = []
    for subject in record['subjectSnapshot']:
        items = [item for item in record['items'] if item['subjectId'] == subject['id']]
        graded = [item for item in items if item.get('awarded') is not None]
        coverage = round(sum(item.get('maximum') or 0 for item in items), 6)
        subtotal = round(sum(item['awarded'] for item in graded), 6)
        complete = bool(items) and len(graded) == len(items) and all(item.get('maximum') is not None for item in items) and abs(coverage - subject['maximum']) < .005
        entries.append({**copy.deepcopy(subject), 'questions': len(items), 'graded': len(graded), 'coverage': coverage,
                        'subtotal': subtotal, 'value': subtotal if complete else None, 'complete': complete})
    return {'entries': entries, 'complete': all(entry['complete'] for entry in entries),
            'graded': sum(entry['graded'] for entry in entries), 'questions': len(record['items']),
            'subtotal': round(sum(entry['subtotal'] for entry in entries), 6),
            'maximum': sum(entry['maximum'] for entry in entries),
            'total': round(sum(entry['subtotal'] for entry in entries), 6) if all(entry['complete'] for entry in entries) else None}


def score_proposal(record):
    summary = summarize(record)
    return {'id': _result_id('score', record['id']), 'name': record['title'],
            'date': record.get('date') or (record.get('submittedAt') or record['createdAt'])[:10], 'paperId': record.get('paperId', ''),
            'examSessionId': record['id'], 'setId': record.get('setId', ''),
            'subjectSnapshot': copy.deepcopy(record['subjectSnapshot']),
            'subjectScores': {entry['id']: entry['value'] for entry in summary['entries']},
            'examCoverage': summary, 'comment': '整卷逐题人工核分；未核完或题目满分未覆盖本科满分时不生成该科总分。'}


def _result_id(kind, session_id, item_id=''):
    return 'exam-' + kind + '-' + hashlib.sha256((session_id + '\0' + item_id).encode()).hexdigest()[:32]


class ExamService:
    def __init__(self, store):
        self.store = store

    def _get(self, state, identity_):
        record = next((item for item in state.get('examSessions', []) if item['id'] == identity_), None)
        if record is None:
            raise ValueError('考试记录不存在，请保留本机草稿')
        return record

    def save(self, body):
        if not isinstance(body, dict) or 'base' not in body:
            raise ValueError('保存考试需要完整基线')
        value = copy.deepcopy(body.get('session'))
        validate_session(value)
        from workspace_service import upsert
        with self.store.changed:
            snapshot = self.store.snapshot()
            state = snapshot['state']
            old = next((item for item in state.get('examSessions', []) if item['id'] == value['id']), None)
            if old == value:
                return snapshot
            if old != body['base']:
                raise ValueError('考试已在其他设备变化；本机草稿未覆盖远端，请导出并比较后重新打开')
            if old is None:
                source = next((item for item in state.get(value['sourceKind'], []) if item['id'] == value['sourceId']), None)
                if source is None or source != value['sourceSnapshot']:
                    raise ValueError('题源已变化或删除，请重新预览；本机草稿仍保留')
                if value.get('finalization') is not None:
                    raise ValueError('新会话不能携带已确认结果')
            else:
                if any(old.get(key) != value.get(key) for key in SOURCE_KEYS) or old.get('finalization') != value.get('finalization'):
                    raise ValueError('原件、科目和已确认结果快照不能被静默替换')
                if len(old['items']) != len(value['items']) or any(any(a.get(key) != b.get(key) for key in ITEM_SOURCE_KEYS) for a, b in zip(old['items'], value['items'])):
                    raise ValueError('已开始考试的题源快照不能替换；请另建会话')
            return self.store.apply([upsert('examSessions', value)])

    def preview(self, body):
        with self.store.changed:
            record = self._get(self.store.snapshot()['state'], body.get('id'))
            if 'base' not in body or record != body['base']:
                raise ValueError('考试进度已变化，请重新核分预览')
            validate_session(record)
            return {'summary': summarize(record), 'scoreProposal': score_proposal(record),
                    'lostItemIds': [item['id'] for item in record['items'] if item.get('awarded') is not None and item.get('maximum') is not None and item['awarded'] < item['maximum']]}

    def confirm(self, body):
        from workspace_service import upsert
        if not isinstance(body, dict) or body.get('confirmation') != 'CONFIRM_EXAM_RESULTS' or type(body.get('createScore')) is not bool:
            raise ValueError('需要明确确认成绩、错题与复习反馈')
        selections = {}
        for key in ('mistakeItemIds', 'reviewItemIds'):
            values = body.get(key)
            if not isinstance(values, list) or len(values) > 100 or any(not identity(value) for value in values) or len(values) != len(set(values)):
                raise ValueError('确认题目选择无效')
            selections[key] = sorted(values)
        signature = hashlib.sha256(json.dumps({**selections, 'createScore': body['createScore']}, sort_keys=True).encode()).hexdigest()
        with self.store.changed:
            snapshot = self.store.snapshot()
            state = snapshot['state']
            record = self._get(state, body.get('id'))
            if record.get('finalization'):
                if record['finalization'].get('signature') == signature:
                    return snapshot
                raise ValueError('本次已确认入库，后续修订请编辑关联成绩或错题；未重复新增')
            if 'base' not in body or record != body['base']:
                raise ValueError('考试已在其他设备变化，请保留草稿并重新核对')
            validate_session(record)
            if record['status'] != 'submitted' or not record.get('submittedAt'):
                raise ValueError('请先明确交卷并预览核分')
            items = {item['id']: item for item in record['items']}
            if any(item_id not in items for values in selections.values() for item_id in values):
                raise ValueError('确认中含不属于本卷的题目')
            timestamp = datetime.now(timezone.utc).isoformat(timespec='seconds')
            operations, mistake_links, review_ids = [], {}, []
            for item_id in selections['mistakeItemIds']:
                item = items[item_id]
                if item.get('awarded') is None or item.get('maximum') is None or item['awarded'] >= item['maximum'] or not item.get('causeKinds'):
                    raise ValueError('只可确认已人工核分且已选错因的失分题')
                existing = next((entry for entry in state.get('mistakes', []) if entry['id'] == item['sourceId']), None) if item['sourceCollection'] == 'mistakes' else None
                if existing:
                    mistake_links[item_id] = existing['id']
                    continue
                subject = next(entry for entry in record['subjectSnapshot'] if entry['id'] == item['subjectId'])
                mistake_id = _result_id('mistake', record['id'], item_id)
                mistake = {'id': mistake_id, 'title': item['title'], 'content': item.get('content', ''), 'solution': item.get('solution', ''),
                           'path': subject['name'] + ' > 整卷模拟', 'date': record.get('date') or timestamp[:10], 'firstErrorAt': record.get('date') or timestamp[:10],
                           'subjectId': item['subjectId'], 'sourcePaperId': record.get('paperId') or item['sourceSnapshot'].get('sourcePaperId', ''), 'examSessionId': record['id'],
                           'examItemId': item_id, 'sourceQuestionId': item['sourceId'], 'setId': record.get('setId', ''),
                           'sourceSnapshot': copy.deepcopy(item['sourceSnapshot']), 'sourceAnchorsSnapshot': copy.deepcopy(item.get('anchors', [])),
                           'anchorIds': [anchor['id'] for anchor in item.get('anchors', []) if any(current == anchor for current in state.get('sourceAnchors', []))],
                           'files': copy.deepcopy(item.get('files', [])), 'knowledgeIds': copy.deepcopy(item.get('knowledgeIds', [])),
                           'questionType': item.get('questionType', ''), 'causeKinds': copy.deepcopy(item['causeKinds']),
                           'wrongThought': item.get('wrongThought', ''), 'correctionReason': item.get('correctionReason', ''),
                           'nextAction': item.get('nextAction', ''), 'examAnswer': item.get('answer', ''),
                           'examAwarded': item['awarded'], 'examMaximum': item['maximum'], 'examElapsedSeconds': item['elapsedSeconds']}
                operations.append(upsert('mistakes', mistake))
                mistake_links[item_id] = mistake_id
            for item_id in selections['reviewItemIds']:
                item = items[item_id]
                mistake_id = mistake_links.get(item_id)
                if not mistake_id and item['sourceCollection'] == 'mistakes' and any(entry['id'] == item['sourceId'] for entry in state.get('mistakes', [])):
                    mistake_id = item['sourceId']
                if not mistake_id or item.get('awarded') is None or item.get('maximum') is None:
                    raise ValueError('复习反馈需关联已有或本次确认的错题，且已人工核分')
                if item['awarded'] < item['maximum'] and not item.get('causeKinds'):
                    raise ValueError('失分复习反馈需要错因')
                review_id = _result_id('review', record['id'], item_id)
                phase = 'retest' if any(entry['id'] == mistake_id for entry in state.get('mistakes', [])) else 'initial'
                attempt = {'id': review_id, 'mistakeId': mistake_id, 'sourcePaperId': record.get('paperId') or item['sourceSnapshot'].get('sourcePaperId', ''),
                           'examSessionId': record['id'], 'examItemId': item_id, 'setId': record.get('setId', ''), 'kind': 'exam',
                           'time': timestamp, 'phase': phase, 'correct': item['awarded'] == item['maximum'], 'answer': item.get('answer', ''),
                           'awarded': item['awarded'], 'maximum': item['maximum'], 'elapsedSeconds': item['elapsedSeconds'],
                           'causeKinds': copy.deepcopy(item.get('causeKinds', [])), 'knowledgeIds': copy.deepcopy(item.get('knowledgeIds', [])),
                           'wrongThought': item.get('wrongThought', ''), 'correctionReason': item.get('correctionReason', ''),
                           'nextAction': item.get('nextAction', ''), 'solutionSnapshot': item.get('solution', '')}
                operations.append(upsert('reviewAttempts', attempt))
                review_ids.append(review_id)
            score_id = ''
            if body['createScore']:
                linked = [entry for entry in state.get('scores', []) if entry.get('examSessionId') == record['id']]
                if len(linked) > 1:
                    raise ValueError('本卷已有多个关联成绩，请先核对，未重复新增')
                if linked:
                    score_id = linked[0]['id']
                else:
                    proposal = score_proposal(record)
                    if not any(value is not None for value in proposal['subjectScores'].values()):
                        raise ValueError('尚无满分覆盖完整且全部核分的科目，不能生成虚假的科目总分')
                    proposal['updatedAt'] = timestamp
                    score_id = proposal['id']
                    operations.append(upsert('scores', proposal))
            if not operations and not score_id:
                raise ValueError('请至少选择一项实际入库结果；也可返回继续作答')
            for operation in operations:
                if any(entry['id'] == operation['id'] for entry in state.get(operation['collection'], [])):
                    raise ValueError('结果编号已存在，未覆盖现有记录，请检查关联记录')
            value = copy.deepcopy(record)
            value['status'], value['updatedAt'] = 'confirmed', timestamp
            value['finalization'] = {'confirmedAt': timestamp, 'signature': signature, 'scoreId': score_id,
                                     'mistakeLinks': mistake_links, 'reviewIds': review_ids,
                                     'summarySnapshot': summarize(record)}
            operations.append(upsert('examSessions', value))
            return self.store.apply(operations)
