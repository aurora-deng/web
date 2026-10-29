"""Explicitly approved, fixed Boost headers; never called by CMake or startup.

Only boost/ headers and the distribution license are extracted. The archive and
the authoritative upstream digest are retained outside the Git checkout.
"""
from pathlib import Path
import hashlib
import json
import shutil
import tarfile
import urllib.request
import concurrent.futures
import subprocess

VERSION = '1.92.0'
PREFIX = 'boost_1_92_0/'
DEST = Path(r'E:\虚拟机高级web开发\deps\boost-1.92.0')
URL = f'https://github.com/boostorg/boost/releases/download/boost-{VERSION}/boost-{VERSION}-b2-nodocs.tar.gz'
EXPECTED = 'e2a814b3a158ab482c7a3d330f8bf5a7a8423d258a4e2fb396996e00fcee2111'
SIZE = 73484649
BUDGET = 900 * 1024 * 1024


def main():
    DEST.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(DEST.parent).free < BUDGET:
        raise SystemExit('Need the approved 900 MiB free budget before download')
    DEST.mkdir(exist_ok=True)
    authoritative = urllib.request.urlopen(URL + '.txt', timeout=30).read().decode()
    if EXPECTED not in authoritative:
        raise SystemExit('Upstream digest changed; refusing download')
    archive = DEST / f'boost-{VERSION}-b2-nodocs.tar.gz'
    def digest_file(path):
        with path.open('rb') as data:
            return hashlib.file_digest(data, 'sha256').hexdigest()
    if not archive.exists() or digest_file(archive) != EXPECTED:
        # 有界八路分段：保留已完成分片，重跑不重复下载。不接受忽略 Range 的响应。
        chunk_size = 4 * 1024 * 1024
        def part(index):
            start=index*chunk_size;end=min(SIZE,start+chunk_size)-1
            destination=DEST/f'boost.part{index:02d}'
            if destination.exists() and destination.stat().st_size==end-start+1:return destination
            result=subprocess.run(['curl.exe','--silent','--show-error','--fail','--location',
                '--connect-timeout','15','--max-time','240','--retry','2',
                '--range',f'{start}-{end}','--output',str(destination),
                '--write-out','%{http_code}',URL],capture_output=True,text=True,check=True)
            if result.stdout!='206':raise RuntimeError('Upstream ignored requested range')
            if destination.stat().st_size!=end-start+1:raise RuntimeError('Incomplete range')
            print(f'Boost download part {index+1}/{(SIZE+chunk_size-1)//chunk_size}',flush=True)
            return destination
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as workers:
            parts=list(workers.map(part,range((SIZE+chunk_size-1)//chunk_size)))
        with archive.open('wb') as output:
            for path in parts:
                with path.open('rb') as data:shutil.copyfileobj(data,output)
    digest = digest_file(archive)
    if digest != EXPECTED:
        raise SystemExit('Upstream SHA-256 mismatch; nothing extracted')
    unpacked = 0
    with tarfile.open(archive) as source:
        for member in source:
            relative = member.name.partition('/')[2]
            if not (relative.startswith('boost/') or relative == 'LICENSE_1_0.txt'):
                continue
            target = (DEST / relative).resolve()
            if not target.is_relative_to(DEST.resolve()):
                raise RuntimeError('Unsafe archive path')
            if not member.isfile():
                continue
            unpacked += member.size
            if unpacked + archive.stat().st_size > BUDGET:
                raise RuntimeError('Extraction exceeds approved disk budget')
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as data, target.open('wb') as output:
                shutil.copyfileobj(data, output)
    (DEST / 'DOWNLOAD_MANIFEST.json').write_text(json.dumps({
        'version': VERSION, 'url': URL, 'sha256': digest,
        'archiveBytes': archive.stat().st_size, 'headersBytes': unpacked,
        'license': 'BSL-1.0', 'scope': 'boost headers and license only',
    }, indent=2), encoding='utf-8')
    print(f'Verified Boost {VERSION}: {DEST}; headers {unpacked} bytes')


if __name__ == '__main__':
    main()
