"""Group monitoring tool implementations; preserve public CLI identities.

Default is read-only planning. Applying requires an exact reviewed plan and a
new verified backup. Never starts labs, installers, listeners or remote calls.
"""
import argparse
import ast
import importlib.util
from importlib.util import resolve_name
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('tool_layout_safety', Path(__file__).with_name('migrate_application_layout.py'))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
PREFIX = 'tools/observability'
MANIFEST = 'docs/architecture/source-observability-tools-migration.json'
GROUPS = {
    'gateways': ['gateway'],
    'laboratory': ['lab', 'lab_config', 'isolated_dual', 'isolated_gateway',
                   'scenarios', 'probe', 'alert_lab', 'alert_receiver', 'resource_lab', 'preview'],
    'pipeline': ['install_metrics_backend', 'install_pipeline'],
    'diagnostics': ['ack_diagnosis', 'function_trace', 'experiments', 'profile_formats'],
    'verification': ['verify', 'verify_alert_policy', 'verify_dual_chat', 'verify_gateway', 'verify_metrics'],
    'lifecycle': ['retention'],
}
MOVES = [(f'{PREFIX}/{name}.py', f'{PREFIX}/{group}/{name}.py')
         for group, names in GROUPS.items() for name in names]
MODULE_MAP = {old[:-3].replace('/', '.'): new[:-3].replace('/', '.') for old, new in MOVES}


def import_rewrite(text, name):
    """Rewrite import statements only, never process-identity/CLI literals."""
    bom = '\ufeff' if text.startswith('\ufeff') else ''
    text = text.removeprefix(bom) if bom else text
    tree = ast.parse(text)
    package = name[:-3].replace('/', '.').rsplit('.', 1)[0]
    lines = text.splitlines(keepends=True)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    def position(line, byte_column):
        return offsets[line-1] + len(lines[line-1].encode('utf-8')[:byte_column].decode('utf-8'))
    edits = []
    for node in ast.walk(tree):
        replacement = None
        if isinstance(node, ast.ImportFrom):
            original = resolve_name('.' * node.level + (node.module or ''), package) if node.level else node.module
            if original in MODULE_MAP:
                replacement = 'from ' + MODULE_MAP[original] + ' import ' + ', '.join(
                    alias.name + (' as ' + alias.asname if alias.asname else '') for alias in node.names)
            elif original == 'tools.observability' and all(original + '.' + alias.name in MODULE_MAP for alias in node.names):
                replacement = ('\n' + ' '*node.col_offset).join(
                    'from ' + MODULE_MAP[original+'.'+alias.name].rsplit('.',1)[0] + ' import '
                    + MODULE_MAP[original+'.'+alias.name].rsplit('.',1)[1]
                    + (' as '+alias.asname if alias.asname else '') for alias in node.names)
        # No current affected module uses bare `import tools.observability.X`;
        # refuse it instead of silently changing Python binding semantics.
        elif isinstance(node, ast.Import) and any(alias.name in MODULE_MAP for alias in node.names):
            raise RuntimeError('Review bare module import: ' + name)
        if replacement is not None:
            edits.append((position(node.lineno,node.col_offset), position(node.end_lineno,node.end_col_offset), replacement))
    for start,end,replacement in sorted(edits,reverse=True):
        text = text[:start] + replacement + text[end:]
    return bom + text


def semantic_digest(text):
    tree = ast.parse(text)
    class Normalize(ast.NodeTransformer):
        def visit_Import(self,node):
            return None
        def visit_ImportFrom(self,node):
            return None
        def visit_Subscript(self,node):
            if ast.unparse(node) in ('Path(__file__).resolve().parents[2]', 'Path(__file__).resolve().parents[3]'):
                return ast.copy_location(ast.Constant(value='CHECKOUT_ROOT'),node)
            return self.generic_visit(node)
    return safety.sha(ast.dump(Normalize().visit(tree),include_attributes=False).encode('utf-8'))


def compatibility_entry(module, cli, exits):
    text = ('"""Compatibility entry only; implementation: ' + module + '."""\n'
            'import sys as _sys\nfrom importlib import import_module as _import_module\n'
            '_implementation = _import_module(' + repr(module) + ')\n')
    if cli:
        text += 'if __name__ == "__main__":\n    ' + ('_sys.exit(_implementation.main())' if exits else '_implementation.main()') + '\nelse:\n    _sys.modules[__name__] = _implementation\n'
    else:
        text += '_sys.modules[__name__] = _implementation\n'
    return text.encode('utf-8')


def plan(root,git):
    if safety.bounded(root,MANIFEST).exists():
        raise RuntimeError('Monitoring tool migration already applied; do not replay')
    moved = dict(MOVES)
    for old,new in MOVES:
        if not safety.bounded(root,old).is_file() or safety.bounded(root,new).exists():
            raise RuntimeError('Missing source or destination conflict: '+old)
    # Only named implementation files and Python import consumers are candidates.
    names = subprocess.check_output([git,'-c','core.fsmonitor=false','ls-files','--cached','--others','--exclude-standard','-z'],cwd=root).decode('utf-8').split('\0')
    files,generated,semantic,entrypoints = {},{},{},[]
    for name in sorted(set(names)|set(moved)):
        if not name or name.startswith(safety.PROTECTED) or safety.private_source_name(name):
            continue
        if not name.startswith(('tests/','tools/','scripts/','backend/')) or not name.endswith('.py'):
            continue
        if name.startswith('tests/project/fixtures/'):
            continue
        path = safety.bounded(root,name)
        if not path.is_file():
            continue
        before = path.read_bytes()
        text = before.decode('utf-8')
        if name not in moved and 'observability' not in text:
            continue
        text = import_rewrite(text,name)
        if name in moved:
            text = text.replace('Path(__file__).resolve().parents[2]','Path(__file__).resolve().parents[3]')
            expected = semantic_digest(before.decode('utf-8'))
            if semantic_digest(text) != expected:
                raise RuntimeError('Tool behavior AST changed: '+name)
            semantic[moved[name]] = expected
            original_ast = ast.parse(before.decode('utf-8'))
            cli = any(isinstance(node,ast.FunctionDef) and node.name=='main' for node in original_ast.body)
            generated[name] = compatibility_entry(MODULE_MAP[name[:-3].replace('/','.')],cli,Path(name).stem in ('isolated_dual','isolated_gateway'))
            if cli:
                entrypoints.append(name)
        after = text.encode('utf-8')
        if before != after or name in moved:
            files[name] = {'before':before,'after':after,'destination':moved.get(name,name)}
    for group in GROUPS:
        name = f'{PREFIX}/{group}/__init__.py'
        if safety.bounded(root,name).exists():
            raise RuntimeError('Generated package already exists: '+name)
        generated[name] = b'"""Monitoring responsibility package; importing does not start tools."""\n'
    doc = {'schema':'observability-tools-responsibility-layout-v1','date':'2026-11-27',
           'moves':MOVES,'semanticSha256':semantic,'behaviorChangeIntended':False,
           'stableCommandEntrypoints':entrypoints,'processIdentityLiteralsPreserved':True,
           'compatibilityImports':list(moved),'historicalEvidenceRewritten':False,
           'files':[{'old':name,'new':entry['destination'],'beforeSha256':safety.sha(entry['before']),
                     'afterSha256':safety.sha(entry['after'])} for name,entry in sorted(files.items())],
           'generated':[{'path':name,'sha256':safety.sha(data)} for name,data in sorted(generated.items())]}
    return files,generated,doc


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--git',default='git')
    parser.add_argument('--apply',action='store_true')
    parser.add_argument('--backup',type=Path)
    parser.add_argument('--expected-plan-sha256')
    args=parser.parse_args()
    files,generated,doc=plan(ROOT,args.git)
    digest=safety.sha(json.dumps(doc,sort_keys=True).encode())
    print(json.dumps({'planSha256':digest,'movedModules':len(MOVES),'modifiedExistingFiles':len(files),
                      'behaviorAstMatches':len(doc['semanticSha256']),'stableClis':len(doc['stableCommandEntrypoints'])}))
    if args.apply:
        if args.backup is None:
            parser.error('--apply requires --backup')
        safety.apply(ROOT,args.backup,args.expected_plan_sha256,files,generated,doc,moves=MOVES,manifest=MANIFEST)
        print('MONITORING_TOOLS_LAYOUT_APPLIED_WITH_VERIFIED_BACKUP')


if __name__ == '__main__':
    main()
