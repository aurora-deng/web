"""Read document text or existing Windows OCR without changing system packages."""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUNDLE = Path.home() / ".cache/codex-runtimes/codex-primary-runtime/dependencies/python/Lib/site-packages"
if BUNDLE.exists():
    sys.path.append(str(BUNDLE))


def ocr(path):
    powershell = Path(os.environ.get("WINDIR", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run([str(powershell), "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "ocr_attachment.ps1"), "-Path", str(Path(path).resolve())], capture_output=True, timeout=65, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode:
        raise ValueError("现有 Windows 图片文字识别不可用；请提供文字版附件或选用支持图片的模型")
    return json.loads(result.stdout.decode("utf-8-sig"))["text"]


def extract(path, name, page=1, pages=3):
    path = Path(path)
    suffix = Path(name).suffix.lower()
    if suffix in {".txt", ".md", ".csv", ".json", ".log", ".html", ".xml", ".cpp", ".py"}:
        raw = path.read_bytes()[:8 * 1024 * 1024]
        for encoding in ("utf-8-sig", "gb18030", "utf-16"):
            try:
                return {"text": raw.decode(encoding), "method": "text"}
            except UnicodeError:
                continue
        raise ValueError("无法识别附件文字编码")
    if suffix == ".docx":
        from docx import Document
        document = Document(path)
        return {"text": "\n".join([p.text for p in document.paragraphs] + [" | ".join(c.text for c in r.cells) for t in document.tables for r in t.rows]), "method": "docx"}
    if suffix == ".pdf":
        from pypdf import PdfReader
        reader = PdfReader(path)
        count = len(reader.pages)
        texts, page_texts = [], []
        for index in range(page - 1, min(count, page - 1 + pages)):
            text = reader.pages[index].extract_text() or ""
            method = 'pdf-text'
            if not text.strip():
                try:
                    renderer = Path.home() / ".cache/codex-runtimes/codex-primary-runtime/dependencies/native/poppler/Library/bin/pdftoppm.exe"
                    with tempfile.TemporaryDirectory() as folder:
                        prefix = Path(folder) / "page"
                        subprocess.run([str(renderer), "-f", str(index + 1), "-l", str(index + 1), "-scale-to", "2400", "-png", "-singlefile", str(path), str(prefix)], capture_output=True, timeout=40, check=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                        text = ocr(prefix.with_suffix(".png"))
                        method = 'windows-ocr'
                except (OSError, ValueError, subprocess.SubprocessError):
                    text = "[本页为扫描图片，当前图片识别不可用，不能猜测内容]"
                    method = 'unavailable'
            start = sum(len(value) + 2 for value in texts)
            page_texts.append({'page': index + 1, 'text': text, 'method': method, 'start': start, 'end': start + len(f'第 {index + 1} 页\n{text}')})
            texts.append(f"第 {index + 1} 页\n{text}")
        return {"text": "\n\n".join(texts), "pageTexts": page_texts, "method": "pdf", "pages": count, "nextPage": page + pages if page + pages <= count else None}
    if suffix in {".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff"}:
        from PIL import Image
        with Image.open(path) as source, tempfile.TemporaryDirectory() as folder:
            image = source.convert("RGB")
            image.thumbnail((2400, 2400))
            output = Path(folder) / "ocr.png"
            image.save(output)
            return {"text": ocr(output), "method": "windows-ocr"}
    raise ValueError("此附件类型暂不能提取文字，可提供文字版或 PDF")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("name")
    parser.add_argument("--page", type=int, default=1)
    args = parser.parse_args()
    try:
        result = extract(args.path, args.name, max(1, args.page))
    except Exception as error:
        result = {"error": str(error)}
    print(json.dumps(result, ensure_ascii=True))
