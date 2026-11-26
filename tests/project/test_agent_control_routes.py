"""Pure configuration and file-snapshot contracts; no production or network IO."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from backend.study.AccountWorkspace.FrontendSnapshot import FrontendSnapshot
from backend.study.AccountWorkspace.GatewayConfig import add_workspace
from tools.observability.gateway import render


def selected(config, path):
    listener = next(item for item in config['static_resources']['listeners'] if item['name'] == 'nebula_http')
    routes = listener['filter_chains'][0]['filters'][0]['typed_config']['route_config']['virtual_hosts'][0]['routes']
    for route in routes:
        match = route['match']
        segment = match.get('path_separated_prefix')
        if (path == match.get('path') or path.startswith(match.get('prefix', '\0')) or
                segment and (path == segment or path.startswith(segment + '/'))):
            return route
    raise AssertionError(path)


class AgentRoutes(unittest.TestCase):
    def config(self, serve=False):
        return add_workspace(render(38110, 38111, 38112, ROOT / 'build-function3' / 'fixture-access.jsonl'),
                             38113, ['http://127.0.0.1:38110'], serve_frontend=serve)

    def test_native_access_is_exact_and_precedes_executable_prefix(self):
        config = self.config()
        access = selected(config, '/api/private/agent-control/access')
        self.assertEqual(access['name'], 'agent_control_access')
        self.assertEqual(access['route']['cluster'], selected(config, '/api/auth/login')['route']['cluster'])
        self.assertEqual(selected(config, '/api/private/agent-control/access/extra')['name'], 'agent_control')

    def test_executable_routes_use_workspace_and_bounded_timeout(self):
        route = selected(self.config(), '/api/private/agent-control/invoke')
        self.assertEqual(route['route']['cluster'], 'nebula_study')
        self.assertEqual(route['route']['timeout'], '15s')

    def test_control_page_and_assets_stay_native_with_public_snapshot_enabled(self):
        for serve in (False, True):
            config = self.config(serve)
            for path in ('/phase11/agent-control', '/phase11/agent-control/', '/phase11/agent-control/app.js'):
                with self.subTest(serve=serve, path=path):
                    route = selected(config, path)
                    self.assertEqual(route['name'], 'agent_control_page')
                    self.assertNotEqual(route['route']['cluster'], 'nebula_study')

    def test_control_page_match_has_segment_boundary(self):
        self.assertNotEqual(selected(self.config(True), '/phase11/agent-control-public/app.js')['name'], 'agent_control_page')

    def test_public_snapshot_excludes_all_private_control_files(self):
        with tempfile.TemporaryDirectory(prefix='agent-snapshot-') as folder:
            root = Path(folder)
            for rel in ('index.html', 'study/index.html', 'asset-manifest.json',
                        'agent-control/index.html', 'agent-control/app.js', 'agent-control/style.css'):
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('fixture', encoding='utf8')
            snapshot = FrontendSnapshot(root)
            self.assertIsNotNone(snapshot.asset('/phase11/'))
            self.assertIsNotNone(snapshot.asset('/phase11/study/'))
            self.assertFalse(any(path.startswith('/phase11/agent-control') for path in snapshot.routes))


if __name__ == '__main__':
    unittest.main()
