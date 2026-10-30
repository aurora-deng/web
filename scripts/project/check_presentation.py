"""Enforce one semantic UI palette, without changing files or installing tools.

WebGL material colours belong to the artwork and are deliberately outside this
CSS/HTML rule. Meta theme-color is browser chrome, not a component palette.
"""
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]
FRONT=ROOT/'frontend/phase11'

def check(text, path):
    text=re.sub(r'/\*[\s\S]*?\*/','',text)
    assert not re.search(r'#[\da-fA-F]{3,8}\b|\brgba?\(\s*[\d.]|\bhsla?\(\s*[\d.]',text),f'Component owns a literal palette: {path}'
    assert not re.search(r'(?<![\w-])(?:white|black|red|blue|green)(?![\w-])',text),f'Named component colour: {path}'
    for name,value in re.findall(r'(--[\w-]+)\s*:\s*([^;{}]+)',text):
        assert not re.search(r'var\(\s*'+re.escape(name)+r'\s*[,)]',value),f'Self-referencing colour variable: {path}: {name}'

def main():
    count=0
    for p in FRONT.rglob('*.css'):
        if 'vendor' in p.parts or p.name=='tokens.css':continue
        check(p.read_text(encoding='utf-8'),p.relative_to(ROOT));count+=1
    for p in FRONT.rglob('*.html'):
        for style in re.findall(r'<style[^>]*>([\s\S]*?)</style>',p.read_text(encoding='utf-8')):check(style,p.relative_to(ROOT))
    print(f'PASS {count} component stylesheets and HTML style blocks use the shared semantic palette')

if __name__=='__main__':main()
