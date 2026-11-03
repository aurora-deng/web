"""Check public documentation links, catalog coverage and move destinations.

Uses only the standard library. Archived evidence, frozen communication files,
and *.local.md account notes are immutable/outside this publishing check.
Fragments are retained but not treated as filesystem paths or reimplemented as
a Markdown renderer's heading-slug algorithm.
"""
import argparse
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote

ROOT=Path(__file__).resolve().parents[2]
FENCES=re.compile(r'^(`{3,}|~{3,}).*?^\1[^\n]*$',re.M|re.S)
LINKS=re.compile(r'!?\[[^\]\n]*\]\((<[^>\n]+>|[^)\n]+)\)')
REFERENCES=re.compile(r'^\s{0,3}\[(?!\^)[^\]\n]+\]:\s*(.+)$',re.M)
SCHEME=re.compile(r'^[a-zA-Z][a-zA-Z0-9+.-]*:|^//')

def immutable(name):
    return name.startswith(('artifacts/','backend/server/','docs/evidence/')) or name.endswith('.local.md')

def pages():
    names=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=ROOT).decode().split('\0')
    return sorted({ROOT/n for n in names if n.endswith('.md') and not immutable(n) and (ROOT/n).is_file()})

def destination(raw):
    raw=raw.strip()
    if raw.startswith('<'):raw=raw[1:raw.index('>')]
    else:raw=re.sub(r'\s+[\"\'][^\n]*[\"\']$','',raw)
    if SCHEME.match(raw):return None
    return unquote(re.split(r'[?#]',raw,1)[0]) or None

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path)
    p.add_argument('--refresh-catalog',action='store_true',help='Explicitly rebuild the public Markdown catalog before checking; never includes private notes')
    a=p.parse_args();errors=[];checked=0;public=pages()
    for page in public:
        text=FENCES.sub('',page.read_text(encoding='utf-8-sig'))
        # Inline examples can contain C++ lambdas such as [this](Context&),
        # which are code rather than Markdown destinations.
        text=re.sub(r'(`+)(?!`)([^\n]*?)\1(?!`)','',text)
        for match in [*LINKS.finditer(text),*REFERENCES.finditer(text)]:
            path=destination(match[1])
            if path is None:continue
            target=(ROOT/path.lstrip('/') if path.startswith('/') else page.parent/path).resolve()
            checked+=1
            if not target.is_relative_to(ROOT) or not target.exists():
                errors.append(f'{page.relative_to(ROOT).as_posix()}: {match[1]}')
    document_root=ROOT/'docs'
    expected={page.relative_to(ROOT).as_posix() for page in public if page.is_relative_to(document_root) and not page.read_text(encoding='utf-8-sig').startswith('<!-- Compatibility redirect:')}
    if a.refresh_catalog:
        rows=[]
        for name in sorted(expected):
            page=ROOT/name
            title=next((line[2:].strip() for line in page.read_text(encoding='utf-8-sig').splitlines() if line.startswith('# ')),page.stem)
            relative=page.relative_to(document_root).as_posix()
            rows.append({'path':name,'title':title,'category':relative.split('/')[0] if '/' in relative else 'navigation','kind':'index' if page.name=='README.md' else 'document'})
        catalog={'schema':1,'generated':datetime.now(timezone(timedelta(hours=8))).date().isoformat(),'scope':'public human-readable documents; raw evidence and local account files excluded','documents':rows}
        (document_root/'catalog.json').write_text(json.dumps(catalog,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    catalog=json.loads((document_root/'catalog.json').read_text(encoding='utf-8'))
    listed=[row['path'] for row in catalog['documents']]
    if len(listed)!=len(set(listed)) or set(listed)!=expected:
        errors.append('Documentation catalog coverage mismatch: '+str(sorted(expected.symmetric_difference(listed))))
    moves=json.loads((document_root/'document-moves.json').read_text(encoding='utf-8'))
    for old,new in moves['moves'].items():
        if not (ROOT/new).is_file():errors.append('Missing move destination: '+new)
        if (ROOT/old).exists() and old not in moves['compatibilityRedirects']:errors.append('Duplicate old document remained: '+old)
    for old,new in moves['compatibilityRedirects'].items():
        page=ROOT/old
        if not page.is_file() or not page.read_text(encoding='utf-8').startswith('<!-- Compatibility redirect:'):
            errors.append('Missing marked compatibility redirect: '+old)
    result={'passed':not errors,'markdownPages':len(public),'checkedLocalLinks':checked,'catalogPages':len(listed),'movedDocuments':len(moves['moves']),'compatibilityRedirects':len(moves['compatibilityRedirects']),'excluded':['raw evidence','frozen backend/server','private *.local.md'],'fragments':'retained; heading slug rendering not checked','errors':errors}
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False))
    if errors:raise SystemExit(1)

if __name__=='__main__':main()
