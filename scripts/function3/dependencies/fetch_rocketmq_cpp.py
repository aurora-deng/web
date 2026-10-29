"""Fetch the approved SDK + pinned protocol sources, without build-time downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

SOURCES = {
    'rocketmq-clients': ('apache/rocketmq-clients', 'da8271b572b41b627bcbbdfb5b3310c0c5e4736b'),
    'rocketmq-apis': ('apache/rocketmq-apis', 'bc8e1845a8b0a1b5ec828690324401a323244354'),
}
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--destination', type=Path, required=True)
parser.add_argument('--allow-download', action='store_true')
args = parser.parse_args()
if not args.allow_download:
    parser.error('separate user approval and --allow-download required')
args.destination.mkdir(parents=True, exist_ok=True)
manifest = {'sdkVersion': 'cpp-5.1.1', 'license': 'Apache-2.0', 'sources': {}}
for name, (repository, commit) in SOURCES.items():
    url = f'https://api.github.com/repos/{repository}/tarball/{commit}'
    request = urllib.request.Request(url, headers={'User-Agent': 'Function3-approved-dependency'})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read(30 * 1024 * 1024 + 1)
    if len(data) > 30 * 1024 * 1024:
        raise RuntimeError('archive size exceeds budget')
    target = args.destination / (name + '.tar.gz')
    if target.exists() and target.read_bytes() != data:
        raise RuntimeError('refusing to replace different archive')
    target.write_bytes(data)
    manifest['sources'][name] = {'url': url, 'commit': commit, 'bytes': len(data),
                               'sha256': hashlib.sha256(data).hexdigest()}
    print('Retrieved pinned source:', name, len(data), 'bytes')
(args.destination / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
