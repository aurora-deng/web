"""Read-only literal ESM dependency graph. No browser, network or source rewrites.

Counts source modules and discovery layers, not measured HTTP requests, browser
waterfall timings, cache hits or compressed transfer bytes. Non-literal imports
and bare/external package specifiers are explicitly outside this analysis.
"""
import argparse
from collections import deque
from html.parser import HTMLParser
import json
from pathlib import Path
import re
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
# Consume quoted strings first so comment delimiters inside strings survive.
_COMMENTS = re.compile(r'''("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`)|/\*[\s\S]*?\*/|//[^\r\n]*''')
_STATIC = re.compile(r'''(?:^|[;\n])\s*(?:import\s+(?:[^;'"`]*?\bfrom\s*)?|export\s+(?:\*(?:\s+as\s+\w+)?|\{[^}]*\})\s+from\s*)(["'])([^"'\r\n]+)\1''')
_DYNAMIC = re.compile(r'''\bimport\s*\(\s*(["'])([^"'\r\n]+)\1\s*\)''')


class EntryParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.modules = []
        self.stylesheets = []
        self.preloads = []

    def handle_starttag(self, tag, attributes):
        fields = dict(attributes)
        if tag == "script" and fields.get("type") == "module" and fields.get("src"):
            self.modules.append(fields["src"])
        if tag == "link" and fields.get("href"):
            if fields.get("rel") == "stylesheet": self.stylesheets.append(fields["href"])
            if fields.get("rel") == "modulepreload": self.preloads.append(fields["href"])


def local_reference(root, source, reference):
    parts = urlsplit(reference)
    if parts.scheme or parts.netloc or not parts.path.startswith(("/", "./", "../")):
        return None
    candidate = root / "frontend" / parts.path.lstrip("/") if parts.path.startswith("/") else source.parent / parts.path
    path = candidate.resolve()
    if not path.is_relative_to(root.resolve(strict=True)):
        raise ValueError("frontend reference escapes the project")
    if not path.is_file():
        raise FileNotFoundError("frontend module does not exist")
    return path


def imports(source):
    clean = _COMMENTS.sub(lambda m: m[1] if m[1] and not m[1].startswith("`") else "\n" * m[0].count("\n"), source)
    quoted = [(m.start(), m.end()) for m in _COMMENTS.finditer(clean) if m[1]]
    dynamic = [m[2] for m in _DYNAMIC.finditer(clean) if not any(start <= m.start() < end for start, end in quoted)]
    return [m[2] for m in _STATIC.finditer(clean)], dynamic


def analyze(root, entry):
    root = root.resolve(strict=True)
    entry = entry.resolve(strict=True)
    if not entry.is_relative_to(root): raise ValueError("entry is outside the project")
    parser = EntryParser()
    parser.feed(entry.read_text(encoding="utf-8"))
    queue = deque()
    depths = {}
    static_edges = {}
    dynamic_edges = {}
    urls = {}
    sizes = {}
    for reference in parser.modules:
        path = local_reference(root, entry, reference)
        if path is not None and path not in depths:
            depths[path] = 0
            urls[path] = reference
            queue.append(path)
    while queue:
        path = queue.popleft()
        source = path.read_text(encoding="utf-8")
        sizes[path] = path.stat().st_size
        static, dynamic = imports(source)
        static_edges[path] = []
        dynamic_edges[path] = []
        for reference in static:
            target = local_reference(root, path, reference)
            if target is None: continue
            if target.suffix not in {".js", ".mjs"}: continue
            static_edges[path].append(target)
            if target not in depths:
                depths[target] = depths[path] + 1
                query = urlsplit(reference).query
                relative = target.relative_to(root / "frontend").as_posix() if target.is_relative_to(root / "frontend") else None
                urls[target] = "/" + relative + ("?" + query if query else "") if relative else None
                queue.append(target)
        for reference in dynamic:
            target = local_reference(root, path, reference)
            if target is not None: dynamic_edges[path].append(target)
    name = lambda path: path.relative_to(root).as_posix()
    preloaded = {p for ref in parser.preloads if (p := local_reference(root, entry, ref)) is not None}
    recommended = [urls[p] for p in sorted(depths, key=lambda p: (-depths[p], name(p)))
                   if depths[p] > 0 and p not in preloaded and urls.get(p)]
    return {
        "entry": name(entry),
        "staticModules": len(depths),
        "staticDiscoveryLayers": max(depths.values(), default=-1) + 1,
        "staticSourceBytes": sum(sizes.values()),
        "entryStylesheets": len(parser.stylesheets),
        "literalDynamicEdges": sum(len(v) for v in dynamic_edges.values()),
        "modulepreloadCandidates": recommended,
        "largestModules": [{"path": name(p), "bytes": sizes[p]} for p in sorted(sizes, key=lambda p: (-sizes[p], name(p)))[:12]],
        "modules": [{"path": name(p), "url": urls.get(p), "depth": depths[p], "bytes": sizes[p],
                     "imports": [name(t) for t in static_edges[p]], "dynamicImports": [name(t) for t in dynamic_edges[p]]} for p in sorted(depths)],
        "limitations": "Literal-source estimate only; excludes runtime-computed imports, browser timings, actual request counts, compression and cache effects.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--entry", type=Path, default=ROOT / "frontend/phase11/index.html")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(ROOT, args.entry)
    if args.output:
        output = args.output.resolve()
        if not output.is_relative_to(ROOT.parent): parser.error("report output must remain in this project validation workspace")
        with output.open("x", encoding="utf-8") as stream:
            stream.write(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "modules"}, ensure_ascii=False, indent=2))


if __name__ == "__main__": main()
