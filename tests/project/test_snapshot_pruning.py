import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
TEMP=ROOT/'build-function3/test-snapshot-pruning'
TEMP.mkdir(parents=True,exist_ok=True)
spec=importlib.util.spec_from_file_location('snapshot_unpack',ROOT/'scripts/function3/validation/unpack_snapshot.py')
snapshot=importlib.util.module_from_spec(spec);spec.loader.exec_module(snapshot)

class PruningTests(unittest.TestCase):
    def test_prunes_only_owned_unchanged_files(self):
        with tempfile.TemporaryDirectory(dir=TEMP) as directory:
            root=Path(directory);owned=root/'retired.css';owned.write_bytes(b'original')
            unknown=root/'private.db';unknown.write_bytes(b'keep')
            sha=hashlib.sha256(owned.read_bytes()).hexdigest()
            removable,preserved=snapshot.removed_files(root,{'retired.css':sha},{})
            self.assertEqual(removable,[owned]);self.assertFalse(preserved)
            self.assertNotIn(unknown,removable)
            owned.write_bytes(b'local user edit')
            self.assertEqual(snapshot.removed_files(root,{'retired.css':sha},{}),([],['retired.css']))

    def test_refuses_parent_escape_and_preserves_current_files(self):
        with tempfile.TemporaryDirectory(dir=TEMP) as directory:
            root=Path(directory)
            with self.assertRaises(RuntimeError):snapshot.source_path(root,'../outside')
            self.assertEqual(snapshot.removed_files(root,{'current.h':'old'},{'current.h':'new'}),([],[]))

if __name__=='__main__':unittest.main()
