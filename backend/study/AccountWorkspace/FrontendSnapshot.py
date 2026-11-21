"""Load an isolated, read-only snapshot of the NEBULA web frontend."""
from pathlib import Path
from urllib.parse import quote


MIME = {
    '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
    '.json': 'application/json; charset=utf-8', '.svg': 'image/svg+xml', '.png': 'image/png',
    '.jpg': 'image/jpeg', '.webp': 'image/webp', '.woff2': 'font/woff2',
}
FRONTEND_PREFIX = '/phase11/'
MAX_ASSET_BYTES = 12 * 1024 * 1024


class FrontendSnapshot:
    def __init__(self, folder: Path):
        root = folder.resolve(strict=True)
        self.routes = {}
        for file in sorted(root.rglob('*')):
            if file.suffix.lower() not in MIME or not file.is_file() or file.resolve() != file:
                continue
            if any(part.startswith('.') for part in file.relative_to(root).parts) or file.stat().st_size > MAX_ASSET_BYTES:
                continue
            relative = quote(file.relative_to(root).as_posix(), safe='/')
            route = FRONTEND_PREFIX + relative
            entry = (file, MIME[file.suffix.lower()])
            self.routes[route] = entry
            if file.name == 'index.html':
                self.routes[route.removesuffix('index.html')] = entry
        if not {FRONTEND_PREFIX, FRONTEND_PREFIX + 'study/', FRONTEND_PREFIX + 'asset-manifest.json'} <= self.routes.keys():
            raise ValueError('请选择包含首页、研途页面和资源清单的前端目录')
        self.routes[FRONTEND_PREFIX.rstrip('/')] = self.routes[FRONTEND_PREFIX]

    def asset(self, path):
        return self.routes.get(path)
