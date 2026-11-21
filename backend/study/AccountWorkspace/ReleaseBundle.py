"""Build and verify the code-only Yantu Linux staging bundle."""
import argparse
import gzip
import hashlib
from io import BytesIO
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile

from backend.study.AccountWorkspace.FrontendSnapshot import MIME


ROOT = Path(__file__).resolve().parents[3]
RUNTIME = 'client/yantu/runtime/'
FRONT = 'frontend/phase11/'
MANIFEST = 'deployment-manifest.json'
TOOLS = {
    'backend/study/AccountWorkspace/AccountWorkspace.py',
    'backend/study/AccountWorkspace/FrontendSnapshot.py',
    'backend/study/AccountWorkspace/GatewayConfig.py',
    'backend/study/AccountWorkspace/LabRunner.py',
    'backend/study/AccountWorkspace/UploadParts.py',
    'config/gateway/envoy.json',
    'scripts/function3/runtime/seed_study_community.sql',
    'tools/observability/gateway.py',
}
FRONT_ADDITIONS = {
    'accounts/AccountEntryView/AccountEntryView.js', 'shared/AccountSession/AccountSession.js',
    'admin/AdminAccess/AdminAccess.js', 'admin/CommunityTrainingPanel/CommunityTrainingPanel.js',
    'files/AttachmentUpload/AttachmentUpload.js', 'profile/ProfileSettings/ProfileSettings.js',
    'shared/DiscussionThreads/DiscussionThreads.css', 'shared/DiscussionThreads/DiscussionThreads.js',
    'shared/WebAssistantPanel/WebAssistantHistory.js', 'shared/XiaoYan/XiaoYan.js', 'shared/XiaoYan/XiaoYan.png',
    'study/StudyWorkspace/StudyWorkspace.css', 'study/StudyWorkspace/StudyWorkspace.js', 'study/index.html',
    'tree/CategoryRanking/CategoryRanking.js', 'tree/StoryPreview/StoryPreview.css', 'tree/StoryPreview/StoryPreview.js',
    'tree/TrainingConsent/TrainingConsent.js',
}
FRONT_METADATA = {'asset-manifest.json', 'package.json', 'vendor/three-r186/LICENSE', 'vendor/three-r186/MANIFEST.json'}
RUNTIME_ADDITIONS = {'target/nebula-downloads.js', 'target/workspace-context.js', 'target/data/school-catalog.json'}
RUNTIME_TYPES = {'.py', '.ps1', '.html', '.js', '.mjs', '.css', '.webmanifest', '.png', '.woff', '.woff2',
                 '.ttf', '.pfb', '.bcmap', '.icc', '.wasm', '.svg', '.gif'}
MAX_FILE = 12 * 1024 * 1024
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_name(name):
    path = PurePosixPath(name)
    return (name == path.as_posix() and not path.is_absolute() and path.parts and
            all(not part.startswith('.') and '\\' not in part and ':' not in part for part in path.parts))


def read_code(name):
    path = ROOT / name
    if not safe_name(name) or path.resolve(strict=True) != path or not path.is_file() or path.stat().st_size > MAX_FILE:
        raise ValueError(f'部署文件超出预定范围 {name}')
    data = path.read_bytes()
    if re.search(rb'-----BEGIN [A-Z ]*PRIVATE KEY-----', data):
        raise ValueError(f'部署文件中发现私钥标记 {name}')
    return data


def payload():
    imported = json.loads(read_code('client/yantu/source-manifest.json'))['files']
    runtime = set(imported) | RUNTIME_ADDITIONS
    for name in runtime:
        file = PurePosixPath(name)
        allowed = (file.suffix in RUNTIME_TYPES or name in RUNTIME_ADDITIONS or
                   name == 'requirements-push.txt' or (name.startswith('target/vendor/') and file.name.startswith('LICENSE')))
        if not safe_name(name) or not allowed or (name.startswith('target/data/') and name not in RUNTIME_ADDITIONS):
            raise ValueError(f'禁止纳入运行资料 {name}')
    tracked = subprocess.check_output(['git', 'ls-files', '-z', '--', FRONT], cwd=ROOT).decode('utf-8').split('\0')
    frontend = {name.removeprefix(FRONT) for name in tracked if name.startswith(FRONT)
                and PurePosixPath(name).suffix in MIME and PurePosixPath(name).suffix != '.json'}
    frontend |= FRONT_ADDITIONS | FRONT_METADATA
    names = TOOLS | {RUNTIME + name for name in runtime} | {FRONT + name for name in frontend}
    files = {name: read_code(name) for name in sorted(names)}
    catalog = json.loads(files[RUNTIME + 'target/data/school-catalog.json'])
    if catalog != {'format': 'yantu-school-catalog-v1', 'schools': []}:
        raise ValueError('部署用院校目录必须为空')
    index = files[RUNTIME + 'target/index.html']
    for seed in (b'DEFAULT_PHASES', b'DEFAULT_SCHOOLS'):
        if not re.search(rb'const\s+' + seed + rb'\s*=\s*\[\s*\]\s*;', index):
            raise ValueError('部署用研途页面仍含预置学习记录')
    for key in (b'PUBLIC_KEY', b'SERVICE_ID', b'TEMPLATE_TASK', b'TEMPLATE_REVIEW'):
        if not re.search(rb'EMAILJS_' + key + rb"\s*=\s*''", index):
            raise ValueError('部署用研途页面仍含预置邮件配置')
    assets = json.loads(files[FRONT + 'asset-manifest.json'])
    actual = sum(PurePosixPath(name).suffix in {'.html', '.js', '.css'} and
                 'vendor' not in PurePosixPath(name).parts for name in frontend)
    if assets['nativeFiles'] != actual:
        raise ValueError('前端清单与部署允许清单不一致，请审阅新增前端文件')
    if sum(map(len, files.values())) > MAX_ARCHIVE_BYTES:
        raise ValueError('部署文件总量超出预算')
    return files, assets['version']


def verify(archive, files, version):
    with tarfile.open(archive, 'r:gz') as bundle:
        members = bundle.getmembers()
        names = [item.name for item in members]
        expected = {*files, MANIFEST}
        if len(names) != len(expected) or set(names) != expected:
            raise ValueError('部署压缩包含重复、缺失或未获批准的路径')
        if any(not item.isfile() or not safe_name(item.name) or item.size > MAX_FILE for item in members):
            raise ValueError('部署压缩包包含非普通文件或异常路径')
        manifest = json.load(bundle.extractfile(MANIFEST))
        if manifest != receipt(files, version):
            raise ValueError('部署文件清单与核验源文件不符')
        for name, data in files.items():
            if digest(bundle.extractfile(name).read()) != digest(data):
                raise ValueError(f'部署文件校验失败 {name}')
    return manifest


def receipt(files, version):
    return {'schemaVersion': 1, 'product': 'nebula-yantu-code-only', 'frontendVersion': version,
            'files': {name: {'bytes': len(data), 'sha256': digest(data)} for name, data in files.items()}}


def build(archive, files, version):
    manifest = json.dumps(receipt(files, version), indent=2, ensure_ascii=False).encode('utf-8') + b'\n'
    with archive.open('xb') as output, gzip.GzipFile(filename='', fileobj=output, mode='wb', mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode='w') as bundle:
            for name, data in sorted({**files, MANIFEST: manifest}.items()):
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = 0o644
                bundle.addfile(member, BytesIO(data))
    verify(archive, files, version)
    with archive.with_name(archive.name + '.sha256').open('x', encoding='ascii', newline='\n') as checksum:
        checksum.write(f'{digest(archive.read_bytes())}  {archive.name}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('build', 'verify'))
    parser.add_argument('archive', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+\.tar\.gz', args.archive.name):
        parser.error('部署包文件名须为英文、数字、短横线或下划线，扩展名为 .tar.gz')
    try:
        files, version = payload()
        if args.action == 'build':
            build(args.archive, files, version)
        else:
            verify(args.archive, files, version)
        print(json.dumps({'passed': True, 'files': len(files), 'frontendVersion': version,
                          'archiveSha256': digest(args.archive.read_bytes())}))
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f'研途部署包检查未通过：{error}\n')


if __name__ == '__main__':
    main()
