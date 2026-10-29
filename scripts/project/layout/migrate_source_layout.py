"""One-time Phase36 source move; no Git reset, external files or runtime data.

Run only from the reviewed checkout with a source snapshot already saved.
Historical evidence intentionally keeps the paths of the version it measured.
"""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[3]
MOVES = [
    ('server/phase11/ai', 'backend/ai/models'),
    ('server/phase11/training', 'backend/ai/training'),
    ('server/phase11/modules/speech', 'backend/ai/speech'),
    ('server/phase11', 'backend/application'),
    ('server', 'backend/server'),
]
TEXT = {'.h', '.cpp', '.c', '.py', '.js', '.cjs', '.mjs', '.txt', '.md', '.cmake', '.yml', '.yaml', '.json', '.sh'}


def inside(path):
    if not path.resolve().is_relative_to(ROOT.resolve()):
        raise RuntimeError('Move escaped the workspace')
    return path


def main():
    if (ROOT / 'docs/architecture/source-layout-migration.json').exists():
        raise SystemExit('Migration already applied; refusing to rewrite paths twice')
    if not (ROOT / 'build-function3/phase35-before-phase36.tar.gz').is_file():
        raise SystemExit('Save the pre-migration source snapshot first')
    for before, after in MOVES:
        source, target = inside(ROOT / before), inside(ROOT / after)
        if not source.exists():
            continue
        if target.exists():
            raise SystemExit(f'Conflicting destination: {after}')
        target.parent.mkdir(parents=True, exist_ok=True)
        source.rename(target)
    # Ignore generated trees and vendor modules: they are not project source.
    files = [ROOT / 'CMakeLists.txt', ROOT / 'README.md', ROOT / 'main.cpp']
    for name in ('backend', 'tests', 'scripts', 'docs', 'tools', 'examples', 'benchmarks', 'frontend'):
        files.extend((ROOT / name).rglob('*'))
    changed = 0
    for file in files:
        if not file.is_file() or file.suffix not in TEXT or 'vendor' in file.parts:
            continue
        if (ROOT / 'docs/evidence') in file.parents or file == Path(__file__).resolve():
            continue
        try:
            original = file.read_text(encoding='utf-8')
        except UnicodeError:
            continue
        content = original
        for before, after in MOVES:
            content = content.replace(before + '/', after + '/')
        if content != original:
            file.write_text(content, encoding='utf-8', newline='\n')
            changed += 1
    cmake = ROOT / 'backend/application/modules/CMakeLists.txt'
    cmake.write_text(cmake.read_text(encoding='utf-8').replace(
        'speech/', '${PROJECT_SOURCE_DIR}/backend/ai/speech/'), encoding='utf-8')
    manifest = ROOT / 'docs/architecture/source-layout-migration.json'
    manifest.write_text(json.dumps({'phase': 36, 'moves': MOVES,
        'updatedTextFiles': changed, 'historicalEvidenceRewritten': False}, indent=2) + '\n', encoding='utf-8')
    print(f'Moved {len(MOVES)} source roots; updated {changed} text files')


if __name__ == '__main__':
    main()
