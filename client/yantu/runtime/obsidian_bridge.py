"""Opt-in diary bridge. Filesystem identities never enter shared learning state."""
import base64
import copy
import difflib
import hashlib
import json
import os
import re
import secrets
import threading
import time
from datetime import date, datetime, timedelta
from pathlib import Path
from urllib.parse import quote

MARK = '#同步/研途'
LIMIT = 1024 * 1024
TASK = re.compile(r'^([ \t]*[-*+] \[([^\]])\][ \t]+)(.*?)(\r?\n)?$')
TASK_ID = re.compile(r'<!--\s*yantu-task:([a-f0-9]{32})\s*-->')
REVIEW_ID = re.compile(r'<!--\s*yantu-review:([a-f0-9]{32}):(insight|plan)\s*-->')
HEADING = re.compile(r'^ {0,3}#{1,6}\s+(.+?)(?:\r?\n)?$')
DATE_FILE = re.compile(r'^(\d{4}-\d{2}-\d{2})\.md$')
STATES = {' ': 'todo', '/': 'in_progress', 'x': 'done', 'X': 'done', '-': 'cancelled'}
DATES = {'date': '⏳', 'startDate': '🛫', 'deadline': '📅', 'completedDate': '✅', 'cancelledDate': '❌'}
CONTROLLED = {'done', 'taskStatus', 'date', 'startDate', 'deadline', 'completedDate', 'cancelledDate'}
OWNED = {'estimatedMinutes', 'activityType', 'subject', 'subjectId', 'priority', 'reminder', 'reminderAt',
         'reminderEnabled', 'reminderMinutes', 'knowledgeIds', 'planChanges', 'updatedAt'}
STATS_START, STATS_END = '<!-- yantu:stats:start -->', '<!-- yantu:stats:end -->'


def packed(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'), allow_nan=False)


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def checked_day(value):
    if not isinstance(value, str) or date.fromisoformat(value).isoformat() != value:
        raise ValueError('日期必须为 YYYY-MM-DD')
    return value


def visible_text(value):
    value = re.sub(r'<!--.*?-->', '', value)
    value = re.sub(r'!\[\[[^\]]+\]\]', '', value)
    value = re.sub(r'\[\[([^\]|]+)(?:\|([^\]]+))?\]\]', lambda m: m[2] or Path(m[1].replace('\\', '/')).name, value)
    value = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', value)
    value = re.sub(r'(?:[A-Za-z]:[\\/]|file://|obsidian://)\S+', '[本机链接]', value)
    return value.strip()


def authorized(value):
    # Permission markers in code, links, comments or escaped text are not grants.
    value = re.sub(r'(`+).*?\1', '', value)
    value = re.sub(r'<!--.*?(?:-->|$)|!?\[\[[^\]]*\]\]|!?\[[^\]]*\]\([^)]*\)', '', value)
    return bool(re.search(r'(?<![\w\\/])#同步/研途(?=\s|$)', value))


def active_lines(lines):
    front = bool(lines and lines[0].strip() == '---')
    fence = None
    task_indents = []
    comment, templater, html = False, False, None
    for index, line in enumerate(lines):
        stripped = line.lstrip()
        indentation = len(line[:len(line) - len(stripped)].expandtabs(4))
        if stripped.strip() and indentation == 0 and not TASK.match(line):
            task_indents.clear()
        if comment:
            if '-->' in line:
                comment = False
            continue
        if '<!--' in line and '-->' not in line.split('<!--', 1)[1]:
            comment = True
        if templater:
            if '%>' in line:
                templater = False
            continue
        if '<%' in line:
            templater = '%>' not in line.split('<%', 1)[1]
            continue
        if html:
            if '</' + html in line.lower():
                html = None
            continue
        html_start = re.match(r'^\s*<(script|style|pre|code)(?:\s|>)', line, re.I)
        if html_start:
            if '</' + html_start[1].lower() not in line.lower():
                html = html_start[1].lower()
            continue
        if front:
            if index and line.strip() in {'---', '...'}:
                front = False
            continue
        found = re.match(r'^[ \t]*(`{3,}|~{3,})', line)
        if found:
            token = found[1]
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
            continue
        if fence or stripped.startswith(('>', '<!--', '![[', '<script', '<style')):
            continue
        task = TASK.match(line)
        if indentation >= 4 and not (task and indentation <= 64 and indentation - 4 in task_indents):
            continue
        if task:
            task_indents = [value for value in task_indents if value < indentation]
            task_indents.append(indentation)
        yield index


def parse_document(raw, stamp, config):
    checked_day(stamp)
    if len(raw) > LIMIT:
        raise ValueError('日记超过 1 MiB，已停止自动处理')
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    lines = text.splitlines(keepends=True)
    allowed = list(active_lines(lines))
    allowed_set = set(allowed)
    result = {'tasks': [], 'reviews': [], 'lines': lines, 'bom': bom,
              'newline': '\r\n' if '\r\n' in text else '\n', 'date': stamp, 'sha': digest(raw)}
    headings = [(i, HEADING.match(lines[i])) for i in allowed if HEADING.match(lines[i])]
    for index in allowed:
        match = TASK.match(lines[index])
        if not match or not authorized(match[3]):
            continue
        content = match[3]
        dates = {}
        for key, symbol in DATES.items():
            values = re.findall(re.escape(symbol) + r'\s*(\d{4}-\d{2}-\d{2})', content)
            if values:
                dates[key] = checked_day(values[-1])
        title = TASK_ID.sub('', content).replace(MARK, '')
        for symbol in DATES.values():
            title = re.sub(re.escape(symbol) + r'\s*\d{4}-\d{2}-\d{2}', '', title)
        title = re.sub(r'\s+\^[\w-]+\s*$', '', title)
        identity = TASK_ID.search(content)
        recurring = '🔁' in content
        status = STATES.get(match[2], 'unknown')
        fields = {'title': visible_text(title), 'taskStatus': status, 'done': status == 'done',
                  'date': dates.pop('date', stamp), **dates}
        if fields.get('startDate') and fields.get('deadline') and fields['startDate'] > fields['deadline']:
            raise ValueError('任务开始日不能晚于截止日')
        if not fields['title']:
            continue
        result['tasks'].append({'line': index, 'ref': identity[1] if identity else None,
                                'fields': fields, 'readonly': recurring or status == 'unknown', 'recurring': recurring})
    sections = {}
    for position, (index, match) in enumerate(headings):
        title = match[1]
        if not authorized(title):
            continue
        clean = REVIEW_ID.sub('', title).replace(MARK, '').strip().rstrip('#').strip()
        roles = [role for role, key in [('insight', 'insightHeading'), ('plan', 'planHeading')]
                 if config.get(key) and config[key].strip() == clean]
        if not roles:
            continue
        if len(roles) != 1 or roles[0] in sections:
            raise ValueError('同一天的共享复盘小节重复，请为心得、明日计划各保留一个原件')
        role = roles[0]
        end = headings[position + 1][0] if position + 1 < len(headings) else len(lines)
        section_id = REVIEW_ID.search(title)
        body_lines, task_paragraph = [], False
        for line_index in range(index + 1, end):
            line = lines[line_index]
            if not line.strip():
                task_paragraph = False
            if TASK.match(line):
                task_paragraph = True
            if line_index in allowed_set and not task_paragraph:
                body_lines.append(line)
        body = ''.join(body_lines).strip()
        if len(body) > 100000:
            raise ValueError('共享复盘小节过长')
        sections[role] = {'line': index, 'end': end, 'text': visible_text(body),
                          'ref': section_id[1] if section_id else None}
    if sections:
        identities = {s['ref'] for s in sections.values() if s['ref']}
        if len(identities) > 1:
            raise ValueError('共享复盘编号不一致，请先处理复制的小节')
        fields = {'date': stamp, 'insight': sections.get('insight', {}).get('text', ''),
                  'plan': sections.get('plan', {}).get('text', ''), 'status': None}
        scores = re.findall(r'^\s*学习状态[：:]\s*(\d{1,2})\s*/\s*10\s*$', fields['insight'], re.M)
        if scores:
            if len(scores) > 1 or not 1 <= int(scores[0]) <= 10:
                raise ValueError('学习状态只能有一个 1–10 分评分')
            fields['status'] = int(scores[0])
        result['reviews'].append({'ref': next(iter(identities), None), 'fields': fields, 'sections': sections})
    return result


def encode_document(lines, bom=False):
    return (b'\xef\xbb\xbf' if bom else b'') + ''.join(lines).encode('utf-8')


def task_line(line, fields, ref, newline):
    match = TASK.match(line)
    if not match:
        raise ValueError('源任务格式已变化')
    status = fields.get('taskStatus', 'done' if fields.get('done') else 'todo')
    symbol = {'todo': ' ', 'in_progress': '/', 'done': 'x', 'cancelled': '-'}[status]
    prefix = re.sub(r'\[[^\]]\]', '[' + symbol + ']', match[1], count=1)
    content = match[3]
    anchor_match = re.search(r'(\s+\^[\w-]+)\s*$', content)
    anchor = anchor_match[1] if anchor_match else ''
    if anchor_match:
        content = content[:anchor_match.start()]
    for key, emoji in DATES.items():
        value = fields.get(key)
        expression = re.escape(emoji) + r'\s*\d{4}-\d{2}-\d{2}'
        if value:
            token = emoji + ' ' + checked_day(value)
            if re.search(expression, content):
                content = re.sub(expression, token, content)
            else:
                content = content.rstrip() + ' ' + token
        else:
            content = re.sub(r'\s*' + expression, '', content)
    if not TASK_ID.search(content):
        content = content.rstrip() + f' <!-- yantu-task:{ref} -->'
    return prefix + content + anchor + (match[4] or '')


class ObsidianBridge:
    def __init__(self, store, data, clock=time.time):
        self.store, self.data, self.clock = store, Path(data), clock
        self.home = self.data / 'obsidian-bridge'
        self.lock, self.stop = threading.RLock(), threading.Event()
        self.worker = None
        self.previews, self.cache, self.seen = {}, {}, {}
        self.last_scan = self.last_sync = None
        self.issues = []
        self.read_count = 0
        self.name_cache, self.last_discovery = [], -1e9
        self._cycle_root = None
        self._public_revision, self._public_cache = None, {}
        self._dependencies = {}
        self.observation = {}
        self.started = False
        self.home.mkdir(exist_ok=True)
        self.config_path = self.home / 'config.json'
        self.config = self._load_config()
        with store.lock:
            store.db.executescript('''CREATE TABLE IF NOT EXISTS obsidian_links(ref TEXT PRIMARY KEY,collection TEXT,record_id TEXT UNIQUE,body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS obsidian_outbox(id TEXT PRIMARY KEY,body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS obsidian_conflicts(id TEXT PRIMARY KEY,body TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS obsidian_meta(key TEXT PRIMARY KEY,body TEXT NOT NULL);''')
            store.db.commit()
        store.obsidian = self

    def _today(self):
        return datetime.fromtimestamp(self.clock()).astimezone().date().isoformat()

    def _public_state(self):
        return self._public_snapshot()[1]

    def _public_snapshot(self):
        # Keep only bridge inputs, and decode the shared document once per revision.
        with self.store.lock:
            revision = self.store.revision()
            if revision != self._public_revision:
                state = self.store.snapshot()['state']
                self._public_cache = {key: state.get(key, []) for key in ('tasks', 'reviews', 'focusSessions')}
                self._public_revision = revision
            return self._public_revision, self._public_cache

    def _config_hash(self, config=None):
        return digest(json.dumps(dict(self.config) if config is None else config, sort_keys=True, ensure_ascii=False).encode('utf-8'))

    def _load_config(self):
        default = dict(vaultPath='', diaryFolder='日记', startDate=self._today(), insightHeading='', planHeading='',
                       statsEnabled=True, enabled=False, paused=False)
        if self.config_path.exists():
            default.update(json.loads(self.config_path.read_text(encoding='utf-8')))
        return default

    def _save_config(self):
        target = self.config_path.with_suffix('.tmp')
        with target.open('w', encoding='utf-8', newline='\n') as handle:
            handle.write(packed(self.config)); handle.flush(); os.fsync(handle.fileno())
        os.replace(target, self.config_path)

    def _root(self, config=None):
        if config is None and self._cycle_root is not None:
            return self._cycle_root
        config = config or self.config
        vault = Path(config['vaultPath']).expanduser()
        if not vault.is_absolute() or not vault.is_dir():
            raise ValueError('请选择现有的 Obsidian 综合库目录')
        vault = vault.resolve()
        root = vault / '日记'
        resolved_root = root.resolve()
        if not root.is_dir() or resolved_root != root or not resolved_root.is_relative_to(vault):
            raise ValueError('日记目录不可用，或指向授权库外部')
        return root

    def _file(self, name, config=None):
        if not isinstance(name, str) or not DATE_FILE.fullmatch(name):
            raise ValueError('只允许访问日记目录的 YYYY-MM-DD.md 文件')
        checked_day(name[:-3])
        root = self._root(config)
        path = root / name
        resolved = path.resolve()
        if resolved != path or not resolved.is_relative_to(root):
            raise ValueError('不允许访问日记目录外的文件或链接')
        return path

    def _read(self, path):
        with path.open('rb') as handle:
            raw = handle.read(LIMIT + 1)
        self.read_count += 1
        if len(raw) > LIMIT:
            raise ValueError('日记超过 1 MiB，已停止自动处理')
        return raw

    def _links(self):
        with self.store.lock:
            return {row[0]: json.loads(row[1]) for row in self.store.db.execute('SELECT ref,body FROM obsidian_links')}

    def _link(self, ref, value):
        with self.store.lock:
            old = self.store.db.execute('SELECT body FROM obsidian_links WHERE ref=?', (ref,)).fetchone()
            if old and json.loads(old[0]).get('detached') and not value.get('detached') and not value.get('rebind'):
                return False
            self.store.db.execute('INSERT OR REPLACE INTO obsidian_links VALUES(?,?,?,?)',
                                  (ref, value['collection'], value['recordId'], packed(value)))
            self.store.db.commit()
        return True

    def _conflict(self, identity, kind, message, **details):
        body = {'id': identity, 'kind': kind, 'message': message, **details}
        with self.store.lock:
            self.store.db.execute('INSERT OR REPLACE INTO obsidian_conflicts VALUES(?,?)', (identity, packed(body)))
            self.store.db.commit()
        return body

    def conflicts(self):
        with self.store.lock:
            return {'conflicts': [json.loads(row[0]) for row in self.store.db.execute('SELECT body FROM obsidian_conflicts')]}

    def configuration(self):
        suggested = r'B:\obsidian_note\note\database'
        config = copy.deepcopy(dict(self.config))
        return {'local': True, 'config': config, 'configHash': self._config_hash(config), 'suggestedVaultPath': suggested,
                'templateHeadings': ['今天最值得保留的经验', '明天第一步', '今天完成了', '今天没完成，但可以接受']}

    def configure(self, body):
        with self.lock:
            if 'expectedConfigHash' in body and body['expectedConfigHash'] != self._config_hash():
                raise ValueError('设置已在其他页面修改，请先撤回未保存修改并重新加载')
            action = body.get('action')
            if action in {'pause', 'resume', 'disconnect'}:
                if action == 'resume' and not self.config.get('approved'):
                    raise ValueError('请先预览并确认连接')
                self.config['paused'] = action != 'resume'
                if action == 'disconnect':
                    self.config.update(enabled=False, approved=False)
                    with self.store.lock:
                        self.store.db.execute('DELETE FROM obsidian_outbox')
                        self.store.db.execute("DELETE FROM obsidian_conflicts WHERE id LIKE 'write-%'")
                        self.store.db.commit()
                    state = self._public_state()
                    for ref, link in self._links().items():
                        current = next((r for r in state.get(link['collection'], []) if r['id'] == link['recordId']), {})
                        if link.get('waiting') and current.get('source') != 'obsidian':
                            self._publish(link['collection'], link['recordId'], {'obsidianExport': False}, current,
                                          unset={'bridgeRef', 'bridgeStatus'})
                            link['detached'] = True
                        else:
                            link['base'] = {key: current[key] for key in CONTROLLED if key in current}
                            if current:
                                self._publish(link['collection'], link['recordId'], {'bridgeStatus': 'unlinked'}, current)
                        self._link(ref, link)
                self._save_config()
                return self.status(local=True)
            value = body.get('config', body)
            if not isinstance(value, dict):
                raise ValueError('联动设置格式无效')
            config = {key: value.get(key, self.config.get(key)) for key in
                      ('vaultPath', 'startDate', 'insightHeading', 'planHeading', 'statsEnabled')}
            config.update(diaryFolder='日记', enabled=False, paused=False, approved=False)
            config['startDate'] = checked_day(config['startDate'])
            for key in ('insightHeading', 'planHeading'):
                config[key] = str(config.get(key) or '').strip()
                if len(config[key]) > 160 or '\n' in config[key] or MARK in config[key]:
                    raise ValueError('请填写不带共享标记的单行复盘小节名称')
            if config['insightHeading'] and config['insightHeading'] == config['planHeading']:
                raise ValueError('心得和明日计划不能指向同一个小节')
            config['statsEnabled'] = bool(config['statsEnabled'])
            root = self._root(config)
            config['vaultPath'] = str(root.parent)
            previous = self.config.get('vaultPath')
            if previous and previous != config['vaultPath'] and self._links():
                raise ValueError('更换笔记库前请先解除已有条目关联，不能将旧队列写入新库')
            self.config = config
            self.observation = {}
            self._dependencies.clear()
            self.cache.clear(); self.previews.clear(); self._save_config()
            return self.configuration()

    def status(self, local=False):
        with self.store.lock:
            links = self._links()
            pending = self.store.db.execute('SELECT COUNT(*) FROM obsidian_outbox').fetchone()[0]
            conflicts = self.store.db.execute('SELECT COUNT(*) FROM obsidian_conflicts').fetchone()[0]
            state = self._public_state()
            waiting = {r.get('bridgeRef') for key in ('tasks', 'reviews') for r in state.get(key, [])
                       if r.get('bridgeStatus') in {'pending', 'waiting-file'} and r.get('bridgeRef')}
        active = [entry for entry in links.values() if not entry.get('detached')]
        result = {'local': local, 'enabled': bool(self.config['enabled']), 'paused': bool(self.config['paused']),
                'pending': max(pending, len(waiting)), 'conflicts': conflicts,
                'linkedTasks': sum(entry['collection'] == 'tasks' for entry in active),
                'linkedReviews': sum(entry['collection'] == 'reviews' for entry in active),
                'lastScan': self.last_scan, 'lastSync': self.last_sync,
                'issues': self.issues if local else [{'message': '电脑端联动需要处理，请到电脑查看'}] if self.issues else []}
        if local:
            result.update(configHash=self._config_hash(), observation=copy.deepcopy(self.observation))
        return result

    def _template_preview(self):
        lines = ['<!-- 保留原有 Templater 指令；以下仅为已有小节的调整片段。 -->', '', '- [ ] 待填写任务', '']
        for key in ('insightHeading', 'planHeading'):
            if self.config.get(key):
                lines.extend(['### ' + self.config[key], '', ''])
        lines.append('<!-- 默认不共享；需要时在单条任务或对应标题末尾手动添加 ' + MARK + '。 -->')
        return '\n'.join(lines) + '\n'

    def _names(self, start=None, end=None):
        root = self._root()
        start = start or self.config['startDate']
        return sorted(path.name for path in root.iterdir() if DATE_FILE.fullmatch(path.name)
                      and path.name[:-3] >= start and (not end or path.name[:-3] <= end))

    def preview(self, body):
        with self.lock:
            start = checked_day(body.get('from') or self.config['startDate'])
            end = checked_day(body['to']) if body.get('to') else None
            if end and end < start:
                raise ValueError('开始日期不能晚于结束日期')
            if start < self.config['startDate'] and body.get('history') is not True:
                raise ValueError('历史日记需单独选择日期并预览')
            names, fingerprints, changes, issues = self._names(start, end), {}, [], []
            for name in names:
                try:
                    raw = self._read(self._file(name))
                    doc = parse_document(raw, name[:-3], self.config)
                    fingerprints[name] = digest(raw)
                    for kind in ('tasks', 'reviews'):
                        for entry in doc[kind]:
                            changes.append({'kind': kind, 'date': name[:-3], 'title': entry['fields'].get('title', '已标记的复盘小节'),
                                            'action': '只读关联' if entry.get('readonly') else '建立或更新关联'})
                except (OSError, ValueError, UnicodeError):
                    issues.append({'message': name + ' 无法安全读取，请在电脑端检查格式或权限'})
            token = secrets.token_hex(24)
            self.previews = {key: item for key, item in self.previews.items() if item['expires'] > self.clock()}
            self.previews[token] = {'expires': self.clock() + 300, 'fingerprints': fingerprints,
                                    'config': copy.deepcopy(self.config), 'history': start < self.config['startDate']}
            return {'token': token, 'files': len(fingerprints), 'tasks': sum(c['kind'] == 'tasks' for c in changes),
                    'reviews': sum(c['kind'] == 'reviews' for c in changes), 'changes': changes, 'issues': issues,
                    'templatePreview': self._template_preview()}

    def apply(self, body):
        with self.lock:
            value = self.previews.pop(body.get('token'), None)
            if body.get('confirmation') != 'ENABLE_OBSIDIAN' or not value or value['expires'] <= self.clock() or value['config'] != self.config:
                raise ValueError('预览已变化或过期，请重新预览并确认')
            for name, sha in value['fingerprints'].items():
                if digest(self._read(self._file(name))) != sha:
                    raise ValueError('日记在预览后已有修改，请重新预览')
            links = self._links()
            for name in value['fingerprints']:
                doc = parse_document(self._read(self._file(name)), name[:-3], self.config)
                for entry in doc['tasks'] + doc['reviews']:
                    ref = entry['ref']
                    if ref in links:
                        link = links[ref]
                        link.update(detached=False, rebind=True)
                        self._link(ref, link)
                        with self.store.lock:
                            self.store.db.execute('DELETE FROM obsidian_conflicts WHERE id=?', (ref,)); self.store.db.commit()
            self.config.update(enabled=True, paused=False, approved=True)
            if value['history']:
                self.config['historyFiles'] = sorted(set(self.config.get('historyFiles', [])) | set(value['fingerprints']))
            self._save_config()
            self.cache.clear()
            return self.scan(force=True)

    def validate_client_operations(self, operations, state=None):
        if not isinstance(operations, list):
            raise ValueError('同步操作无效')
        state = state if state is not None else self.store.snapshot()['state']
        links = self._links()
        by_record = {(entry['collection'], entry['recordId']): (ref, entry) for ref, entry in links.items()}
        for op in operations:
            if not isinstance(op, dict):
                raise ValueError('同步操作无效')
            if op.get('kind') == 'setting' and str(op.get('key', '')).lower().startswith('obsidian'):
                raise ValueError('Obsidian 设置仅能在电脑端配置')
            if op.get('kind') not in {'upsert', 'delete'}:
                continue
            col, rid = op.get('collection'), op.get('id')
            fields = op.get('fields', {})
            record = next((r for r in state.get(col, []) if r.get('id') == rid), {})
            pair = by_record.get((col, rid))
            if pair and pair[1].get('detached') and pair[1].get('waiting') and not record.get('bridgeRef'):
                pair = None
            if fields.get('activityType', 'unclassified') not in {'exam', 'skill', 'project', 'unclassified'}:
                raise ValueError('统计归属无效')
            if not pair:
                if record.get('bridgeRef'):
                    permitted = OWNED | (CONTROLLED if col == 'tasks' and not record.get('bridgeReadonly') else set())
                    for key in set(fields) | set(op.get('unset', [])):
                        changed = key in op.get('unset', []) and key in record or key in fields and fields[key] != record.get(key)
                        if changed and key not in permitted:
                            raise ValueError('来源映射尚未重新连接，正文不能从研途改写')
                    continue
                if any(key.startswith('bridge') for key in fields) or fields.get('source') == 'obsidian':
                    raise ValueError('不能伪造 Obsidian 来源或关联编号')
                continue
            ref, link = pair
            if op['kind'] == 'delete':
                continue
            if link.get('detached'):
                raise ValueError('条目已经解除关联，请在电脑端重新关联，旧离线队列不能恢复回写')
            allowed = OWNED | (CONTROLLED if col == 'tasks' and not link.get('readonly') else set())
            if link.get('waiting'):
                allowed |= {'title', 'obsidianExport'}
            for key in set(fields) | set(op.get('unset', [])):
                changed = key in op.get('unset', []) and key in record or key in fields and fields[key] != record.get(key)
                if changed and key not in allowed:
                    raise ValueError('关联正文由 Obsidian 主写；循环或未知状态任务不能从研途修改')
            if col == 'tasks':
                if 'taskStatus' in fields:
                    if fields['taskStatus'] not in set(STATES.values()):
                        raise ValueError('任务状态无效')
                if 'done' in fields and type(fields['done']) is not bool:
                    raise ValueError('任务完成状态无效')
                if 'done' in fields and 'taskStatus' in fields and fields['done'] != (fields['taskStatus'] == 'done'):
                    raise ValueError('任务完成状态与任务阶段不一致')

    def normalize_record(self, collection, record, fields, previous, unset=()):
        # Derive coupled fields only after the normal optimistic conflict guard.
        if collection != 'tasks' or not record.get('bridgeRef'):
            return
        for key in ('startDate', 'deadline', 'completedDate', 'cancelledDate'):
            if key in fields and fields[key] in ('', None):
                record.pop(key, None)
        if 'taskStatus' in fields:
            record['done'] = record['taskStatus'] == 'done'
        elif 'done' in fields:
            record['taskStatus'] = 'done' if record['done'] else 'todo'
        if 'done' in fields or 'taskStatus' in fields:
            if record.get('done'):
                if not previous or not previous.get('done'):
                    record['completedDate'] = self._today()
            else:
                record.pop('completedDate', None)
        if (set(fields) | set(unset)) & CONTROLLED:
            record['bridgeStatus'] = 'pending'

    def after_operations(self, operations):
        # Called inside the same SQLite transaction; never performs file I/O.
        for op in operations:
            if op.get('kind') != 'delete':
                continue
            row = self.store.db.execute('SELECT ref,body FROM obsidian_links WHERE collection=? AND record_id=?',
                                        (op.get('collection'), op.get('id'))).fetchone()
            if row:
                entry = json.loads(row[1]); entry['detached'] = True
                self.store.db.execute('UPDATE obsidian_links SET body=? WHERE ref=?', (packed(entry), row[0]))
                self.store.db.execute('DELETE FROM obsidian_conflicts WHERE id=?', (row[0],))

    def _publish(self, collection, rid, fields, previous=None, unset=()):
        previous = previous if previous is not None else next((r for r in self._public_state().get(collection, []) if r['id'] == rid), {})
        changes = {k: v for k, v in fields.items() if previous.get(k) != v or k not in previous}
        unset = [k for k in unset if k in previous]
        if not changes and not unset:
            return
        op = {'opId': secrets.token_hex(16), 'kind': 'upsert', 'collection': collection, 'id': rid,
              'fields': changes, 'unset': unset, 'baseExists': bool(previous), 'baseRecord': previous,
              'base': {k: {'exists': k in previous, 'value': previous.get(k)} for k in set(changes) | set(unset)}}
        self.store.apply([op], trusted_obsidian=True)

    def _set_phase(self, entry, phase):
        self._publish(entry['collection'], entry['recordId'], {'bridgeStatus': phase})

    def _patches(self, raw, target):
        old, new = raw.splitlines(keepends=True), target.splitlines(keepends=True)
        offsets = [0]
        for line in old:
            offsets.append(offsets[-1] + len(line))
        return [[offsets[i], offsets[j], base64.b64encode(b''.join(new[a:b])).decode('ascii')]
                for kind, i, j, a, b in difflib.SequenceMatcher(a=old, b=new, autojunk=False).get_opcodes() if kind != 'equal']

    def _backup(self, raw, identity):
        folder = self.home / 'private-backups'; folder.mkdir(exist_ok=True)
        path = folder / (identity + '.md')
        if path.exists():
            if digest(path.read_bytes()) != digest(raw):
                raise ValueError('原文件版本校验失败')
            return path.name
        cutoff = self.clock() - 30 * 86400
        with self.store.lock:
            protected = {json.loads(row[0]).get('backup') for row in self.store.db.execute('SELECT body FROM obsidian_outbox')}
        files = sorted(folder.glob('*.md'), key=lambda p: p.stat().st_mtime)
        if self.conflicts()['conflicts']:
            protected.update(p.name for p in files)
        total = sum(p.stat().st_size for p in files)
        for path in files:
            if path.name in protected:
                continue
            if path.stat().st_mtime < cutoff or total + len(raw) > 100 * 1024 ** 2:
                size = path.stat().st_size; path.unlink(); total -= size
        if total + len(raw) > 100 * 1024 ** 2:
            raise ValueError('本机版本保护空间不足，已暂停回写')
        path = folder / (identity + '.md')
        if not path.exists():
            with path.open('xb') as handle:
                handle.write(raw); handle.flush(); os.fsync(handle.fileno())
        return path.name

    def _write(self, name, raw, target):
        if raw == target:
            return
        if len(target) > LIMIT:
            raise ValueError('新增关联或统计后日记超过 1 MiB，未写入文件')
        identity = secrets.token_hex(16)
        job = {'id': identity, 'name': name, 'base': digest(raw), 'target': digest(target),
               'patches': self._patches(raw, target), 'created': self.clock(), 'backup': identity + '.md'}
        with self.store.lock:
            self.store.db.execute('INSERT INTO obsidian_outbox VALUES(?,?)', (identity, packed(job))); self.store.db.commit()
        self._complete_write(job)

    def _complete_write(self, job):
        path = self._file(job['name'])
        raw = self._read(path)
        if digest(raw) == job['target']:
            with self.store.lock:
                self.store.db.execute('DELETE FROM obsidian_outbox WHERE id=?', (job['id'],)); self.store.db.commit()
            return
        if digest(raw) != job['base']:
            self._conflict('write-' + job['id'], 'file', '日记在回写期间已有修改，已保留待处理版本', jobId=job['id'])
            raise ValueError('日记已有并发修改，待回写操作已暂停')
        target = raw
        for start, end, replacement in sorted(job['patches'], reverse=True):
            target = target[:start] + base64.b64decode(replacement) + target[end:]
        if digest(target) != job['target']:
            raise ValueError('待回写内容校验失败')
        self._backup(raw, job['id'])
        temp = path.with_name('.yantu-' + job['id'] + '.tmp')
        try:
            if temp.exists():
                if temp.resolve() != temp or temp.is_symlink():
                    raise ValueError('临时回写路径不安全')
                if digest(self._read(temp)) != job['target']:
                    temp.unlink()
            if not temp.exists():
                with temp.open('xb') as handle:
                    handle.write(target); handle.flush(); os.fsync(handle.fileno())
            if digest(self._read(path)) != job['base']:
                self._conflict('write-' + job['id'], 'file', '写入前原文变化，未覆盖文件', jobId=job['id'])
                raise ValueError('写入前原文变化，请先处理冲突')
            os.replace(temp, path)
            if digest(self._read(path)) != job['target']:
                raise ValueError('回写复读校验失败，请在电脑端检查版本')
            with self.store.lock:
                self.store.db.execute('DELETE FROM obsidian_outbox WHERE id=?', (job['id'],)); self.store.db.commit()
        finally:
            temp.unlink(missing_ok=True)

    def _recover(self):
        with self.store.lock:
            jobs = [json.loads(row[0]) for row in self.store.db.execute('SELECT body FROM obsidian_outbox')]
        blocked = set()
        held = {c.get('jobId') for c in self.conflicts()['conflicts'] if c['kind'] == 'file'}
        for job in jobs:
            if job['id'] in held:
                blocked.add(job['name']); continue
            try:
                self._complete_write(job)
            except (OSError, ValueError):
                blocked.add(job['name'])
                self.issues.append({'message': '有日记回写等待处理，请查看冲突或本机版本空间'})
        return blocked

    def _task_merge(self, entry, binding, current):
        incoming = copy.deepcopy(entry['fields'])
        if not binding or not current:
            return incoming
        base = binding.get('base', {})
        conflicts = {}
        for key in CONTROLLED:
            a, b, c = base.get(key), incoming.get(key), current.get(key)
            if key not in base and key not in incoming and key not in current:
                continue
            if c != a and b != a and b != c:
                conflicts[key] = {'base': a, 'obsidian': b, 'yantu': c}
            elif c != a and b == a:
                if key in current:
                    incoming[key] = c
                else:
                    incoming.pop(key, None)
        if conflicts:
            self._conflict(binding['ref'], 'fields', '两端修改了同一任务字段，请选择保留哪一版',
                           ref=binding['ref'], fields=conflicts, current=entry['fields'], incoming={k: current.get(k) for k in CONTROLLED})
            self._set_phase(binding, 'conflict')
            return None
        incoming['done'] = incoming.get('taskStatus') == 'done'
        if incoming['done'] and not incoming.get('completedDate'):
            incoming['completedDate'] = self._today()
        if not incoming['done']:
            incoming.pop('completedDate', None)
        return incoming

    def _stats(self, state, stamp, nl):
        from activity_classification import daily_summary
        summary = daily_summary(state, stamp)
        return nl.join([STATS_START, '### 研途统计',
                        f"- 已记录专注：{int(summary['totalSeconds'] // 60)} 分钟",
                        f"- 考研投入：{int(summary['examSeconds'] // 60)} 分钟",
                        f"- 完成任务：{summary['completedTasks']} 项", STATS_END, ''])

    def _scan_file(self, name, raw, links, duplicate_refs):
        stamp = name[:-3]
        doc = parse_document(raw, stamp, self.config)
        lines, nl = list(doc['lines']), doc['newline']
        state = self._public_state()
        publications, found, has_grant = [], set(), bool(doc['tasks'] or doc['reviews'])
        blocked = False
        existing_conflicts = {c['id'] for c in self.conflicts()['conflicts']}
        for collection in ('tasks', 'reviews'):
            for entry in doc[collection]:
                ref = entry['ref'] or secrets.token_hex(16)
                entry['ref'] = ref
                found.add(ref)
                binding = links.get(ref)
                if ref in duplicate_refs or ref in existing_conflicts:
                    if binding:
                        self._set_phase(binding, 'conflict')
                    blocked = True; continue
                if binding and binding.get('detached'):
                    continue
                if binding and binding['collection'] != collection:
                    self._conflict(ref, 'duplicate', '关联编号类型冲突，请选择原件或拆分任务', ref=ref)
                    blocked = True; continue
                restored = [r for r in state.get(collection, []) if r.get('bridgeRef') == ref]
                if len(restored) > 1:
                    self._conflict(ref, 'duplicate', '恢复的数据含重复关联编号，请在电脑端先处理', ref=ref)
                    blocked = True; continue
                rid = binding['recordId'] if binding else restored[0]['id'] if restored else ('ob-t-' if collection == 'tasks' else 'ob-r-') + ref
                current = next((r for r in state.get(collection, []) if r.get('id') == rid), {})
                if binding and not current and not binding.get('rebind'):
                    self._conflict(ref, 'missing-record', '研途记录已变化或恢复，请重新预览关联', ref=ref)
                    blocked = True; continue
                fields = entry['fields']
                if collection == 'tasks':
                    if entry['readonly']:
                        merged = copy.deepcopy(fields)
                    else:
                        merged = self._task_merge(entry, binding, current)
                    if merged is None:
                        blocked = True; continue
                    lines[entry['line']] = task_line(lines[entry['line']], merged, ref, nl) if not entry['readonly'] else lines[entry['line']]
                    if not TASK_ID.search(lines[entry['line']]):
                        line = lines[entry['line']]; ending = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
                        body = line[:-len(ending)] if ending else line
                        anchor = re.search(r'\s+\^[\w-]+\s*$', body)
                        offset = anchor.start() if anchor else len(body)
                        lines[entry['line']] = body[:offset] + f' <!-- yantu-task:{ref} -->' + body[offset:] + ending
                    fields = merged
                else:
                    for role, section in entry['sections'].items():
                        index = section['line']
                        if not REVIEW_ID.search(lines[index]):
                            ending = '\r\n' if lines[index].endswith('\r\n') else '\n' if lines[index].endswith('\n') else ''
                            title = lines[index][:-len(ending)] if ending else lines[index]
                            lines[index] = title + f' <!-- yantu-review:{ref}:{role} -->' + ending
                link = {'ref': ref, 'collection': collection, 'recordId': rid, 'name': name,
                        'date': stamp, 'base': fields, 'readonly': bool(entry.get('readonly')), 'detached': False}
                public = {**fields, 'bridgeRef': ref, 'source': 'obsidian', 'bridgeReadonly': bool(entry.get('readonly')),
                          'bridgeStatus': 'synced', 'journalDate': stamp}
                if not current:
                    public['activityType'] = 'unclassified'
                publications.append((link, public, current))
        if has_grant and self.config['statsEnabled'] and not blocked:
            text = ''.join(lines)
            stats = self._stats(state, stamp, nl)
            if STATS_START in text or STATS_END in text:
                if text.count(STATS_START) != 1 or text.count(STATS_END) != 1 or text.index(STATS_END) < text.index(STATS_START):
                    raise ValueError('统计区边界重复或损坏，未改写文件')
                text = re.sub(re.escape(STATS_START) + r'.*?' + re.escape(STATS_END) + r'(?:\r?\n)?', lambda _: stats, text, flags=re.S)
            else:
                text += (nl if text and not text.endswith('\n') else '') + nl + stats
            target = (b'\xef\xbb\xbf' if doc['bom'] else b'') + text.encode('utf-8')
        else:
            target = encode_document(lines, doc['bom'])
        self._write(name, raw, target)
        for link, fields, current in publications:
            self._publish(link['collection'], link['recordId'], fields, current,
                          CONTROLLED - set(fields) if link['collection'] == 'tasks' else ())
            self._link(link['ref'], link)
            links[link['ref']] = link
        for ref, link in list(links.items()):
            if link['name'] == name and ref not in found and not link.get('detached') and not link.get('waiting'):
                self.store.apply([{'opId': secrets.token_hex(16), 'kind': 'delete', 'collection': link['collection'], 'id': link['recordId']}], trusted_obsidian=True)
                link['detached'] = True; self._link(ref, link)
                with self.store.lock:
                    self.store.db.execute('DELETE FROM obsidian_conflicts WHERE id=?', (ref,)); self.store.db.commit()
        self.last_sync = int(self.clock() * 1000)
        return target

    def _queue_exports(self):
        state = self._public_state()
        for record in state.get('tasks', []):
            if record.get('obsidianExport') is not True or record.get('source') == 'obsidian':
                continue
            ref = record.get('bridgeRef') or secrets.token_hex(16)
            name = checked_day(record.get('date') or self._today()) + '.md'
            path = self._file(name)
            link = {'ref': ref, 'collection': 'tasks', 'recordId': record['id'], 'name': name,
                    'date': name[:-3], 'base': {}, 'readonly': False, 'waiting': True, 'detached': False}
            previous = self._links().get(ref)
            if previous and previous.get('detached'):
                continue
            self._link(ref, link)
            if not path.exists():
                self._publish('tasks', record['id'], {'bridgeRef': ref, 'bridgeStatus': 'waiting-file'}, record)
                continue
            raw = self._read(path)
            doc = parse_document(raw, name[:-3], self.config)
            if not any(entry['ref'] == ref for entry in doc['tasks']):
                title = str(record.get('title') or '').replace('\n', ' ').replace('\r', ' ').strip()
                if not title or len(title) > 300 or MARK in title or '<!--' in title:
                    self.issues.append({'message': '待写回任务标题为空、过长或包含保留标记'}); continue
                fields = {'done': bool(record.get('done')), 'taskStatus': record.get('taskStatus') or ('done' if record.get('done') else 'todo'),
                          'date': name[:-3], **{key: record[key] for key in ('startDate', 'deadline') if record.get(key)}}
                line = task_line('- [ ] ' + title + ' ' + MARK + doc['newline'], fields, ref, doc['newline'])
                text = raw.decode('utf-8-sig')
                text += (doc['newline'] if text and not text.endswith('\n') else '') + doc['newline'] + '### 研途新增任务' + doc['newline'] + line
                target = (b'\xef\xbb\xbf' if doc['bom'] else b'') + text.encode('utf-8')
                self._write(name, raw, target)
            self._publish('tasks', record['id'], {'bridgeRef': ref, 'bridgeStatus': 'pending'}, record)
            self.cache.pop(name, None)

    def scan(self, force=False):
        with self.lock:
            if not self.config['enabled'] or self.config['paused']:
                return self.status(local=True)
            self.last_scan = int(self.clock() * 1000)
            self.issues = []
            try:
                self._cycle_root = self._root()
                blocked = self._recover()
                self._queue_exports()
                links = self._links()
                if force or self.clock() - self.last_discovery >= 10:
                    self.name_cache, self.last_discovery = self._names(), self.clock()
                names = sorted(set(self.name_cache) | set(self.config.get('historyFiles', [])) | {l['name'] for l in links.values() if not l.get('detached')})
                documents, refs = {}, {}
                revision, state = self._public_snapshot()
                by_name = {}
                for link in links.values():
                    if not link.get('detached'):
                        by_name.setdefault(link['name'], set()).add((link['collection'], link['recordId']))
                by_id = {(key, record['id']): record for key in ('tasks', 'reviews') for record in state.get(key, [])}
                def dependency(name):
                    identities = tuple(sorted(by_name.get(name, ())))
                    version = (revision, self.config['statsEnabled'], identities)
                    previous = self._dependencies.get(name)
                    if previous and previous[0] == version:
                        return previous[1]
                    records = [by_id[identity] for identity in identities if identity in by_id]
                    summary = None
                    if self.config['statsEnabled']:
                        from activity_classification import daily_summary
                        summary = daily_summary(state, name[:-3])
                    value = digest(packed([records, summary]).encode('utf-8'))
                    self._dependencies[name] = (version, value)
                    return value
                for name in names:
                    try:
                        path = self._file(name)
                        stat = path.stat()
                        signature = (stat.st_mtime_ns, stat.st_size)
                        if not force and self.seen.get(name, (None, None))[0] != signature:
                            self.seen[name] = (signature, self.clock()); continue
                        if not force and self.clock() - self.seen.get(name, (signature, 0))[1] < 1:
                            continue
                        cached = self.cache.get(name)
                        dep = dependency(name)
                        if not force and cached and cached['signature'] == signature and cached.get('dependency') == dep:
                            doc, raw = cached['doc'], cached['raw']
                        else:
                            raw = self._read(path); doc = parse_document(raw, name[:-3], self.config)
                        doc_refs = {key: [{'ref': e['ref']} for e in doc[key]] for key in ('tasks', 'reviews')}
                        documents[name] = (digest(raw) if raw is not None else cached.get('sha'), doc_refs, signature, cached, dep)
                        for entry in doc['tasks'] + doc['reviews']:
                            if entry['ref']:
                                refs.setdefault(entry['ref'], []).append(name)
                    except FileNotFoundError:
                        self.issues.append({'message': '已关联日记暂时不可用，未删除研途记录'})
                    except (OSError, ValueError, UnicodeError):
                        self.issues.append({'message': '日记读取失败或格式不受支持，未改动原文'})
                today_name = self._today() + '.md'
                self.observation = {'files': len(documents),
                                    'today': 'found' if today_name in documents else 'unavailable' if today_name in names else 'missing',
                                    'sharedTasks': sum(len(doc[1]['tasks']) for doc in documents.values()),
                                    'sharedReviews': sum(len(doc[1]['reviews']) for doc in documents.values())}
                duplicates = {ref for ref, places in refs.items() if len(places) > 1}
                for ref in duplicates:
                    self._conflict(ref, 'duplicate', '同一关联编号出现在多处，请保留一个原件或移除复制项的隐藏编号', ref=ref)
                for conflict in self.conflicts()['conflicts']:
                    if conflict['kind'] == 'duplicate' and conflict['id'] not in duplicates and len(refs.get(conflict['id'], [])) == 1:
                        with self.store.lock:
                            self.store.db.execute('DELETE FROM obsidian_conflicts WHERE id=?', (conflict['id'],)); self.store.db.commit()
                active_names = {link['name'] for link in links.values() if not link.get('detached')}
                for name, (sha, doc, signature, cached, dep) in documents.items():
                    if name in blocked:
                        continue
                    if not force and cached and cached['signature'] == signature and cached.get('dependency') == dep:
                        continue
                    if not doc['tasks'] and not doc['reviews'] and name not in active_names:
                        self.cache[name] = {'signature': signature, 'doc': {'tasks': [], 'reviews': []}, 'raw': None, 'sha': sha, 'dependency': dep}
                        continue
                    try:
                        raw = self._read(self._file(name))
                        if digest(raw) != sha:
                            self.cache.pop(name, None)
                            continue
                        target = self._scan_file(name, raw, links, duplicates)
                        stat = self._file(name).stat()
                        parsed = parse_document(target, name[:-3], self.config)
                        # Cache only explicitly shared parse data, not private diary bodies.
                        self.cache[name] = {'signature': (stat.st_mtime_ns, stat.st_size),
                                            'doc': {'tasks': [{'ref': e['ref']} for e in parsed['tasks']], 'reviews': [{'ref': e['ref']} for e in parsed['reviews']]},
                                            'raw': None, 'sha': digest(target), 'dependency': dep}
                    except (OSError, ValueError, UnicodeError):
                        self.issues.append({'message': '有日记未完成回写，请在电脑端处理冲突或文件权限'})
                        self.cache.pop(name, None)
                return self.status(local=True)
            except (OSError, ValueError, UnicodeError):
                self.observation = {'today': 'unavailable'}
                self.issues.append({'message': '日记目录暂不可用，修改已保留待同步'})
                return self.status(local=True)
            finally:
                self._cycle_root = None

    def resolve(self, body):
        with self.lock:
            identity, choice = body.get('id'), body.get('choice')
            if choice not in {'obsidian', 'yantu'}:
                raise ValueError('请选择保留 Obsidian 原文或研途结构化字段')
            with self.store.lock:
                row = self.store.db.execute('SELECT body FROM obsidian_conflicts WHERE id=?', (identity,)).fetchone()
            if not row:
                raise ValueError('冲突已处理，请刷新')
            conflict = json.loads(row[0])
            if conflict['kind'] in {'duplicate', 'missing-record'}:
                raise ValueError('请先在 Obsidian 选择原件或修改复制项编号，再重新预览')
            if conflict['kind'] == 'file':
                if choice != 'obsidian':
                    raise ValueError('文件并发冲突只允许保留原文；结构化修改将在重新扫描后重新比较')
                with self.store.lock:
                    self.store.db.execute('DELETE FROM obsidian_outbox WHERE id=?', (conflict['jobId'],)); self.store.db.commit()
            else:
                ref = conflict['ref']; link = self._links().get(ref)
                if not link:
                    raise ValueError('关联已解除')
                doc = parse_document(self._read(self._file(link['name'])), link['name'][:-3], self.config)
                source = next((e['fields'] for e in doc['tasks'] if e['ref'] == ref), None)
                current = next((r for r in self.store.snapshot()['state'].get('tasks', []) if r['id'] == link['recordId']), None)
                if source is None or current is None:
                    raise ValueError('冲突来源已变化，请先重新扫描')
                incoming = {key: current.get(key) for key in CONTROLLED}
                if source != conflict['current'] or incoming != conflict['incoming']:
                    self._conflict(identity, 'fields', '冲突后内容又有修改，请刷新并重新选择', ref=ref,
                                   fields=conflict['fields'], current=source, incoming=incoming)
                    raise ValueError('冲突后内容又有修改，请刷新并重新选择')
                if choice == 'obsidian':
                    self._publish('tasks', link['recordId'], {key: conflict['current'][key] for key in CONTROLLED if key in conflict['current']},
                                  unset=CONTROLLED - set(conflict['current']))
                link['base'] = conflict['current']; self._link(ref, link)
            with self.store.lock:
                self.store.db.execute('DELETE FROM obsidian_conflicts WHERE id=?', (identity,)); self.store.db.commit()
            self.cache.clear()
            self._public_revision = None
            return self.scan(force=True)

    def source(self, ref):
        link = self._links().get(ref)
        if not link or link.get('detached'):
            raise ValueError('关联不存在或已解除')
        self._file(link['name'])
        return {'url': 'obsidian://open?vault=' + quote(Path(self.config['vaultPath']).name, safe='') + '&file=' + quote('日记/' + link['name'], safe='')}

    def pause_for_restore(self):
        with self.lock:
            self.config['paused'] = True; self.config['approved'] = False; self._save_config()
            self.cache.clear()
            self._public_revision = None
            self._dependencies.clear()

    def start(self):
        if self.worker:
            return
        def run():
            while not self.stop.wait(2):
                try:
                    self.scan()
                except Exception:
                    self.issues = [{'message': '联动暂时停止，请在电脑端查看设置'}]
        self.worker = threading.Thread(target=run, name='yantu-obsidian-bridge', daemon=True)
        self.worker.start()

    def close(self):
        self.stop.set()
        if self.worker:
            self.worker.join(timeout=5)
