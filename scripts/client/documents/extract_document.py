"""Local-only, bounded document extraction. No network, shell, OCR or downloads.

TXT/Markdown/DOCX use the standard library. Text PDF reuses the already installed
PyMuPDF 1.28.0 (AGPL-3.0/commercial); its license is documented, never pip-installed.
"""
from pathlib import Path
import json
import sys
import zipfile
import xml.etree.ElementTree as ET

def extract(path):
    if not path.is_file() or path.stat().st_size > 10*1024*1024:
        raise ValueError('文件大小超过10 MiB')
    suffix=path.suffix.lower()
    if suffix in ('.txt','.md'):
        text=path.read_text(encoding='utf-8-sig')
    elif suffix=='.docx':
        with zipfile.ZipFile(path) as archive:
            if len(archive.infolist())>2000 or sum(x.file_size for x in archive.infolist())>50*1024*1024:
                raise ValueError('DOCX解压内容超出限制')
            info=archive.getinfo('word/document.xml')
            if info.file_size>2*1024*1024:raise ValueError('DOCX正文XML过大')
            body=archive.read(info)
            if b'<!DOCTYPE' in body.upper() or b'<!ENTITY' in body.upper():raise ValueError('拒绝XML外部实体')
            root=ET.fromstring(body)
            namespace='{http://schemas.openxmlformats.org/wordprocessingml/2006/main}'
            text='\n'.join(''.join(p.itertext()) for p in root.iter(namespace+'p'))
    elif suffix=='.pdf':
        try:import pymupdf
        except ImportError:raise ValueError('现有Python未提供PyMuPDF，未安装新依赖；可先导出TXT')
        with pymupdf.open(path) as document:
            if document.needs_pass:raise ValueError('请先解密PDF')
            if len(document)>64:raise ValueError('单份PDF最多64页，请先拆分')
            text=''
            for page in document:
                text+=page.get_text()+'\n'
                if len(text.encode('utf-8'))>200*1024:raise ValueError('PDF提取文本超过200 KiB')
            if len(text.strip())<8:raise ValueError('PDF无可检索正文；扫描件不自动运行OCR')
    else:raise ValueError('仅支持TXT、Markdown、文本PDF和DOCX')
    if not text.strip() or len(text.encode('utf-8'))>200*1024:raise ValueError('提取正文须为1..200 KiB')
    return text

if __name__=='__main__':
    code=0
    try:result={'text':extract(Path(sys.argv[1]))}
    except Exception as error:result={'error':str(error)};code=1
    Path(sys.argv[2]).write_text(json.dumps(result,ensure_ascii=False),encoding='utf-8')
    raise SystemExit(code)
