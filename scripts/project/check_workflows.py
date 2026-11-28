"""Check the explicit reusable CI catalog, dependencies and least-privilege policy.

This is a contract check for the repository's block-style workflow files, not a
replacement for GitHub's YAML/platform validation. No network, installs or jobs.
"""
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
CATALOG = '.github/workflow-catalog.json'
NAME = re.compile(r'[a-z][a-z0-9-]{0,63}')
PATH = re.compile(r'\.github/workflows/[a-z][a-z0-9-]*\.ya?ml')
GUARD = re.compile(r"github\.event_name == 'workflow_dispatch'(?: && (?:inputs\.\w+|\(inputs\.\w+(?: \|\| inputs\.\w+)*\)))?")


def section(text, key):
    match = re.search(r'^' + re.escape(key) + r':\s*\n', text, re.M)
    if not match:
        raise ValueError('Missing block section: ' + key)
    rest = text[match.end():]
    end = re.search(r'^[A-Za-z_][\w-]*:', rest, re.M)
    return rest[:end.start()] if end else rest


def jobs(text):
    body = section(text, 'jobs')
    headers = list(re.finditer(r'^  ([a-z][a-z0-9-]*):\s*\n', body, re.M))
    if not headers:
        raise ValueError('No workflow jobs')
    names = [match[1] for match in headers]
    if len(names) != len(set(names)):
        raise ValueError('Duplicate workflow job')
    return {match[1]: body[match.end():headers[i + 1].start() if i + 1 < len(headers) else len(body)]
            for i, match in enumerate(headers)}


def needs(block):
    found = re.search(r'^    needs: ([^\n]+)$', block, re.M)
    if not found:
        return []
    value = found[1].strip()
    result = [item.strip() for item in value[1:-1].split(',')] if value.startswith('[') and value.endswith(']') else [value]
    if any(not NAME.fullmatch(item) for item in result) or len(result) != len(set(result)):
        raise ValueError('Invalid job dependency')
    return result


def acyclic(graph):
    visiting, visited = set(), set()
    def visit(node):
        if node in visiting:
            raise ValueError('Cyclic workflow dependency')
        if node in visited:
            return
        if node not in graph:
            raise ValueError('Missing workflow dependency')
        visiting.add(node)
        for child in graph[node]:
            visit(child)
        visiting.remove(node)
        visited.add(node)
    for node in graph:
        visit(node)


def check(catalog, sources):
    if catalog.get('schemaVersion') != 1:
        raise ValueError('Unsupported workflow catalog schema')
    rows = catalog.get('workflows')
    if not isinstance(rows, list) or not 2 <= len(rows) <= 64:
        raise ValueError('Expected bounded multiple workflow entries')
    by_id, by_path = {}, {}
    for row in rows:
        name, path = row.get('id', ''), row.get('path', '')
        if not isinstance(name, str) or not NAME.fullmatch(name) or name in by_id:
            raise ValueError('Invalid or duplicate workflow id')
        if not isinstance(path, str) or not PATH.fullmatch(path) or path in by_path:
            raise ValueError('Invalid or duplicate workflow path')
        if row.get('kind') not in ('entrypoint', 'reusable') or type(row.get('manualOnly')) is not bool:
            raise ValueError('Invalid workflow kind or manual policy')
        if not isinstance(row.get('jobs'), list) or not row['jobs'] or len(set(row['jobs'])) != len(row['jobs']):
            raise ValueError('Invalid catalog job names')
        if not isinstance(row.get('requires'), list) or any(not isinstance(x, str) or not NAME.fullmatch(x) for x in row['requires']):
            raise ValueError('Invalid lane dependencies')
        if not isinstance(row.get('inputs'), dict) or any(v not in ('boolean', 'string', 'number') for v in row['inputs'].values()):
            raise ValueError('Invalid workflow input types')
        by_id[name], by_path[path] = row, row
    if set(sources) != set(by_path):
        raise ValueError('Unregistered or missing workflow file')
    calls = {path: [] for path in by_path}
    for path, row in by_path.items():
        text = sources[path]
        if '\ufffd' in text or re.search(r'\?{3,}', text):
            raise ValueError('Workflow encoding corruption')
        events = re.findall(r'^  ([a-z][a-z0-9_]*):', section(text, 'on'), re.M)
        allowed = {'workflow_call'} if row['kind'] == 'reusable' else {'push', 'pull_request', 'workflow_dispatch'}
        if not events or len(events) != len(set(events)) or not set(events) <= allowed:
            raise ValueError('Unexpected workflow trigger')
        inputs = dict(re.findall(r'^      (\w+):\s*\n(?:(?:^        .*\n)|\n)*?^        type: (boolean|string|number)$', section(text, 'on'), re.M))
        if inputs != row['inputs']:
            raise ValueError('Workflow input contract mismatch')
        if section(text, 'permissions').strip() != 'contents: read':
            raise ValueError('Workflow permissions must stay read-only')
        if re.search(r'\bsecrets:\s*inherit\b|pull_request_target|^\s*(?:-\s+)?run:\s*\$\{\{', text, re.M):
            raise ValueError('Unsafe secret inheritance or command interface')
        definitions = jobs(text)
        if list(definitions) != row['jobs']:
            raise ValueError('Catalog job mismatch')
        dependencies = {name: needs(block) for name, block in definitions.items()}
        acyclic(dependencies)
        for name, block in definitions.items():
            if row['manualOnly']:
                guard = re.search(r'^    if: (.*)$', block, re.M)
                if not guard or not GUARD.fullmatch(guard[1]):
                    raise ValueError('Missing inner manual dispatch gate')
                if '    environment: isolated-ci\n' not in block:
                    raise ValueError('Missing isolated environment gate')
            call = re.search(r'^    uses: (.*)$', block, re.M)
            if call:
                target = call[1]
                if not target.startswith('./') or target[2:] not in by_path:
                    raise ValueError('Unknown local reusable workflow')
                target = target[2:]
                called = by_path[target]
                if called['kind'] != 'reusable':
                    raise ValueError('Workflow entrypoint cannot be called')
                if dependencies[name] != called['requires']:
                    raise ValueError('Caller lost required lane dependencies')
                if called['manualOnly']:
                    guard = re.search(r'^    if: (.*)$', block, re.M)
                    if not guard or not GUARD.fullmatch(guard[1]):
                        raise ValueError('Missing caller manual dispatch gate')
                calls[path].append(target)
        for action in re.findall(r'^\s+(?:- )?uses: (.*)$', text, re.M):
            if action.startswith('./'):
                continue
            if not re.fullmatch(r'[A-Za-z0-9_./-]+@[0-9a-f]{40}(?:\s+#.*)?', action):
                raise ValueError('External action must have a full commit pin')
    acyclic(calls)
    if not any(row['kind'] == 'entrypoint' for row in rows):
        raise ValueError('Missing workflow entrypoint')
    return {'passed': True, 'workflows': len(rows), 'reusable': sum(row['kind'] == 'reusable' for row in rows),
            'manualLanes': [row['id'] for row in rows if row['manualOnly']], 'remoteActionsRun': False}


def load(root=ROOT):
    root = Path(root).resolve()
    catalog = json.loads((root / CATALOG).read_text(encoding='utf-8'))
    folder = root / '.github/workflows'
    sources = {}
    for path in sorted(folder.iterdir()):
        if path.suffix not in ('.yml', '.yaml'):
            continue
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError('Workflow escaped project root')
        sources[path.relative_to(root).as_posix()] = path.read_text(encoding='utf-8')
    return catalog, sources


if __name__ == '__main__':
    print(json.dumps(check(*load())))