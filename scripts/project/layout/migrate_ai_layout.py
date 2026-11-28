"""Explicit AI C++ layout migration. Default is read-only planning.

--apply requires current approval for the printed scope and an existing empty
backup destination in this checkout's evidence parent. No Git/runtime actions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[3]
MOVES = [
    ('backend/ai/models/ModelProvider', 'backend/ai/inference/contract/ModelProvider'),
    ('backend/ai/models/InProcessOnnxModelProvider', 'backend/ai/inference/inprocess/InProcessOnnxModelProvider'),
    *[(f'backend/ai/models/{name}', f'backend/ai/inference/remote/{name}') for name in
      ('GrpcOnnxModelProvider', 'GrpcOnnxWorker', 'OllamaModelProvider', 'OllamaStreamDecoder')],
    ('backend/ai/models/ResilientModelProvider', 'backend/ai/inference/resilience/ResilientModelProvider'),
    *[(f'backend/ai/models/{name}', f'backend/ai/registry/{name}') for name in
      ('ModelRegistryService', 'ModelEvaluationService', 'DeterministicExactScorer')],
    *[(f'backend/ai/models/{name}', f'backend/ai/rag/{name}') for name in
      ('RagService', 'DeterministicKnowledgeIndex')],
    ('backend/ai/bootstrap/KnowledgeRuntime', 'backend/ai/rag/KnowledgeRuntime'),
    *[(f'backend/ai/training/{name}', f'backend/ai/training/data/{name}') for name in
      ('TrainingDataService', 'JsonlTrainingDataExporter')],
]


def bounded(path):
    if not path.resolve().is_relative_to(ROOT.resolve()) or path.is_symlink():
        raise RuntimeError('Migration target outside checkout or symlink')
    return path


def sha(content):
    return hashlib.sha256(content).hexdigest()


def plan():
    manifest = ROOT / 'docs/architecture/source-layout-migration-2.json'
    if manifest.exists():
        raise RuntimeError('Migration already applied; do not replay')
    moved = set()
    for old, new in MOVES:
        source, target = bounded(ROOT / old), bounded(ROOT / new)
        if not source.is_dir() or target.exists():
            raise RuntimeError('Missing source or conflicting target: ' + old)
        for path in source.rglob('*'):
            bounded(path)
            if path.is_file():
                moved.add(path.relative_to(ROOT).as_posix())
    names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
    files = {}
    for name in names:
        if not name:
            continue
        path = ROOT / name
        parts = Path(name).parts
        if parts[0] not in ('backend', 'tests', 'scripts', 'tools', 'examples', 'benchmarks') and name != 'CMakeLists.txt':
            continue
        if name.startswith('backend/server/') or name.startswith('scripts/project/layout/'):
            continue
        if path.suffix not in ('.h', '.cpp', '.c', '.py', '.cmake', '.txt', '.json', '.sh'):
            continue
        bounded(path)
        before = path.read_bytes()
        try:
            text = before.decode('utf-8')
        except UnicodeError:
            continue
        after = text
        for old, new in MOVES:
            after = after.replace(old + '/', new + '/')
        if after != text or name in moved:
            files[name] = (before, after.encode('utf-8'))
    for name in moved:
        if name not in files:
            content = bounded(ROOT / name).read_bytes()
            files[name] = (content, content)
    document = {'schema': 'ai-cpp-layout-v1', 'date': '2026-11-27', 'moves': MOVES,
                'frozenCoreUnchanged': True, 'historicalEvidenceRewritten': False,
                'files': [{'old': name, 'beforeSha256': sha(before), 'afterSha256': sha(after)}
                          for name, (before, after) in sorted(files.items())]}
    return files, document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--backup', type=Path)
    parser.add_argument('--expected-plan-sha256')
    args = parser.parse_args()
    files, document = plan()
    digest = sha(json.dumps(document, sort_keys=True).encode())
    print(json.dumps({'directories': len(MOVES), 'files': len(files), 'planSha256': digest}))
    if not args.apply:
        return
    if digest != args.expected_plan_sha256:
        raise RuntimeError('Plan changed; review the new scope before applying')
    backup = args.backup
    if backup is None or backup.exists() or backup.is_symlink() or backup.resolve().parent != ROOT.parent.resolve():
        raise RuntimeError('New backup archive must be in the evidence parent, not source/runtime')
    # Complete backup and verification precede every modification/move.
    with tarfile.open(backup, 'x:gz') as archive:
        for name in sorted(files):
            archive.add(ROOT / name, arcname=name, recursive=False)
    with tarfile.open(backup) as archive:
        for name, (before, _) in files.items():
            assert archive.extractfile(name).read() == before
    for name, (before, after) in files.items():
        path = bounded(ROOT / name)
        if path.read_bytes() != before:
            raise RuntimeError('Concurrent change detected; refusing overwrite: ' + name)
    for name, (before, after) in files.items():
        if before != after:
            bounded(ROOT / name).write_bytes(after)
    for old, new in MOVES:
        source, target = bounded(ROOT / old), bounded(ROOT / new)
        target.parent.mkdir(parents=True, exist_ok=True)
        source.rename(target)
    (ROOT / 'docs/architecture/source-layout-migration-2.json').write_text(
        json.dumps(document, indent=2) + '\n', encoding='utf-8')
    print('AI_CPP_LAYOUT_APPLIED_WITH_VERIFIED_BACKUP')


if __name__ == '__main__':
    main()
