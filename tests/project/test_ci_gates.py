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

if __name__=='__main__':unittest.main()
