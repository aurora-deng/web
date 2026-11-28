"""Move non-AI Phase 11 operations into Function3 responsibility folders.

Default is a read-only plan. Applying requires a reviewed digest and a new
verified backup. It never starts a service, connects to PostgreSQL, or reads
secret files.
"""
import argparse
import ast
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('phase11_layout_safety', Path(__file__).with_name('migrate_application_layout.py'))
safety = importlib.util.module_from_spec(spec)
spec.loader.exec_module(safety)
MANIFEST = 'docs/architecture/source-phase11-operations-migration.json'
MOVES = [
    ('scripts/phase11/postgres-backup.sh', 'scripts/function3/database/backup.sh'),
    ('scripts/phase11/postgres-migrate.sh', 'scripts/function3/database/migrate.sh'),
    ('scripts/phase11/postgres-migration-status.sh', 'scripts/function3/database/migration_status.sh'),
    ('scripts/phase11/postgres-user-service.sh', 'scripts/function3/database/user_service.sh'),
    ('scripts/phase11/seed-demo-users.py', 'scripts/function3/data/seed_demo_users.py'),
    ('scripts/phase11/phase16-soak.py', 'scripts/function3/validation/phase16_soak.py'),
    ('scripts/phase11/webserver-user-service.sh', 'scripts/function3/runtime/webserver/service.sh'),
    ('scripts/phase11/webserver-user-supervisor.sh', 'scripts/function3/runtime/webserver/supervisor.sh'),
]
MODULE_MAP = {
    old[:-3].replace('/', '.'): new[:-3].replace('/', '.')
    for old, new in MOVES if old.endswith('.py')
}
REFERENCE_REWRITES = {
    'CMakeLists.txt': {
        'scripts/phase11/webserver-user-supervisor.sh': 'scripts/function3/runtime/webserver/supervisor.sh',
    },
    'scripts/function3/runtime/service.py': {
        'scripts/phase11/postgres-user-service.sh': 'scripts/function3/database/user_service.sh',
    },
    'scripts/phase15-business-benchmark.sh': {
        'scripts/phase11/webserver-user-service.sh': 'scripts/function3/runtime/webserver/service.sh',
        'scripts/phase11/webserver-user-supervisor.sh': 'scripts/function3/runtime/webserver/supervisor.sh',
    },
    'scripts/README.md': {
        'scripts/phase11/seed-demo-users.py': 'scripts/function3/data/seed_demo_users.py',
    },
}


def module_ast_digest(text):
    tree = ast.parse(text)
    class Normalize(ast.NodeTransformer):
        def visit_Import(self, node):
            return None
        def visit_ImportFrom(self, node):
            return None
    return safety.sha(ast.dump(Normalize().visit(tree), include_attributes=False).encode('utf-8'))


def rewrite_moved_content(text, old, new):
    if old.endswith('.py'):
        return text
    if old.endswith('postgres-migrate.sh') or old.endswith('postgres-migration-status.sh'):
        text = text.replace('dirname "${BASH_SOURCE[0]}")/../..', 'dirname "${BASH_SOURCE[0]}")/../../..')
    if old.endswith('webserver-user-service.sh'):
        text = text.replace('dirname -- "${BASH_SOURCE[0]}")/../..', 'dirname -- "${BASH_SOURCE[0]}")/../../../..')
        text = text.replace('project_root=\"${PHASE11_PROJECT_ROOT:-$(cd -- \"${script_dir}/../..\" && pwd)}\"', 'project_root=\"${PHASE11_PROJECT_ROOT:-$(cd -- \"${script_dir}/../../../..\" && pwd)}\"')
    if old.endswith('webserver-user-supervisor.sh'):
        text = text.replace('service_script="${PHASE11_SERVICE_SCRIPT:-${script_dir}/webserver-user-service.sh}"', 'service_script="${PHASE11_SERVICE_SCRIPT:-${script_dir}/service.sh}"')
    return text


def python_compatibility(module):
    return ("""#!/usr/bin/env python3
\"\"\"Compatibility entrypoint; implementation: %s.\"\"\"
from pathlib import Path
import sys
from importlib import import_module
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
_implementation = import_module(%r)
for _name in dir(_implementation):
    if not _name.startswith(\"__\"): globals()[_name] = getattr(_implementation, _name)
if __name__ == \"__main__\":
    raise SystemExit(_implementation.main())
""") % (module, module)


def shell_compatibility(target):
    return """#!/usr/bin/env bash
# Compatibility entrypoint; implementation: %s
set -euo pipefail
script_dir=\"$(cd -- \"$(dirname -- \"${BASH_SOURCE[0]}\")\" && pwd)\"
exec bash \"${script_dir}/../function3/%s\" \"$@\"
""" % (target, target.split('scripts/function3/', 1)[1])


def plan(root, git):
    if safety.bounded(root, MANIFEST).exists():
        raise RuntimeError('Phase 11 operations migration already applied; do not replay')
    names = subprocess.check_output(
        [git, '-c', 'core.fsmonitor=false', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'],
        cwd=root).decode('utf-8').split('\0')
    moved = dict(MOVES)
    files, generated, semantic, shell_hashes = {}, {}, {}, {}
    for old, new in MOVES:
        if not safety.bounded(root, old).is_file() or safety.bounded(root, new).exists():
            raise RuntimeError('Missing source or destination conflict: ' + old)
    for name in sorted(set(names) | set(moved) | set(REFERENCE_REWRITES)):
        if not name or name.startswith(safety.PROTECTED) or safety.private_source_name(name):
            continue
        if name not in moved and name not in REFERENCE_REWRITES:
            continue
        if not name.endswith(('.py', '.sh', '.md', '.txt', '.cmake')) and name != 'CMakeLists.txt':
            continue
        path = safety.bounded(root, name)
        if not path.is_file():
            continue
        before = path.read_bytes()
        text = before.decode('utf-8')
        if name in moved:
            transformed = rewrite_moved_content(text, name, moved[name])
            if name.endswith('.py'):
                if module_ast_digest(transformed) != module_ast_digest(text):
                    raise RuntimeError('Python behavior changed during path move: ' + name)
                semantic[moved[name]] = module_ast_digest(text)
            else:
                shell_hashes[moved[name]] = {'beforeSha256': safety.sha(before), 'afterSha256': safety.sha(transformed.encode('utf-8'))}
            text = transformed
            if name.endswith('.py'):
                generated[name] = python_compatibility(MODULE_MAP[name[:-3].replace('/', '.')]).encode('utf-8')
            else:
                generated[name] = shell_compatibility(moved[name]).encode('utf-8')
        elif name in REFERENCE_REWRITES:
            for old_ref, new_ref in REFERENCE_REWRITES[name].items():
                text = text.replace(old_ref, new_ref)
        after = text.encode('utf-8')
        if before != after or name in moved:
            files[name] = {'before': before, 'after': after, 'destination': moved.get(name, name)}
    readmes = {
        'scripts/phase11/README.md': '# Phase 11 兼容入口\n\n本目录保留旧命令的薄转发入口，实际实现按职责归入 Function3 与 AI 目录。\n\n- `scripts/function3/database/`：PostgreSQL 备份、迁移和用户级服务管理。\n- `scripts/function3/data/`：演示数据初始化。\n- `scripts/function3/runtime/webserver/`：Web 服务启动、停止与监督。\n- `scripts/function3/validation/phase16_soak.py`：隔离环境稳定性验证。\n- `scripts/ai/`：数据生成、训练、导出与工具链。\n\n旧入口只转发参数，不维护第二份实现。CMake 和日常运行脚本使用新的职责路径。执行前阅读对应脚本的帮助与安全要求；迁移本身不会启动服务、修改数据库或安装依赖。\n',
        'scripts/function3/database/README.md': '# PostgreSQL 运维脚本\n\n本目录包含备份、迁移、迁移状态查询和用户级服务管理。使用现有认证机制或本机 pgpass，不在源码、命令示例或日志中保存真实密码。\n\n数据库写入、迁移、服务启停和恢复必须先确认目标实例并获得相应批准。先备份、检查状态，再按脚本帮助执行；目录整理本身不访问数据库。\n',
        'scripts/function3/data/README.md': '# Function3 数据工具\n\n演示用户初始化通过项目现有 HTTP 接口执行，不直接写 PostgreSQL。仅面向明确指定的测试实例，不得向正式环境批量创建账号。\n\n输入参数和运行限制见 `seed_demo_users.py --help`。目录迁移保留原脚本行为，旧 Phase 11 命令继续转发。\n',
        'scripts/function3/runtime/webserver/README.md': '# Web 服务生命周期\n\n`service.sh` 管理 Web 服务启停、就绪检查及所拥有的 PID；`supervisor.sh` 负责持续监督。它们不会因被导入或查看目录而自动运行，也不自动安装 systemd 服务。\n\n启动配置、工作目录和状态文件必须对应同一个实例。正式服务启停、替换和旧版本清理需要单独批准；不得终止不属于本实例的进程。\n',
    }
    for name, content in readmes.items():
        if safety.bounded(root, name).exists():
            raise RuntimeError('Generated README already exists: ' + name)
        generated[name] = content.encode('utf-8')
    doc = {
        'schema': 'phase11-operations-layout-v1', 'date': '2026-11-28',
        'moves': MOVES, 'behaviorChangeIntended': False,
        'pythonSemanticSha256': semantic, 'shellContentSha256': shell_hashes,
        'referenceRewrites': REFERENCE_REWRITES,
        'compatibilityEntrypoints': [old for old, _ in MOVES],
        'historicalEvidenceRewritten': False,
        'files': [{'old': name, 'new': entry['destination'], 'beforeSha256': safety.sha(entry['before']),
                   'afterSha256': safety.sha(entry['after'])} for name, entry in sorted(files.items())],
        'generated': [{'path': name, 'sha256': safety.sha(data)} for name, data in sorted(generated.items())],
    }
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
    print(json.dumps({'planSha256': digest, 'movedScripts': len(MOVES),
                      'modifiedReferences': sum(1 for name in files if name not in dict(MOVES)),
                      'pythonSemanticMatches': len(doc['pythonSemanticSha256'])}))
    if args.apply:
        if args.backup is None:
            parser.error('--apply requires --backup')
        safety.apply(ROOT, args.backup, args.expected_plan_sha256, files, generated, doc,
                     moves=MOVES, manifest=MANIFEST)
        print('PHASE11_OPERATIONS_LAYOUT_APPLIED_WITH_VERIFIED_BACKUP')


if __name__ == '__main__':
    main()
