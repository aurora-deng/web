"""Shared learning records, stable references and explicit bulk-change previews."""

import copy
import hashlib
import json
import math
import re
import secrets
from pathlib import Path

COLLECTIONS = {
    'knowledgeTopics', 'knowledgeMerges', 'sourceAnchors', 'readingPositions', 'readingMarks',
    'captureInbox', 'savedViews', 'boards', 'flashcards', 'cardReviews', 'questionSets',
    'practiceDrafts', 'examSessions', 'subjects', 'chapters', 'timeBlocks', 'vocabulary',
    'readingAnalyses', 'writingProjects', 'studyTemplates', 'memoryTopics', 'schoolArchives',
    'assistantDrafts', 'schoolCatalog',
}
SAFE_ID = re.compile(r'^[A-Za-z0-9_.-]{1,100}$')


def normalized(text):
    return ' '.join(str(text).split()).casefold()


def search_text(record):
    keys = ('title', 'name', 'word', 'content', 'text', 'solution', 'knowledgePoints', 'aliases', 'front', 'back', 'sentence', 'translation', 'definition', 'evidence', 'sections', 'nodes', 'items', 'passage', 'ownAnswer', 'structure', 'paragraphRoles', 'options', 'original', 'versions', 'patterns', 'commonErrors', 'question', 'conditions', 'steps', 'counterexamples', 'proof', 'complexity', 'formula', 'code', 'keywords', 'material', 'answerLayers', 'comparisons', 'requirements', 'selfIntroduction', 'interviewQA', 'checklist')
    return '\n'.join(json.dumps(record[key], ensure_ascii=False) if isinstance(record.get(key), (dict, list)) else str(record.get(key, '')) for key in keys)


def index_schema(db):
    db.executescript('''CREATE TABLE IF NOT EXISTS workspace_text(collection TEXT,record_id TEXT,title TEXT,body TEXT,PRIMARY KEY(collection,record_id));
        CREATE TABLE IF NOT EXISTS workspace_index_meta(id INTEGER PRIMARY KEY,revision INTEGER);''')
    if 'raw' not in {row[1] for row in db.execute('PRAGMA table_info(workspace_text)')}:
        db.execute("ALTER TABLE workspace_text ADD COLUMN raw TEXT NOT NULL DEFAULT ''")
        db.execute('DELETE FROM workspace_index_meta')
        db.commit()


def index_record(db, collection, record):
    text = search_text(record)
    db.execute('INSERT OR REPLACE INTO workspace_text(collection,record_id,title,body,raw) VALUES(?,?,?,?,?)', (collection, record['id'], record.get('title') or record.get('name') or record.get('word') or record['id'], normalized(text), ' '.join(text.split())))


def upsert(collection, record):
    return {'opId': secrets.token_hex(16), 'kind': 'upsert', 'collection': collection,
            'id': record['id'], 'fields': {key: value for key, value in record.items() if key != 'id'}}


def validate_record(collection, record):
    from planning_service import validate_planning_record
    validate_planning_record(collection, record)
    from subject_tools import validate_record as validate_subject_record
    validate_subject_record(collection, record)
    if collection == 'examSessions':
        from exam_service import validate_session
        validate_session(record)
    if collection == 'assistantDrafts':
        from assistant_drafts import validate_receipt
        validate_receipt(record)
    if collection in {'questionSets', 'practiceDrafts'}:
        items = record.get('items')
        if not isinstance(items, list) or not 1 <= len(items) <= 100:
            raise ValueError('题组或练习需要 1 至 100 道题')
        ids = set()
        for item in items:
            if not isinstance(item, dict) or not isinstance(item.get('id'), str) or not SAFE_ID.fullmatch(item['id']) or item['id'] in ids:
                raise ValueError('练习题目编号无效或重复')
            ids.add(item['id'])
            if not isinstance(item.get('sourceId'), str) or not SAFE_ID.fullmatch(item['sourceId']):
                raise ValueError('练习题源编号无效')
            for key, limit in (('title', 300), ('content', 200000), ('solution', 200000), ('answer', 200000), ('wrongThought', 20000), ('correctionReason', 20000), ('nextAction', 20000)):
                if not isinstance(item.get(key, ''), str) or len(item.get(key, '')) > limit:
                    raise ValueError('练习文字超限或无效')
            hints = item.get('hints', [])
            if not isinstance(hints, list) or len(hints) > 50 or any(not isinstance(hint, str) or len(hint) > 20000 for hint in hints):
                raise ValueError('练习提示无效')
            seconds = item.get('elapsedSeconds', 0)
            if type(seconds) not in (int, float) or not math.isfinite(seconds) or not 0 <= seconds <= 30 * 86400:
                raise ValueError('练习用时无效')
            if 'correct' in item and type(item['correct']) is not bool:
                raise ValueError('练习反馈必须由使用者明确选择做对或做错')
        if collection == 'practiceDrafts':
            if record.get('status') not in {'active', 'paused', 'completed'} or type(record.get('position')) is not int or not 0 <= record['position'] < len(items):
                raise ValueError('练习状态或当前位置无效')
    if collection == 'boards':
        nodes, edges = record.get('nodes'), record.get('edges')
        if not isinstance(nodes, list) or not isinstance(edges, list) or len(nodes) > 500 or len(edges) > 1000:
            raise ValueError('白板最多 500 个节点和 1000 条连线')
        ids = set()
        for node in nodes:
            if not isinstance(node, dict) or not isinstance(node.get('id'), str) or not SAFE_ID.fullmatch(node['id']) or node['id'] in ids:
                raise ValueError('白板节点编号无效或重复')
            if not isinstance(node.get('label'), str) or len(node['label']) > 300 or not isinstance(node.get('text', ''), str) or len(node.get('text', '')) > 20000:
                raise ValueError('白板节点文字超限或无效')
            if any(type(node.get(key)) not in (int, float) or not math.isfinite(node[key]) or abs(node[key]) > 1000000 for key in ('x', 'y')):
                raise ValueError('白板节点位置无效')
            for key in ('recordId', 'anchorId'):
                if node.get(key) and (not isinstance(node[key], str) or not SAFE_ID.fullmatch(node[key])):
                    raise ValueError('白板资料关联编号无效')
            if node.get('url'):
                from urllib.parse import urlsplit
                value = node['url']
                if not isinstance(value, str) or len(value) > 2048 or any(ord(character) <= 32 for character in value):
                    raise ValueError('白板网页地址无效')
                address = urlsplit(value)
                if address.scheme not in {'http', 'https'} or not address.hostname or address.username or address.password:
                    raise ValueError('白板网页仅允许不含账号密码的 HTTP 或 HTTPS 地址')
            if 'collapsed' in node and type(node['collapsed']) is not bool:
                raise ValueError('白板节点折叠状态无效')
            ids.add(node['id'])
        edge_ids = set()
        for edge in edges:
            if not isinstance(edge, dict) or not isinstance(edge.get('id'), str) or not SAFE_ID.fullmatch(edge['id']) or edge['id'] in ids | edge_ids:
                raise ValueError('白板连线编号无效或重复')
            if not isinstance(edge.get('source'), str) or not isinstance(edge.get('target'), str) or edge['source'] not in ids or edge['target'] not in ids or edge['source'] == edge['target']:
                raise ValueError('白板连线指向无效')
            if not isinstance(edge.get('label', ''), str) or len(edge.get('label', '')) > 300:
                raise ValueError('白板连线名称超限')
            edge_ids.add(edge['id'])
    if collection == 'flashcards':
        kind = record.get('type', 'qa')
        if kind not in ('qa', 'cloze', 'occlusion'):
            raise ValueError('背诵卡类型无效')
        for key, maximum in (('title', 300), ('front', 200000), ('back', 200000)):
            value = record.get(key, '')
            if not isinstance(value, str) or len(value) > maximum:
                raise ValueError('背诵卡文字超限或格式无效')
        if kind == 'cloze' and not re.search(r'\{\{c\d+::[^{}]+\}\}', record.get('front', '')):
            raise ValueError('填空卡需要至少一个有效填空')
        if kind == 'occlusion':
            if not SAFE_ID.fullmatch(str(record.get('fileId', ''))):
                raise ValueError('遮挡卡缺少来源附件')
            if type(record.get('page')) is not int or not 1 <= record['page'] <= 10000:
                raise ValueError('遮挡卡页码无效')
            masks = record.get('masks')
            if not isinstance(masks, list) or not 1 <= len(masks) <= 50:
                raise ValueError('遮挡卡需要 1 至 50 个区域')
            for mask in masks:
                if not isinstance(mask, list) or len(mask) != 4 or any(type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 1 for value in mask):
                    raise ValueError('遮挡卡区域坐标无效')
                if mask[2] <= 0 or mask[3] <= 0 or mask[0] + mask[2] > 1.000001 or mask[1] + mask[3] > 1.000001:
                    raise ValueError('遮挡卡区域超出原页')
    if collection == 'readingMarks':
        from reading_export import validate
        validate(record)
    if collection == 'knowledgeTopics':
        if not isinstance(record.get('name'), str) or not 1 <= len(record['name'].strip()) <= 160:
            raise ValueError('知识点名称需要 1 至 160 字')
        aliases = record.get('aliases', [])
        if not isinstance(aliases, list) or len(aliases) > 100 or any(not isinstance(alias, str) or not 1 <= len(alias.strip()) <= 160 for alias in aliases):
            raise ValueError('知识点别名无效')
    if collection == 'sourceAnchors':
        if not SAFE_ID.fullmatch(str(record.get('fileId', ''))) or not SAFE_ID.fullmatch(str(record.get('sourceId', ''))):
            raise ValueError('来源引用缺少附件或原记录编号')
        if not isinstance(record.get('page'), int) or not 1 <= record['page'] <= 10000:
            raise ValueError('来源页码无效')
        rectangles = record.get('regions', [])
        if not isinstance(rectangles, list) or len(rectangles) > 50:
            raise ValueError('单条来源最多 50 个区域')
        for rectangle in rectangles:
            if not isinstance(rectangle, list) or len(rectangle) != 4 or not all(isinstance(x, (int, float)) and math.isfinite(x) and 0 <= x <= 1 for x in rectangle):
                raise ValueError('题目区域坐标无效')
            if rectangle[2] <= 0 or rectangle[3] <= 0 or rectangle[0] + rectangle[2] > 1.000001 or rectangle[1] + rectangle[3] > 1.000001:
                raise ValueError('题目区域超出原页')
        if record.get('fingerprint') and not re.fullmatch('[0-9a-f]{64}', record['fingerprint']):
            raise ValueError('来源版本校验值无效')
    ids = record.get('knowledgeIds', [])
    if not isinstance(ids, list) or len(ids) > 500 or any(not isinstance(item, str) or not SAFE_ID.fullmatch(item) for item in ids):
        raise ValueError('知识点关联编号无效')


def validate_topics(state):
    names = {}
    for topic in state.get('knowledgeTopics', []):
        if topic.get('mergedInto'):
            continue
        for value in [topic.get('name', ''), *topic.get('aliases', [])]:
            key = normalized(value)
            if key in names and names[key] != topic['id']:
                raise ValueError('知识点名称或别名已存在，请使用现有知识点或先预览合并')
            names[key] = topic['id']


class WorkspaceService:
    def __init__(self, store, data):
        self.store, self.data = store, Path(data)
        self._search_cache = {}

    def capabilities(self):
        return {'version': 1, 'collections': sorted(COLLECTIONS), 'features': ['knowledge-merge', 'source-anchors', 'cleanup-preview', 'search', 'restore-preview', 'planning-v1', 'planning-preview', 'focus-lease-v1', 'subject-tools-v1', 'exam-v1', 'assistant-drafts-v1'],
                'privacy': {'peerDetails': False, 'secretsInExport': False}}

    def fingerprint(self, file_id):
        info = self.store.file_info(file_id)
        from library_sources import resolve_file
        source = resolve_file(self.data, file_id)
        if not info or not source.is_file():
            raise ValueError('来源附件不存在或尚未同步')
        with source.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        return {**info, 'fingerprint': digest}

    def merge_preview(self, body):
        snapshot = self.store.snapshot()
        state = snapshot['state']
        source, target = body.get('source'), body.get('target')
        topics = {item['id']: item for item in state.get('knowledgeTopics', [])}
        if source == target or source not in topics or target not in topics or topics[source].get('mergedInto') or topics[target].get('mergedInto'):
            raise ValueError('请选择两个不同的有效知识点')
        changes = []
        for collection, records in state.items():
            if not isinstance(records, list) or collection == 'knowledgeMerges':
                continue
            for record in records:
                if isinstance(record, dict) and source in record.get('knowledgeIds', []):
                    changes.append({'collection': collection, 'id': record['id'], 'title': record.get('title') or record.get('name') or record['id'],
                                    'before': record['knowledgeIds'], 'after': list(dict.fromkeys(target if key == source else key for key in record['knowledgeIds']))})
        return {'revision': snapshot['revision'], 'source': topics[source], 'target': topics[target], 'changes': changes,
                'aliases': list(dict.fromkeys([*topics[target].get('aliases', []), topics[source]['name'], *topics[source].get('aliases', [])]))}

    def merge(self, body):
        with self.store.changed:
            preview = self.merge_preview(body)
            if body.get('revision') != preview['revision'] or body.get('confirmation') != 'MERGE_TOPICS':
                raise ValueError('资料已变化或未确认，请重新预览合并')
            source, target = preview['source'], preview['target']
            log = {'id': secrets.token_hex(16), 'title': source['name'] + ' → ' + target['name'], 'source': source, 'target': target,
                   'changes': preview['changes'], 'mergedAliases': preview['aliases']}
            operations = [upsert('knowledgeTopics', {**source, 'mergedInto': target['id']}),
                          upsert('knowledgeTopics', {**target, 'aliases': preview['aliases']}), upsert('knowledgeMerges', log)]
            for change in preview['changes']:
                operations.append(upsert(change['collection'], {'id': change['id'], 'knowledgeIds': change['after']}))
            return self.store.apply(operations)

    def undo_merge(self, body):
        with self.store.changed:
            state = self.store.snapshot()['state']
            log = next((item for item in state.get('knowledgeMerges', []) if item['id'] == body.get('id')), None)
            if not log or log.get('undone'):
                raise ValueError('该合并不存在或已撤销')
            topics = {item['id']: item for item in state.get('knowledgeTopics', [])}
            source, target = topics.get(log['source']['id']), topics.get(log['target']['id'])
            if not source or not target or source != {**log['source'], 'mergedInto': log['target']['id']} or target != {**log['target'], 'aliases': log['mergedAliases']}:
                raise ValueError('合并后知识点又有修改，请通过历史版本逐项恢复')
            operations = []
            for change in log['changes']:
                record = next((record for record in state.get(change['collection'], []) if record['id'] == change['id']), None)
                if not record or record.get('knowledgeIds') != change['after']:
                    raise ValueError('关联资料已变化，撤销未执行，请比较历史版本')
                operations.append(upsert(change['collection'], {'id': change['id'], 'knowledgeIds': change['before']}))
            operations += [upsert('knowledgeTopics', log['source']), upsert('knowledgeTopics', log['target']), upsert('knowledgeMerges', {'id': log['id'], 'undone': True})]
            operations[0:0] = [{'opId': secrets.token_hex(16), 'kind': 'upsert', 'collection': 'knowledgeTopics', 'id': source['id'], 'fields': {}, 'unset': ['mergedInto']}]
            return self.store.apply(operations)

    def cleanup_preview(self):
        from data_guard import file_ids
        with self.store.lock:
            snapshot = self.store.snapshot()
            active = file_ids(snapshot['state'])
            retained = set()
            for (body,) in self.store.db.execute('SELECT body FROM recovery_records UNION ALL SELECT body FROM sync_conflicts'):
                retained.update(file_ids(json.loads(body)))
            rows = self.store.db.execute('SELECT files.id,name,size,COALESCE(uploaded,0) FROM files LEFT JOIN file_dates ON file_dates.id=files.id').fetchall()
        import time
        from library_sources import Catalog
        linked = Catalog(self.data).ids()
        return {'revision': snapshot['revision'], 'files': len(rows), 'bytes': sum(row[2] for row in rows),
                'protectedFiles': len(active | retained), 'resourceLibraryFiles': len(linked), 'orphans': [{'id': identity, 'name': name, 'bytes': size, 'eligible': time.time() - stamp >= 3600} for identity, name, size, stamp in rows if identity not in active | retained | linked]}

    def cleanup(self, body):
        if body.get('confirmation') != 'DELETE_UNREFERENCED_FILES':
            raise ValueError('需要明确确认清理未引用附件')
        requested = body.get('ids', [])
        if not isinstance(requested, list) or len(requested) > 5000 or not all(isinstance(item, str) for item in requested):
            raise ValueError('清理清单无效')
        with self.store.lock:
            preview = self.cleanup_preview()
            allowed = {item['id'] for item in preview['orphans'] if item['eligible']}
            if preview['revision'] != body.get('revision') or not set(requested) <= allowed:
                raise ValueError('资料引用已变化，请重新预览清理')
            for identity in requested:
                (self.data / 'files' / identity).unlink(missing_ok=True)
                for table in ('files', 'file_dates', 'file_fingerprints', 'attachment_text'):
                    self.store.db.execute('DELETE FROM ' + table + ' WHERE id=?', (identity,))
            self.store.db.commit()
        return {'deleted': len(requested)}

    def search(self, query, offset=0, limit=50, collection='', topic=''):
        query = normalized(query)
        if not query or len(query) > 300:
            raise ValueError('请输入 1 至 300 字搜索内容')
        offset, limit = max(0, int(offset)), max(1, min(100, int(limit)))
        results, indexed, failed, ocr = [], set(), set(), set()
        with self.store.lock:
            cache_key = (query, collection, topic)
            cached = self._search_cache.get(cache_key)
            if cached and cached[0] == self.store.db.total_changes:
                value = cached[1]
                return {**value, 'results': value['results'][offset:offset + limit], 'offset': offset, 'limit': limit, 'hasMore': offset + limit < value['total']}
            snapshot = self.store.snapshot()
            allowed = {(key, item['id']) for key, records in snapshot['state'].items() if isinstance(records, list) and (not collection or key == collection)
                       for item in records if isinstance(item, dict) and item.get('id') and (not topic or topic in item.get('knowledgeIds', []))}
            from data_guard import file_ids
            allowed_files = file_ids([item for key, records in snapshot['state'].items() if isinstance(records, list)
                                      for item in records if isinstance(item, dict) and (key, item.get('id')) in allowed]) if collection or topic else None
            revision = self.store.db.execute('SELECT revision FROM workspace_index_meta WHERE id=1').fetchone()
            if not revision or revision[0] != snapshot['revision']:
                self.store.db.execute('DELETE FROM workspace_text')
                for collection, records in snapshot['state'].items():
                    if isinstance(records, list):
                        for record in records:
                            if isinstance(record, dict) and record.get('id'):
                                index_record(self.store.db, collection, record)
                self.store.db.execute('INSERT OR REPLACE INTO workspace_index_meta VALUES(1,?)', (snapshot['revision'],))
                self.store.db.commit()
            for key, identity, title, text, raw in self.store.db.execute('SELECT collection,record_id,title,body,raw FROM workspace_text WHERE instr(body,?)>0 ORDER BY collection,record_id', (query,)):
                if (key, identity) not in allowed:
                    continue
                position = text.find(query)
                results.append({'collection': key, 'id': identity, 'title': title, 'snippet': raw[max(0, position - 50):position + 180], 'kind': 'record'})
            found_pages = set()
            for identity, page, body in self.store.db.execute('SELECT id,page,body FROM attachment_text'):
                value = json.loads(body)
                indexed.add(identity)
                if value.get('error'):
                    failed.add(identity)
                    continue
                pages = value.get('pageTexts') or [{'page': page, 'text': value.get('text', ''), 'method': value.get('method'), 'legacy': True}]
                for part in pages:
                    if part.get('method') == 'unavailable':
                        failed.add(identity)
                        continue
                    if part.get('method') == 'windows-ocr':
                        ocr.add(identity)
                    text = part.get('text', '')
                    position = normalized(text).find(query)
                    if position >= 0:
                        key = (identity, part['page'])
                        if key in found_pages or allowed_files is not None and identity not in allowed_files:
                            continue
                        found_pages.add(key)
                        results.append({'fileId': identity, 'page': part['page'], 'title': self.store.file_info(identity)['name'] if self.store.file_info(identity) else '附件', 'snippet': text[max(0, position - 50):position + 180], 'kind': 'attachment', 'coverage': '旧索引片段起始页，需重新索引确认' if part.get('legacy') else '已提取单页', 'method': part.get('method')})
            count = self.store.db.execute('SELECT COUNT(*) FROM files').fetchone()[0]
            value = {'results': results, 'total': len(results), 'index': {'indexed': len(indexed), 'unindexed': max(0, count - len(indexed)), 'failed': len(failed), 'ocrUnverified': len(ocr), 'complete': False}}
            if len(self._search_cache) >= 4:
                self._search_cache.pop(next(iter(self._search_cache)))
            self._search_cache[cache_key] = (self.store.db.total_changes, value)
        return {**value, 'results': results[offset:offset + limit], 'offset': offset, 'limit': limit, 'hasMore': offset + limit < len(results)}
