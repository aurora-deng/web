"""Version the complete native-module graph, including dynamically loaded styles.

One release digest propagates dependency changes to every entrypoint. Vendor bytes
stay untouched. --check is non-mutating; normal mode updates references/manifest.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2];FRONT=ROOT/'frontend/phase11'
# Only literal JS/CSS/HTML references, never external provider addresses or API URLs.
REF=re.compile(r'''(?P<quote>["'])(?P<url>(?:/phase11/|\./|\.\./)[^"'<>\s]*?\.(?:js|css|html))(?:\?v=[^"'<>\s]*)?(?P=quote)''')
def main():
    p=argparse.ArgumentParser();p.add_argument('--check',action='store_true');a=p.parse_args()
    files=[p for p in sorted(FRONT.rglob('*')) if p.suffix in ('.html','.js','.css') and 'vendor' not in p.relative_to(FRONT).parts]
    texts={p:REF.sub(lambda m:m['quote']+m['url']+m['quote'],p.read_text(encoding='utf-8')) for p in files}
    # Ignore the generated version in its own hash; fixed point after a single run.
    payload='\n'.join(p.relative_to(FRONT).as_posix()+':'+hashlib.sha256(s.encode()).hexdigest() for p,s in texts.items())
    version=hashlib.sha256(payload.encode()).hexdigest()[:16];changed=[]
    for path,content in texts.items():
        rendered=REF.sub(lambda m:m['quote']+m['url']+'?v='+version+m['quote'],content)
        if rendered!=path.read_text(encoding='utf-8'):
            changed.append(path.relative_to(ROOT).as_posix())
            if not a.check:path.write_text(rendered,encoding='utf-8')
    manifest={'version':version,'nativeFiles':len(files),'vendor':'three-r186 (unchanged)','includesDynamicReferences':True}
    output=FRONT/'asset-manifest.json'
    if not a.check:output.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    if a.check and (changed or not output.exists() or json.loads(output.read_text())!=manifest):raise SystemExit('Stale frontend release references; run scripts/project/frontend_release.py')
    print(json.dumps({**manifest,'updatedFiles':len(changed)}))
if __name__=='__main__':main()
