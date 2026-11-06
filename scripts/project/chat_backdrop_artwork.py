"""Package the local JPEG into a versioned native module; never downloads assets.

FrontendAssets currently exposes HTML/CSS/JS only. Keeping artwork in a generated
data module lets the existing server load it without changing its binary/routes.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[2]
COMPONENT=ROOT/'frontend/phase11/chat/ChatBackdrop'

def dimensions(data):
    assert data[:2]==b'\xff\xd8' and data[-2:]==b'\xff\xd9','Expected a complete JPEG'
    offset=2
    while offset<len(data):
        assert data[offset]==255,'Invalid JPEG segment'
        while data[offset]==255:offset+=1
        marker=data[offset];offset+=1
        length=struct.unpack_from('>H',data,offset)[0]
        if marker in (0xc0,0xc1,0xc2,0xc3,0xc5,0xc6,0xc7,0xc9,0xca,0xcb,0xcd,0xce,0xcf):
            height,width=struct.unpack_from('>HH',data,offset+3)
            return width,height
        assert length>=2,'Invalid JPEG segment length'
        offset+=length
    raise ValueError('JPEG dimensions missing')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--check',action='store_true');args=p.parse_args()
    sources=list(COMPONENT.glob('sakura-lake-*.jpg'));assert len(sources)==1,'Choose exactly one source artwork'
    source=sources[0];data=source.read_bytes();assert len(data)<512*1024,'Artwork exceeds the download budget'
    digest=hashlib.sha256(data).hexdigest();assert source.stem.endswith(digest[:12]),'Stale artwork hash filename'
    width,height=dimensions(data)
    asset={'width':width,'height':height,'sha256':digest,'uri':'data:image/jpeg;base64,'+base64.b64encode(data).decode()}
    output='/** Generated artwork data. Edit the JPEG source and run scripts/project/chat_backdrop_artwork.py. */\nexport const CHAT_BACKDROP_ARTWORK = Object.freeze('+json.dumps(asset,indent=2)+');\n'
    assert len(output.encode())<512*1024,'Generated artwork module exceeds 512 KiB'
    target=COMPONENT/'ChatBackdropArtwork.js'
    if args.check:
        assert target.is_file() and target.read_text(encoding='utf-8')==output,'Stale artwork module; run scripts/project/chat_backdrop_artwork.py'
    else:target.write_text(output,encoding='utf-8')
    print(json.dumps({'sourceBytes':len(data),'moduleBytes':len(output.encode()),'width':width,'height':height,'sha256':digest,'passed':True}))

if __name__=='__main__':main()
