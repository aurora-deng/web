"""T01-T06 validation only. No network, execution, storage, or generated facts.

Parent integration: call validate_record(collection, merged_record) from the
shared write validator. validate_update additionally protects writing history
when the previous complete record is available.
"""

import datetime as dt
import json
import re
from urllib.parse import urlsplit

COLLECTIONS = frozenset({
    'vocabulary', 'readingAnalyses', 'writingProjects', 'studyTemplates',
    'memoryTopics', 'schoolArchives',
})
SAFE_ID = re.compile(r'^[A-Za-z0-9_.-]{1,100}$')
TEXT_FIELDS = {
    'vocabulary': ('word', 'sentence', 'meaning', 'example'),
    'readingAnalyses': ('passage', 'ownAnswer', 'structure', 'paragraphRoles'),
    'writingProjects': ('prompt', 'original', 'patterns', 'commonErrors'),
    'studyTemplates': ('question', 'conditions', 'steps', 'counterexamples', 'proof', 'complexity', 'formula', 'codeLanguage', 'code'),
    'memoryTopics': ('keywords', 'material', 'answerLayers', 'comparisons'),
    'schoolArchives': ('requirements', 'selfIntroduction', 'interviewQA', 'interviewMaterials'),
}
REQUIRED = {
    'vocabulary': ('word', 'sentence', 'meaning'), 'readingAnalyses': ('passage',),
    'writingProjects': ('original',), 'studyTemplates': ('question',),
    'memoryTopics': ('keywords',), 'schoolArchives': ('schoolId',),
}


def _text(value, name, maximum=100000, required=False):
    if not isinstance(value, str) or len(value) > maximum or (required and not value.strip()):
        raise ValueError(f'{name}文字无效、缺失或超限')


def _id(value):
    if not isinstance(value, str) or not SAFE_ID.fullmatch(value):
        raise ValueError('关联或条目编号无效')


def _date(value, name):
    if not isinstance(value, str) or not re.fullmatch(r'\d{4}-\d{2}-\d{2}', value):
        raise ValueError(f'{name}日期无效')
    try:
        dt.date.fromisoformat(value)
    except ValueError as exc:
        raise ValueError(f'{name}日期无效') from exc


def _timestamp(value):
    if not isinstance(value, str) or len(value) > 50:
        raise ValueError('时间无效')
    try:
        parsed = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
    except ValueError as exc:
        raise ValueError('时间无效') from exc
    if parsed.tzinfo is None:
        raise ValueError('时间需要时区')


def _url(value):
    _text(value, '来源链接', 2000)
    if not value:
        return
    try:
        parsed = urlsplit(value)
        valid = parsed.scheme in {'http', 'https'} and parsed.hostname and not parsed.username and not parsed.password
        parsed.port
    except ValueError as exc:
        raise ValueError('来源链接无效') from exc
    if not valid or any(ord(char) < 33 for char in value) or '\\' in value:
        raise ValueError('来源链接仅支持不含账号密码的 HTTP / HTTPS 地址')


def validate_record(collection, record):
    """Validate a complete merged record; unrelated collections are untouched."""
    if collection not in COLLECTIONS:
        return
    if not isinstance(record, dict):
        raise ValueError('学习记录必须为对象')
    _id(record.get('id'))
    _text(record.get('title'), '标题', 300, True)
    for name in TEXT_FIELDS[collection]:
        _text(record.get(name, ''), name, 300 if name == 'word' else 80 if name == 'codeLanguage' else 100000, name in REQUIRED[collection])
    for name, maximum in (('sourceTitle', 1000), ('sourcePosition', 1000)):
        _text(record.get(name, ''), name, maximum)
    _url(record.get('sourceUrl', ''))
    for name in ('sourceId', 'anchorId', 'schoolId', 'cardId'):
        if record.get(name) or name in REQUIRED[collection]:
            _id(record.get(name))
    for name, maximum in (('linkedIds', 200), ('knowledgeIds', 500)):
        values = record.get(name, [])
        if not isinstance(values, list) or len(values) > maximum:
            raise ValueError('关联数量无效')
        for value in values:
            _id(value)
        if len(values) != len(set(values)):
            raise ValueError('关联编号重复')
    for name in ('createdAt', 'updatedAt', 'attemptedAt', 'analysisViewedAt'):
        if record.get(name):
            _timestamp(record[name])
    if collection in {'memoryTopics', 'schoolArchives'}:
        year = record.get('applicableYear', '')
        if year != '' and (type(year) not in (str, int) or not re.fullmatch(r'(19\d{2}|20\d{2}|21\d{2}|2200)', str(year))):
            raise ValueError('适用年份应为 1900 至 2200 的整数')
        if record.get('checkedOn'):
            _date(record['checkedOn'], '核验')
        if not isinstance(record.get('currentness', 'unverified'), str) or record.get('currentness', 'unverified') not in {'unverified', 'checked', 'outdated'}:
            raise ValueError('核验状态无效')
    enums = {'writingProjects': {'writing', 'translation'}, 'studyTemplates': {'math', 'cs'}}
    if collection in enums and (not isinstance(record.get('kind'), str) or record.get('kind') not in enums[collection]):
        raise ValueError('类型无效')
    array_schema = {
        'readingAnalyses': {'options': (50, ('label', 'evidence', 'position', 'distractorReason'))},
        'writingProjects': {'versions': (200, ('text', 'manualComments', 'selfReview', 'modelSuggestions'))},
        'schoolArchives': {'checklist': (100, ('text',)), 'deadlines': (100, ('label', 'source'))},
    }
    for name, (maximum, fields) in array_schema.get(collection, {}).items():
        values = record.get(name, [])
        if not isinstance(values, list) or len(values) > maximum:
            raise ValueError('条目数量无效')
        seen = set()
        for item in values:
            if not isinstance(item, dict):
                raise ValueError('条目必须为对象')
            _id(item.get('id'))
            if item['id'] in seen:
                raise ValueError('条目编号重复')
            seen.add(item['id'])
            for field in fields:
                _text(item.get(field, ''), field, required=(name in {'versions', 'checklist'} and field == 'text') or (name == 'deadlines' and field == 'label'))
            if name == 'versions':
                _timestamp(item.get('createdAt'))
                if any(key in item for key in ('score', 'examScore', 'modelScore')):
                    raise ValueError('修订版本不保存模型或考试评分')
            elif name == 'checklist' and type(item.get('done')) is not bool:
                raise ValueError('材料勾选状态必须为布尔值')
            elif name == 'deadlines':
                _date(item.get('date'), '截止')
    try:
        serialized = json.dumps(record, ensure_ascii=False, allow_nan=False)
    except (ValueError, TypeError) as exc:
        raise ValueError('记录不是有效 JSON') from exc
    if len(serialized) > 2000000:
        raise ValueError('记录超过 200 万字符，请拆分保存')


def validate_update(collection, previous, record):
    """Optional stronger hook for the parent write path (no storage access)."""
    validate_record(collection, record)
    if collection == 'writingProjects' and previous:
        if previous.get('original') and record.get('original') != previous.get('original'):
            raise ValueError('已归档原稿不可覆盖，请追加修订版本')
        old_versions = previous.get('versions', [])
        if record.get('versions', [])[:len(old_versions)] != old_versions:
            raise ValueError('历史修订不可覆盖或移除')
