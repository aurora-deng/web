import importlib.util
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('source_inventory',ROOT/'scripts/project/source_guard.py')
inventory=importlib.util.module_from_spec(spec);spec.loader.exec_module(inventory)
spec=importlib.util.spec_from_file_location('build_stamp',ROOT/'scripts/project/build_stamp.py')
stamp=importlib.util.module_from_spec(spec);spec.loader.exec_module(stamp)
class Inventory(unittest.TestCase):
    def test_build_fingerprint_uses_portable_case_and_newline_order(self):
        base=ROOT/'build-function3/inventory-contract';base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as folder:
            root=Path(folder)
            # B precedes a in the canonical manifest, regardless of host Path rules.
            entries=[('CMakeLists.txt',b'cmake\n'),('backend/B.cpp',b'B\n'),
                     ('backend/a.cpp',b'a\n'),('main.cpp',b'main\n')]
            for name,data in entries:
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data.replace(b'\n',b'\r\n'))
            expected=hashlib.sha256(b''.join(name.encode()+b'\0'+data for name,data in entries)).hexdigest()
            with patch.object(stamp,'ROOT',root):self.assertEqual(stamp.fingerprint(),expected)
    def test_public_build_scripts_and_workflow_included_private_outputs_excluded(self):
        base=(ROOT/'build-function3/inventory-contract').resolve();assert base.is_relative_to(ROOT.resolve());base.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as folder:
            root=Path(folder)
            names=['scripts/project/build_stamp.py','.github/workflows/ci.yml','.gitignore','.env.example','.env.production','build-release/generated.cpp','artifacts/run.json','models/private.txt','docs/database/accounts.local.md','secret.key','main.cpp']
            for name in names:
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('fixture')
            with patch.object(inventory,'ROOT',root),patch.object(inventory.subprocess,'check_output',return_value=('\0'.join(names)+'\0').encode()):
                self.assertEqual(set(inventory.names()),{'scripts/project/build_stamp.py','.github/workflows/ci.yml','.gitignore','.env.example','main.cpp'})
if __name__=='__main__':unittest.main()
