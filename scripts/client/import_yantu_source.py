"""Snapshot the owned Yantu source and bundled UI libraries, without user data."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil


ROOT = Path(__file__).resolve().parents[2] / 'client' / 'yantu' / 'runtime'
PYTHON = '''
activity_classification assistant_drafts assistant_jobs assistant_service backup_destinations
data_guard desktop_runtime exam_service extract_attachment library_sources login_startup
media_tools mood_wall notifications obsidian_bridge planning_service privacy_ingress
private_connection reading_export remote_access resource_backup restore_planner server
subject_tools workspace_service
'''.split()
VENDORS = '''
css webfonts jszip-3.10.1 marked-17.0.5 cytoscape-3.34.3 katex-0.19.0
ts-fsrs-5.4.2 dompurify-3.4.17 pdfjs-5.6.205
'''.split()
VENDOR_EXTENSIONS = {'.js', '.mjs', '.css', '.woff', '.woff2', '.ttf', '.pfb',
                     '.bcmap', '.icc', '.wasm', '.svg', '.gif'}


def approved_files(source):
    yield from (Path(name + '.py') for name in PYTHON)
    yield from map(Path, ('ocr_attachment.ps1', 'requirements-push.txt'))
    yield from (path.relative_to(source) for path in sorted((source / 'target').iterdir())
                if path.is_file() and path.suffix in {'.js', '.css', '.html', '.webmanifest'})
    yield from (Path('target/icons') / name for name in
                ('icon-192.png', 'icon-512.png', 'study-pet.png', 'mood-stationery.png'))
    vendor_root = source / 'target' / 'vendor'
    yield from (Path('target/vendor') / name for name in
                ('chart.umd.min.js', 'lz-string.min.js', 'email.min.js'))
    for package in VENDORS:
        for path in sorted((vendor_root / package).rglob('*')):
            if path.is_file() and (path.suffix in VENDOR_EXTENSIONS or path.name.startswith('LICENSE')):
                yield path.relative_to(source)


def write_index(source, destination):
    html = source.read_text(encoding='utf-8-sig')
    for name in ('DEFAULT_PHASES', 'DEFAULT_SCHOOLS'):
        html, matches = re.subn(rf'const {name} = \[.*?\n        \];', f'const {name} = [];', html, flags=re.S)
        if matches != 1:
            raise ValueError(f'Review changed Yantu template in {name}')
    html, matches = re.subn(r'(?m)^(\s*const EMAILJS_[A-Z_]+\s*=)[^\r\n]+', r"\1 '';", html)
    if matches != 4:
        raise ValueError('Review changed mail settings')
    html = html.replace('examYear: 2027,', 'examYear: new Date().getFullYear() + 1,')
    destination.write_text(html, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    if ROOT.exists():
        parser.error('Use a new checkout or an empty snapshot location before importing')
    paths = sorted(set(approved_files(source)))
    for path in paths:
        original = source / path
        if not original.is_file() or original.resolve() != original or original.is_symlink():
            parser.error(f'Unexpected pathname in source allowlist: {path}')
    checksums = {}
    for path in paths:
        original, destination = source / path, ROOT / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        if path == Path('target/index.html'):
            write_index(original, destination)
        else:
            shutil.copyfile(original, destination)
        checksums[path.as_posix()] = hashlib.sha256(destination.read_bytes()).hexdigest()
    catalog = ROOT / 'target/data/school-catalog.json'
    catalog.parent.mkdir(parents=True)
    catalog.write_text('{"format":"yantu-school-catalog-v1","schools":[]}\n', encoding='utf-8')
    (ROOT.parent / 'source-manifest.json').write_text(
        json.dumps({'schemaVersion': 1, 'files': checksums}, indent=2) + '\n', encoding='utf-8')
    print(f'Imported {len(paths)} source and UI files, with empty workspace data.')


if __name__ == '__main__':
    main()
