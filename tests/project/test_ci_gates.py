"""Negative tests: disabled/missing real adapters must never produce a passing full report."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/project'))
from package_release import runtime_path, validate_reproduction
from check_business_docs import check as check_business_docs

spec=importlib.util.spec_from_file_location('project_ci',ROOT/'scripts/project/ci_checks.py')
ci=importlib.util.module_from_spec(spec);spec.loader.exec_module(ci)

class CiGates(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporaryRoot=(ROOT/'build-function3/ci-gates').resolve()
        assert cls.temporaryRoot.is_relative_to(ROOT.resolve())
        cls.temporaryRoot.mkdir(parents=True,exist_ok=True)
    def invoke(self,root,reason):
        output=root/'report.json'
        with patch.object(sys,'argv',['ci_checks.py','full','--build',str(root),'--output',str(output)]):
            with self.assertRaisesRegex(RuntimeError,reason):ci.main()
        self.assertFalse(json.loads(output.read_text())['passed'])
    def test_missing_configuration(self):
        with tempfile.TemporaryDirectory(dir=self.temporaryRoot) as path:self.invoke(Path(path),'provisioned Linux build')
    def test_disabled_business_is_not_a_green_skip(self):
        with tempfile.TemporaryDirectory(dir=self.temporaryRoot) as path:
            root=Path(path);(root/'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL='+str(ROOT)+'\nWEBSERVER_ENABLE_PHASE11:BOOL=OFF\n')
            self.invoke(root,'Required business features disabled')
    def test_macro_on_an_unrelated_source_is_not_driver_evidence(self):
        with tempfile.TemporaryDirectory(dir=self.temporaryRoot) as path:
            root=Path(path)
            (root/'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL='+str(ROOT)+'\n'+'\n'.join(n+':BOOL=ON' for n in ['WEBSERVER_ENABLE_PHASE11','WEBSERVER_ENABLE_POSTGRES','WEBSERVER_ENABLE_SODIUM','BUILD_TESTING']))
            (root/'compile_commands.json').write_text(json.dumps([{'file':'unrelated.cpp','command':'c++ -DWEBSERVER_HAS_POSTGRES=1 -DWEBSERVER_HAS_SODIUM=1 unrelated.cpp'}]))
            with patch.object(ci,'run'),patch.dict(os.environ,{'PHASE11_POSTGRES_CONNECTION':'isolated_test'}):self.invoke(root,'Required adapter not compiled')
    def test_missing_suites_are_not_green_and_private_ai_is_a_business_test(self):
        with self.assertRaisesRegex(RuntimeError,'Required test lanes'):ci.test_matrix(['phase11_postgres_contract_tests'])
        names=['phase11_core_tests','function3_workspace_contract','phase11_chat_reliability_tests',
               'phase11_http_auth_tests','phase11_file_tests','phase11_sodium_credentials_tests',
               'phase11_postgres_contract_tests','function3_postgres_contract',
               'function3_runtime_config','function3_business_shutdown','function3_commit_wakeup',
               'function3_tree_contract','function3_ai_link_contract','function3_speech_link_contract',
               'HttpParserTest.WaitsForHalfPacketAndContentLengthBody',
               'WebSocketParserTest.AcceptsMaskedText','phase39_private_generation_tests']
        matrix=ci.test_matrix(names)
        self.assertIn('phase39_private_generation_tests',matrix['businessWithoutPostgres'])
        with self.assertRaisesRegex(RuntimeError,'protocol tests not registered'):
            ci.test_matrix([n for n in names if not n.startswith('HttpParserTest.')])

    def test_delivery_requires_matching_binary_and_real_wire_evidence(self):
        revision = 'a' * 40
        binary = 'b' * 64
        report = {'profile': 'business', 'passed': True, 'binarySha256': binary, 'migration': 24,
                  'smoke': {'schemaVerifiedAtStartup': True, 'requiredMigration': 24, 'revision': revision[:12]},
                  'ctest': {'passed': 113, 'registered': 114, 'skipped': ['function3_redis_contract']},
                  'otlpWire': {'schema': 24, 'unauthenticated': 401, 'nonOps': 403, 'ops': 200,
                               'postgreSqlSpans': 4, 'exportResumed': True, 'httpSampling': {'failureRoots': 1}}}
        validate_reproduction(report, binary, revision)
        with self.assertRaisesRegex(RuntimeError, 'another binary'):
            validate_reproduction(report, 'c' * 64, revision)
        with self.assertRaisesRegex(RuntimeError, 'another Git revision'):
            validate_reproduction(report, binary, 'c' * 40)
        report['otlpWire']['exportResumed'] = False
        with self.assertRaisesRegex(RuntimeError, 'wire'):
            validate_reproduction(report, binary, revision)

    def test_delivery_path_allowlist(self):
        self.assertTrue(runtime_path('frontend/observability/index.html'))
        self.assertTrue(runtime_path('backend/application/observability/OperationCatalog/OperationCatalog.def'))
        self.assertTrue(runtime_path('docs/contracts/business-routes.json'))
        self.assertTrue(runtime_path('backend/application/storage/postgres/migrations/024_latest.sql'))
        for name in ('/webserver', 'frontend/phase11/../../credentials.json', 'frontend/phase11/.local/token.json',
                     'frontend/phase11/site.key', 'frontend/observability/events.sqlite', 'tools/runtime/model.onnx',
                     'frontend/phase11/back\\slash.js'):
            with self.subTest(name=name):
                self.assertFalse(runtime_path(name))

    def test_business_docs_gate_rejects_missing_domain_and_learning_guide(self):
        with tempfile.TemporaryDirectory(dir=self.temporaryRoot) as path:
            root = Path(path)
            contract = root / 'docs/contracts'
            business = root / 'docs/business/example'
            contract.mkdir(parents=True)
            business.mkdir(parents=True)
            (contract / 'business-routes.json').write_text('{"routes":[{"domain":"one"}]}')
            registry = contract / 'business-docs.json'
            registry.write_text('{"version":1,"modules":{"example":["one"]}}')
            (business.parent / 'README.md').write_text('[Reference](example/README.md) [Learn](example/learning.md)')
            (business.parent / 'release-matrix.md').write_text('[Example](example/README.md)')
            (business / 'README.md').write_text('# Example')
            self.assertIn('Missing titled business document: example/learning.md',
                          check_business_docs(root)['problems'])
            (business / 'learning.md').write_text('# Learn')
            self.assertTrue(check_business_docs(root)['passed'])
            registry.write_text('{"version":1,"modules":{"example":[]}}')
            self.assertIn('one: documentation missing', check_business_docs(root)['problems'])

if __name__=='__main__':unittest.main()
