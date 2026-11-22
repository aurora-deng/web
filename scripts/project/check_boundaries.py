"""Executable Phase43-47 architecture checks, independent of the C++ toolchain."""
import hashlib
import json
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[2]

def systems_boundaries():
    workflow = ROOT / 'backend/workflow'
    fleet = ROOT / 'backend/fleet'
    locations = {
        workflow: (workflow,),
        fleet / 'domain': (fleet / 'domain',),
        fleet / 'ports': (fleet / 'ports', fleet / 'domain'),
        fleet / 'application': (fleet / 'application', fleet / 'ports', fleet / 'domain', workflow),
    }
    for layer, allowed in locations.items():
        for source in layer.rglob('*'):
            if source.suffix not in ('.h', '.cpp'):
                continue
            includes = re.findall(r'^\s*#\s*include\s*([<"])([^>"\n]+)',
                                  source.read_text(encoding='utf-8'), re.M)
            for delimiter, header in includes:
                if delimiter == '<':
                    assert not any(part in header for part in ('grpc', 'sqlite', 'rclcpp', 'nav2', 'onnx')), (
                        f'infrastructure leaked into {source}: {header}')
                    continue
                local = (ROOT / header if header.startswith('backend/') else source.parent / header).resolve()
                assert local.is_file() and any(local.is_relative_to(parent) for parent in allowed), (
                    f'C++ systems dependency crosses a layer: {source}: {header}')

def main():
    expected=json.loads((ROOT/'docs/architecture/communication/frozen-core-sha256.json').read_text())['files']
    current={p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes().replace(b"\r\n",b"\n")).hexdigest() for p in (ROOT/'backend/server').rglob('*') if p.is_file()}
    assert current==expected,'Frozen communication core changed; an explicit unfreeze decision is required'
    for folder in ('backend/application','backend/ai'):
        for p in (ROOT/folder).rglob('*'):
            if p.suffix not in ('.h','.cpp'):continue
            for include in re.findall(r'#include\s*"(backend/[^"\n]+)"',p.read_text(encoding='utf-8')):
                assert (ROOT/include).is_file(),f'Missing project header: {p}: {include}'
    for layer in ('domain','ports','application'):
      for module in ('accounts','contacts','chat','files'):
        for p in (ROOT/'backend/application/modules'/module/layer).rglob('*'):
            if p.suffix not in ('.h','.cpp'):continue
            for include in re.findall(r'#include\s*[<"]([^>"\n]+)',p.read_text(encoding='utf-8')):
                assert not any(x in include for x in ('backend/server/','libpq','onnxruntime','grpcpp','boost/')),f'illegal chat {layer} dependency: {p}: {include}'
    main=(ROOT/'main.cpp').read_text(encoding='utf-8')
    assert len(main.splitlines())<=40 and 'unique_ptr' not in main and '.router(' not in main,'Process entry owns domain resources/routes again'
    assert not any(word in main for word in ('PostgresStore','AuthService','ChatService','AiChatService')),'Process entry leaked concrete domain assembly'
    for layer in ('domain','ports'):
        for p in (ROOT/'backend/application'/layer).rglob('*.h'):
            for include in re.findall(r'#include\s*[<"]([^>"\n]+)',p.read_text(encoding='utf-8')):
                assert not any(x in include for x in ('backend/server/','storage/','libpq','grpcpp','onnxruntime')),f'Infrastructure leaked into {layer}: {p}'
    for family,facade in [('postgres','PostgresStore'),('memory','InMemoryStore')]:
        for p in (ROOT/'backend/application/storage'/family).glob('*Repository/*.cpp'):
            text=p.read_text(encoding='utf-8')
            assert not re.search(r'#include[^\n]*'+facade,text),f'Driver depends on all-purpose facade: {p}'
            assert not re.search(r'\b(?:pool_\.acquire|PQconnectdb|beginTransaction)\s*\(',text),f'Driver acquired an independent transaction: {p}'
    systems_boundaries()
    print('PASS frozen communication core (152 files), business boundaries and C++ systems layers')
if __name__=='__main__':main()
