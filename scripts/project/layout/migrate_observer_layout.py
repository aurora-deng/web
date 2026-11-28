"""Read-only plan or explicitly reviewed Observer component migration.

Does not start HTTP servers, read private configuration, install or deploy.
"""
import argparse
import ast
import importlib.util
from importlib.util import resolve_name
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('layout_safety', Path(__file__).with_name('migrate_application_layout.py'))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
MANIFEST = 'docs/architecture/source-observer-layout-migration.json'
PREFIX = 'tools/observability/observer'
GROUPS = {
    'contracts': ['events', 'operation_catalog'],
    'alerts': ['alert_policy', 'custom_alerts'],
    'queries': ['experiment_queries', 'metric_queries', 'prometheus_queries', 'trace_queries'],
    'metrics': ['histogram', 'local_metrics', 'measurement_quality', 'service_levels', 'prometheus_service_levels'],
    'logs': ['gateway_logs', 'log_store', 'structured_logging'],
    'resources': ['linux_resources', 'resource_history', 'commit_report'],
    'traces': ['local_traces'], 'analysis': ['projection'],
    'sources': ['external_profiles', 'source_monitor', 'sources', 'target_http'],
    'storage': ['store'], 'configuration': ['workspace'],
    'http': ['server', 'access_preflight'],
}
MOVES = [(f'{PREFIX}/{name}.py', f'{PREFIX}/{group}/{name}.py')
         for group, names in GROUPS.items() for name in names]
MODULE_MAP = {old[:-3].replace('/', '.'): new[:-3].replace('/', '.') for old, new in MOVES}


def import_rewrite(text, name):
    if not name.endswith('.py'):
        return text
    if name not in dict(MOVES) and 'observer' not in text:
        return text
    bom = '\ufeff' if text.startswith('\ufeff') else ''
    text = text.removeprefix(bom) if bom else text
    module_pattern = '|'.join(re.escape(old) for old in sorted(MODULE_MAP, key=len, reverse=True))
    text = re.sub(r'(?:' + module_pattern + r')(?![\w])', lambda match: MODULE_MAP[match.group()], text)
    tree = ast.parse(text)
    package = name[:-3].replace('/', '.').rsplit('.', 1)[0]
    edits = []
    lines = text.splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    def position(line, byte_column):
        return offsets[line-1] + len(lines[line-1].encode('utf-8')[:byte_column].decode('utf-8'))
    for node in ast.walk(tree):
        if not isinstance(node, ast.ImportFrom):
            continue
        original = resolve_name('.' * node.level + (node.module or ''), package) if node.level else node.module
        start, end = position(node.lineno, node.col_offset), position(node.end_lineno, node.end_col_offset)
        statement = text[start:end]
        if original in MODULE_MAP:
            replacement = re.sub(r'^from\s+[\w.]+\s+import', 'from ' + MODULE_MAP[original] + ' import', statement, count=1)
            edits.append((start, end, replacement))
        elif original == 'tools.observability.observer':
            if all(original + '.' + alias.name in MODULE_MAP for alias in node.names):
                parts = []
                for alias in node.names:
                    new = MODULE_MAP[original + '.' + alias.name]
                    parent, leaf = new.rsplit('.', 1)
                    parts.append('from ' + parent + ' import ' + leaf + (' as ' + alias.asname if alias.asname else ''))
                edits.append((start, end, ('\n' + ' '*node.col_offset).join(parts)))
    for start, end, replacement in sorted(edits, reverse=True):
        text = text[:start] + replacement + text[end:]
    return bom + text


def semantic_digest(text):
    tree = ast.parse(text)
    class Normalize(ast.NodeTransformer):
        def visit_Import(self, node):
            return None
        def visit_ImportFrom(self, node):
            return None
        def visit_Assign(self, node):
            if any(isinstance(target, ast.Name) and target.id in ('ROOT', 'root') for target in node.targets) and '__file__' in ast.unparse(node.value):
                node.value = ast.Constant(value='CHECKOUT_ROOT')
            return self.generic_visit(node)
    return safety.sha(ast.dump(Normalize().visit(tree), include_attributes=False).encode('utf-8'))


def plan(root, git):
    if safety.bounded(root, MANIFEST).exists():
        raise RuntimeError('Observer migration already applied; do not replay')
    names = subprocess.check_output([git, '-c', 'core.fsmonitor=false', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=root).decode('utf-8').split('\0')
    moved = dict(MOVES)
    for old, new in MOVES:
        if not safety.bounded(root, old).is_file() or safety.bounded(root, new).exists():
            raise RuntimeError('Missing Observer source or destination conflict: ' + old)
    files = {}
    semantic = {}
    for name in sorted(set(names) | set(moved)):
        if not name or name.startswith(safety.PROTECTED) or safety.private_source_name(name):
            continue
        if name.startswith('docs/architecture/source-') or name.startswith('tests/project/fixtures/'):
            continue
        path = safety.bounded(root, name)
        if not path.is_file() or path.suffix not in safety.TEXT_SUFFIXES:
            continue
        if name.split('/')[0] not in ('backend', 'tests', 'scripts', 'tools', 'examples', 'benchmarks', 'docs') and name not in ('CMakeLists.txt', 'README.md'):
            continue
        before = path.read_bytes()
        text = before.decode('utf-8')
        text = import_rewrite(text, name)
        for old, new in MOVES:
            text = text.replace(old, new)
        if name in moved:
            text = text.replace('Path(__file__).resolve().parents[3]', 'Path(__file__).resolve().parents[4]')
            semantic[moved[name]] = semantic_digest(before.decode('utf-8'))
            if semantic_digest(text) != semantic[moved[name]]:
                raise RuntimeError('Observer behavior AST changed: ' + name)
        after = text.encode('utf-8')
        if before != after or name in moved:
            files[name] = {'before': before, 'after': after, 'destination': moved.get(name, name)}
    generated = {f'{PREFIX}/{group}/__init__.py': b'"""Internal Observer responsibility package; no startup side effects."""\n' for group in GROUPS}
    # Preserve the existing public CLI as a thin, explicit compatibility entry.
    generated[PREFIX + '/server.py'] = b'"""Compatibility CLI; implementation lives in observer.http.server."""\nfrom .http.server import ObserverServer, main\n\nif __name__ == "__main__":\n    main()\n'
    for name in generated:
        if name not in moved and safety.bounded(root, name).exists():
            raise RuntimeError('Generated Observer destination exists: ' + name)
    doc = {'schema': 'observer-responsibility-layout-v1', 'date': '2026-11-27', 'moves': MOVES,
           'behaviorChangeIntended': False, 'frozenCoreUnchanged': True, 'historicalEvidenceRewritten': False,
           'semanticSha256': semantic, 'compatibilityEntrypoints': [PREFIX + '/server.py'],
           'files': [{'old': name, 'new': entry['destination'], 'beforeSha256': safety.sha(entry['before']),
                      'afterSha256': safety.sha(entry['after'])} for name, entry in sorted(files.items())],
           'generated': [{'path': name, 'sha256': safety.sha(data)} for name, data in sorted(generated.items())]}
    return files, generated, doc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--git', default='git')
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--backup', type=Path)
    parser.add_argument('--expected-plan-sha256')
    args = parser.parse_args()
    files, generated, doc = plan(ROOT, args.git)
    digest = safety.sha(json.dumps(doc, sort_keys=True).encode())
    print(json.dumps({'planSha256': digest, 'movedModules': len(MOVES), 'existingFiles': len(files), 'behaviorAstMatches': len(doc['semanticSha256'])}))
    if not args.apply:
        return
    if args.backup is None:
        parser.error('--apply requires --backup')
    safety.apply(ROOT, args.backup, args.expected_plan_sha256, files, generated, doc, moves=MOVES, manifest=MANIFEST)
    print('OBSERVER_LAYOUT_APPLIED_WITH_VERIFIED_BACKUP')


if __name__ == '__main__':
    main()
