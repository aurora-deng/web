"""Fetch only the separately approved r186 browser files; never run during CMake.

Each Git blob was read from the r186 tag. Verify Git's content hash before writing.
This script installs no packages and does not modify PATH or system settings.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import urllib.request

FILES = {
    'three.module.js': 'c52087acfabae80efabb8cc05e3a1d96e35a1426',
    'three.core.js': '0a1d6cb9ef3cabc97a50bd02394c85e8d1c513c2',
    'OrbitControls.js': '3b49ec2db3aae21cc7c9fa1df38d2c703e28b324',
    'LICENSE': '8ada2a5f982916b0ba4b7a0aa7de347587e745d7',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--allow-download', action='store_true')
    args = parser.parse_args()
    if not args.allow_download:
        parser.error('separate user approval and --allow-download are required')
    args.destination.mkdir(parents=True, exist_ok=True)
    manifest = {'version': 'r186', 'license': 'MIT', 'files': {}}
    for name, expected in FILES.items():
        url = 'https://api.github.com/repos/mrdoob/three.js/git/blobs/' + expected
        request = urllib.request.Request(url, headers={'User-Agent': 'Function3-approved-dependency'})
        with urllib.request.urlopen(request, timeout=40) as response:
            raw = response.read(6 * 1024 * 1024 + 1)
        if len(raw) > 6 * 1024 * 1024:
            raise RuntimeError('download exceeded approved size budget')
        result = json.loads(raw)
        if result.get('encoding') != 'base64':
            raise RuntimeError('unexpected GitHub blob encoding')
        data = base64.b64decode(result['content'])
        actual = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if actual != expected:
            raise RuntimeError('Git blob hash mismatch: ' + name)
        target = args.destination / name
        if target.exists() and target.read_bytes() != data:
            raise RuntimeError('refusing to overwrite a different dependency: ' + name)
        target.write_bytes(data)
        manifest['files'][name] = {'source': url, 'gitBlob': expected,
                                  'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data)}
        print('Verified', name, len(data), 'bytes')
    (args.destination / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')

if __name__ == '__main__':
    main()
