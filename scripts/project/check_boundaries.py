"""Executable Phase43-47 architecture checks, independent of the C++ toolchain."""
import hashlib
import json
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[2]
def main():
    expected=json.loads((ROOT/'docs/architecture/communication/frozen-core-sha256.json').read_text())['files']
    current={p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes().replace(b"\r\n",b"\n")).hexdigest() for p in (ROOT/'backend/server').rglob('*') if p.is_file()}
    assert current==expected,'Frozen communication core changed; an explicit unfreeze decision is required'
    for layer in ('domain','ports','application'):
        for p in (ROOT/'backend/application/modules/chat'/layer).rglob('*'):
            if p.suffix not in ('.h','.cpp'):continue
            for include in re.findall(r'#include\s*[<"]([^>"\n]+)',p.read_text(encoding='utf-8')):
                assert not any(x in include for x in ('backend/server/','libpq','onnxruntime','grpcpp','boost/')),f'illegal chat {layer} dependency: {p}: {include}'
    print('PASS frozen communication core (152 files) and chat domain/port/application dependency boundaries')
if __name__=='__main__':main()
