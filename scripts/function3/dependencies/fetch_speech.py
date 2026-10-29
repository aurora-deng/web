"""Fetch only explicitly approved Linux TTS/media artifacts; never called by CMake."""
import argparse
import hashlib
import json
import time
from pathlib import Path
import urllib.request

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--destination',type=Path,required=True)
parser.add_argument('--allow-download',action='store_true')
parser.add_argument('--metadata-only',action='store_true')
parser.add_argument('--only',choices=('engine','model','ffmpeg'))
args=parser.parse_args()
headers={'User-Agent':'Function3-approved-artifacts','Connection':'close'}
def read(url):
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url,headers=headers),timeout=60) as response:
                return response.read()
        except Exception:
            if attempt==3: raise
            time.sleep(1+attempt)

assets=[
 {'name':'sherpa-onnx-v1.13.8-linux-x64-shared-lib.tar.bz2','url':'https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/sherpa-onnx-v1.13.8-linux-x64-shared-lib.tar.bz2','bytes':9816899,'digest':'sha256:3892d184be41027e18165e67f549cd4e4cdd8dcd73ac5579e97afd55e14e30b6','assetId':555144064},
 {'name':'kokoro-int8-multi-lang-v1_1.tar.bz2','url':'https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-int8-multi-lang-v1_1.tar.bz2','bytes':147031220,'digest':'sha256:a1e94694776049035c4f2c6529f003aaece993c76aae9a78995831c3c4dcafc6','assetId':265069793},
]
assets.append({'name':'ffmpeg-8.1.3.tar.xz','url':'https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz','bytes':25*1024*1024,'digest':None})
if args.only:
    prefix={'engine':'sherpa-','model':'kokoro-','ffmpeg':'ffmpeg-'}[args.only]
    assets=[asset for asset in assets if asset['name'].startswith(prefix)]
print(json.dumps(assets,indent=2))
if args.metadata_only: raise SystemExit(0)
if not args.allow_download: parser.error('separate user permission and --allow-download required')
args.destination.mkdir(parents=True,exist_ok=True)
for asset in assets:
    target=args.destination/asset['name']
    digest=hashlib.sha256(); count=0
    if target.exists():
        data=target.read_bytes(); digest.update(data); count=len(data)
    else:
        download_url=('https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/assets/'+str(asset['assetId'])+'?download=1') if asset.get('assetId') else asset['url']
        download_headers=dict(headers)
        if asset.get('assetId'): download_headers['Accept']='application/octet-stream'
        request=urllib.request.Request(download_url,headers=download_headers)
        temporary=target.with_suffix(target.suffix+'.partial')
        # 大包使用可恢复的小范围请求，网络中断只重试当前分块。
        if asset.get('assetId'):
            offset=temporary.stat().st_size if temporary.exists() else 0
            while offset<asset['bytes']:
                end=min(offset+1024*1024-1,asset['bytes']-1)
                for attempt in range(5):
                    try:
                        h=dict(download_headers);h['Range']=f'bytes={offset}-{end}'
                        ranged=urllib.request.Request(download_url+'&offset='+str(offset),headers=h)
                        with urllib.request.urlopen(ranged,timeout=40) as response:
                            if offset and response.status!=206:raise RuntimeError('server ignored resume range')
                            data=response.read(end-offset+1)
                            if len(data)!=end-offset+1:raise RuntimeError('incomplete artifact block')
                        with temporary.open('ab') as output:output.write(data)
                        offset+=len(data)
                        print('Download progress',asset['name'],offset,'/',asset['bytes'],flush=True)
                        break
                    except Exception:
                        if attempt==4:raise
                        time.sleep(attempt+1)
            digest.update(temporary.read_bytes());count=temporary.stat().st_size
        else:
            with urllib.request.urlopen(request,timeout=90) as response,temporary.open('wb') as output:
                while data:=response.read(1024*1024):
                    count+=len(data)
                    if count>asset['bytes']: raise RuntimeError('artifact exceeds declared budget')
                    digest.update(data); output.write(data)
        actual='sha256:'+digest.hexdigest()
        if asset['digest'] and actual!=asset['digest']: raise RuntimeError('official digest mismatch')
        temporary.rename(target)
    actual='sha256:'+digest.hexdigest()
    if asset['digest'] and actual!=asset['digest']: raise RuntimeError('cached official digest mismatch')
    asset['actualSha256']=digest.hexdigest(); asset['actualBytes']=count
    print('Verified artifact',asset['name'],count,digest.hexdigest(),flush=True)
(args.destination/'MANIFEST.json').write_text(json.dumps(assets,indent=2)+'\n',encoding='utf-8')
