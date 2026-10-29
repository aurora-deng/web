import importlib.util
from pathlib import Path
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('process_identity',ROOT/'scripts/function3/validation/process_identity.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)

class Identity(unittest.TestCase):
    def setUp(self):
        directory=(ROOT/'build-function3/process-identity').resolve()
        assert directory.is_relative_to(ROOT.resolve());directory.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=directory);self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.process=self.root/'123';self.process.mkdir()
    def fixture(self,state='R',ticks='45',command=b'/test/webserver\0'):
        (self.process/'stat').write_text('123 (web server) '+' '.join([state]+['0']*18+[ticks]+['0']*5))
        (self.process/'cmdline').write_bytes(command)
    def read(self):return module.owned_process(123,'45','/test/webserver',self.root)
    def test_exact_process(self):self.fixture();self.assertEqual(self.read(),123)
    def test_reused_pid_and_different_executable_are_refused(self):
        self.fixture(ticks='46')
        with self.assertRaises(RuntimeError):self.read()
        self.fixture(command=b'/other/program\0/test/webserver\0')
        with self.assertRaises(RuntimeError):self.read()
    def test_exiting_argv_window_and_zombie_are_stopped(self):
        self.fixture(command=b'');self.assertIsNone(self.read())
        self.fixture(state='Z');self.assertIsNone(self.read())
    def test_absent_process_is_stopped(self):self.assertIsNone(self.read())
if __name__=='__main__':unittest.main()
