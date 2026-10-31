"""Index actual Linux business registrations and emit domain-owned browser bindings.

No downloads. --check fails on stale bindings. Payload/ACL semantics remain in the
OpenAPI and real service tests; route discovery alone is not a schema proof.
"""
import argparse
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]
FRONT=ROOT/'frontend/phase11'
DOMAIN_DIR={'accounts':'accounts','contacts':'contacts','chat':'chat','files':'files',
            'activities':'activities','profiles':'profile','tree':'tree','admin':'admin',
            'ai':'ai','privateAi':'ai','webAi':'ai','knowledge':'knowledge','training':'training',
            'announcements':'announcements','speech':'speech','notifications':'announcements','shared':'shared'}
DOMAIN_DIR['operations']='shared'

def domain(path):
    if path.startswith('/health/'):return 'operations'
    if path.startswith('/api/me/speech'):return 'speech'
    if path.startswith('/api/me/profile-display'):return 'profiles'
    if path.startswith('/api/me/notification-settings'):return 'notifications'
    if path.startswith(('/api/auth/','/api/me')):return 'accounts'
    if path.startswith(('/api/friends','/api/friend-requests','/api/blocks','/api/users/search')):return 'contacts'
    if path.startswith('/api/files') or path.endswith('/file-messages'):return 'files'
    if path.startswith(('/api/conversations','/api/search/messages')):return 'chat'
    if path.startswith('/api/ai/private'):return 'privateAi'
    if path.startswith('/api/ai/web'):return 'webAi'
    first=path.split('/')[2]
    return first if first in DOMAIN_DIR else 'shared'

def operation(method,path):
    parts=re.findall(r'[A-Za-z0-9]+',path.removeprefix('/api/'))
    return method.lower()+''.join(p[0].upper()+p[1:] for p in parts)

def discover():
    entries={}
    for folder in ('backend/application','backend/ai'):
        for p in sorted((ROOT/folder).rglob('*.cpp')):
            text=p.read_text(encoding='utf-8')
            for method,path in re.findall(r'(?:\.|->)(GET|POST|PUT|PATCH|DEL|HEAD)\(\s*"([^"\n]+)"',text):
                if not path.startswith(('/api/','/health/')):continue
                method='DELETE' if method=='DEL' else method
                key=(method,path)
                entries.setdefault(key,{'method':method,'path':path,'domain':domain(path),'operation':operation(method,path),'sources':[]})['sources'].append(p.relative_to(ROOT).as_posix())
    # This adapter intentionally registers two runtime-selected bases. Extract
    # its suffixes, rather than silently omitting the dynamic registrations.
    p=ROOT/'backend/ai/private/transport/PrivateGenerationHttp/PrivateGenerationHttp.cpp'
    text=p.read_text(encoding='utf-8')
    bases=re.findall(r'"(/api/ai/(?:web|private)/generations)"',text)
    for method,suffix in re.findall(r'router\.(POST|GET|DEL)\(base(?:\+"([^"]+)")?\s*,',text):
        for base in bases:
            method='DELETE' if method=='DEL' else method;path=base+suffix
            entries[(method,path)]={'method':method,'path':path,'domain':domain(path),'operation':operation(method,path),'sources':[p.relative_to(ROOT).as_posix()]}
    p=ROOT/'backend/application/modules/assistant/transport/AssistantModelsHttp/AssistantModelsHttp.cpp'
    text=p.read_text(encoding='utf-8')
    for block in re.findall(r'for\(const auto&path:\{([^}]+)\}\)router.GET\(path',text):
        for path in re.findall(r'"(/api/[^\"]+)"',block):
            entries[('GET',path)]={'method':'GET','path':path,'domain':domain(path),'operation':operation('GET',path),'sources':[p.relative_to(ROOT).as_posix()]}
    return sorted(entries.values(),key=lambda r:(r['domain'],r['path'],r['method']))

def outputs(rows):
    payload={'version':1,'scope':'Linux business HTTP; local assistant has a separate Windows boundary',
             'payloadSemantics':'chat-workspace.openapi.json and realtime-contracts.json plus real integration tests; not inferred from route literals',
             'routes':rows}
    result={ROOT/'docs/contracts/business-routes.json':json.dumps(payload,ensure_ascii=False,indent=2)+'\n'}
    compact=[{k:r[k] for k in ('method','path','domain','operation')} for r in rows]
    result[FRONT/'shared/ApiContracts/ApiCatalog.js']='// Generated from actual C++ business registrations. Do not edit.\nexport const apiCatalog='+json.dumps(compact,ensure_ascii=False,separators=(',',':'))+';\n'
    for name in sorted({r['domain'] for r in rows}):
        title=name[0].upper()+name[1:]+'Api'
        file=FRONT/DOMAIN_DIR[name]/title/(title+'.js')
        relative=Path(__import__('os').path.relpath(FRONT/'shared/ApiContracts/ApiContracts.js',file.parent)).as_posix()
        if not relative.startswith('.'):relative='./'+relative
        definitions=[[r['operation'],r['method'],r['path']] for r in rows if r['domain']==name]
        result[file]='// Generated domain contract bindings; transport and page state are injected separately.\nimport { defineEndpoints } from '+json.dumps(relative)+';\nexport const '+name+'Api=defineEndpoints('+json.dumps(name)+','+json.dumps(definitions,ensure_ascii=False,separators=(',',':'))+');\n'
    return result

def validate_consumers(rows,front=FRONT):
    """Regenerating a renamed endpoint cannot silently leave a dead UI method."""
    allowed={name+'Api':{r['operation'] for r in rows if r['domain']==name} for name in {r['domain'] for r in rows}}
    for path in front.rglob('*.js'):
        if 'vendor' in path.parts:continue
        text=path.read_text(encoding='utf-8')
        for variable,member in re.findall(r'\b([A-Za-z]+Api)\.([A-Za-z0-9_]+)',text):
            if variable in allowed and member not in allowed[variable]:
                raise RuntimeError(f'Unknown domain API binding: {path}: {variable}.{member}')
            if variable in allowed and not re.search(r'\bimport\s*\{[^}]*\b'+variable+r'\b[^}]*\}\s*from',text):
                raise RuntimeError(f'Domain API not imported: {path}: {variable}')
        for relative in re.findall(r'''\bfrom\s*["'](\.{1,2}/[^"']+)["']''',text):
            if not (path.parent/relative.split('?',1)[0]).is_file():
                raise RuntimeError(f'Missing frontend module: {path}: {relative}')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--check',action='store_true');a=p.parse_args()
    rows=discover();stale=[]
    for path,text in outputs(rows).items():
        current=path.read_text(encoding='utf-8') if path.exists() else ''
        if path.suffix=='.js':current=re.sub(r'\?v=[A-Za-z0-9_-]+(?=[\"\'])','',current)
        if current==text:continue
        stale.append(path.relative_to(ROOT).as_posix())
        if not a.check:path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text,encoding='utf-8')
    if a.check and stale:raise SystemExit('Stale business contract bindings: '+', '.join(stale))
    validate_consumers(rows)
    print(json.dumps({'businessHttpContracts':len(rows),'domains':len({r['domain'] for r in rows}),'updated':len(stale)}))
if __name__=='__main__':main()
