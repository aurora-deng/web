"""Resume the approved Kokoro asset in four bounded ranges, checking official SHA-256."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import time
import urllib.request

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--destination',type=Path,required=True)
p.add_argument('--allow-download',action='store_true')
a=p.parse_args()
if not a.allow_download:p.error('explicit separate user permission required')
name='kokoro-int8-multi-lang-v1_1.tar.bz2'
size=147031220
expected='a1e94694776049035c4f2c6529f003aaece993c76aae9a78995831c3c4dcafc6'
official='https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/assets/265069793?download=1'
destination=a.destination;destination.mkdir(parents=True,exist_ok=True)
headers={'User-Agent':'Function3-approved-artifacts','Accept':'application/octet-stream'}
with urllib.request.urlopen(urllib.request.Request(official,headers=headers),timeout=45) as response:
    signed=response.geturl() # 官方临时地址只保留在内存，不写入清单和日志。
parts=destination/'kokoro-blocks';parts.mkdir(exist_ok=True)
chunk=1024*1024
partial=destination/(name+'.partial')
if partial.exists():
    with partial.open('rb') as old:
        for index in range(partial.stat().st_size//chunk):
            block=old.read(chunk);target=parts/str(index)
            if not target.exists():target.write_bytes(block)
def block(index):
    start=index*chunk;end=min(size,start+chunk)-1;target=parts/str(index)
    if target.exists() and target.stat().st_size==end-start+1:return target
    for attempt in range(5):
        try:
            request=urllib.request.Request(signed,headers={'Range':f'bytes={start}-{end}','User-Agent':headers['User-Agent']})
            with urllib.request.urlopen(request,timeout=45) as response:
                if response.status!=206:raise RuntimeError('range response required')
                data=response.read(end-start+1)
            if len(data)!=end-start+1:raise RuntimeError('truncated range')
            target.write_bytes(data)
            return target
        except Exception:
            if attempt==4:raise
            time.sleep(1+attempt)
count=(size+chunk-1)//chunk
with ThreadPoolExecutor(max_workers=4) as pool:
    for i,result in enumerate(pool.map(block,range(count))):
        if i%10==0:print('Verified blocks',i+1,'/',count,flush=True)
target=destination/name;digest=hashlib.sha256()
with target.with_suffix('.assembling').open('wb') as output:
    for index in range(count):
        data=(parts/str(index)).read_bytes();digest.update(data);output.write(data)
if digest.hexdigest()!=expected:raise RuntimeError('publisher SHA-256 mismatch; assembled file not installed')
target.with_suffix('.assembling').rename(target)
(destination/'KOKORO_MANIFEST.json').write_text(json.dumps({'name':name,'bytes':size,'sha256':expected,'publisherAssetId':265069793},indent=2)+'\n')
print('Verified approved Kokoro model',size,expected,flush=True)
