"""Delivery must fail closed for missing workers, configuration and changed libraries."""
import hashlib
import io
import json
from pathlib import Path
import platform
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'scripts/project'))
sys.path.insert(0,str(ROOT))
from runtime_inventory import inventory,WORKERS,NATIVE
from package_release import members, write_archive
from release_preflight import check
from tools.observability.observer.access_preflight import metrics_access,private_file,prometheus_baseline
from tools.observability.retention import mark,prune


class Delivery(unittest.TestCase):
    def setUp(self):
        temporary=ROOT/'build-function3/delivery-checks';temporary.mkdir(parents=True,exist_ok=True)
        self.temp=tempfile.TemporaryDirectory(dir=temporary);self.root=Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def fixture(self):
        flags=('PHASE11','POSTGRES','SODIUM','CURL_CLIENT',*NATIVE)
        (self.root/'CMakeCache.txt').write_text('\n'.join('WEBSERVER_ENABLE_'+key+':BOOL=ON' for key in flags))
        for file in ('webserver',*WORKERS.values()):
            path=self.root/file;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'fixture-program')

    def test_native_missing_worker_and_feature(self):
        self.fixture();result=inventory(self.root,'native',{},False)
        self.assertTrue(result['passed']);self.assertFalse(result['runtime']['modelWarmupProbed'])
        (self.root/WORKERS['onnx']).unlink()
        self.assertFalse(inventory(self.root,'native',{},False)['passed'])
        self.fixture()
        cache=self.root/'CMakeCache.txt';cache.write_text(cache.read_text().replace('GRPC:BOOL=ON','GRPC:BOOL=OFF'))
        self.assertFalse(inventory(self.root,'native',{},False)['passed'])

    def test_configuration_never_exposes_values(self):
        self.fixture();secret='private-password-never-serialize'
        result=inventory(self.root,'native',{'WEB_PHASE11_DATABASE':secret},False)
        self.assertFalse(result['runtime']['configurationValid'])
        self.assertNotIn(secret,json.dumps(result))

    def release(self):
        path=self.root/'webserver';path.write_bytes(b'elf-fixture')
        manifest={'schemaVersion':2,'profile':'business','machine':platform.machine(),
                  'files':{'webserver':hashlib.sha256(path.read_bytes()).hexdigest()}}
        (self.root/'release.json').write_text(json.dumps(manifest));return manifest

    def test_release_tamper_and_config(self):
        self.release();self.assertTrue(check(self.root,{},False)['passed'])
        result=check(self.root,{'WEB_TLS_CERT':'not-present'},False)
        self.assertFalse(result['passed']);self.assertNotIn('not-present',json.dumps(result))
        (self.root/'webserver').write_bytes(b'changed')
        self.assertFalse(check(self.root,{},False)['passed'])

    def test_zip_modes_match_the_existing_tar_delivery(self):
        contents = {"webserver": b"server", "fleet/nebula_fleet_edge": b"edge", "data.json": b"data"}
        tar_path = self.root / "runtime.tar.gz"
        zip_path = self.root / "runtime.zip"
        for path in (tar_path, zip_path):
            write_archive(path, contents)
        self.assertEqual(list(members(tar_path)), list(members(zip_path)))
        self.assertEqual([(name, mode) for name, mode, _ in members(zip_path)],
                         [("data.json", 0o644), ("fleet/nebula_fleet_edge", 0o755), ("webserver", 0o755)])

    def test_native_manifest_and_library_changes(self):
        manifest=self.release();manifest['profile']='native'
        (self.root/'release.json').write_text(json.dumps(manifest))
        self.assertFalse(check(self.root,{},False)['passed'])
        library=self.root/'libfixture.so';library.write_bytes(b'library')
        native={'programs':{'server':{'path':'webserver','sha256':manifest['files']['webserver'],
            'linkage':{'dependencies':[{'soname':'libfixture.so','sha256':hashlib.sha256(b'library').hexdigest()}]}}}}
        (self.root/'native-runtime.json').write_text(json.dumps(native))
        reply=type('Reply',(),{'returncode':0,'stdout':'libfixture.so => /runtime/libfixture.so (0x123)'})()
        from release_preflight import digest
        def runtime_digest(path):return digest(library if path.name=='libfixture.so' else path)
        with patch('release_preflight.subprocess.run',return_value=reply),patch('release_preflight.digest',side_effect=runtime_digest):
            self.assertTrue(check(self.root,{})['passed'])
            library.write_bytes(b'changed')
            self.assertFalse(check(self.root,{})['passed'])

    def test_private_input_rejects_directory(self):
        with self.assertRaises(ValueError):private_file(self.root)

    def test_ops_preflight_rejects_anonymous_success(self):
        class Reply:
            status=200
            def __enter__(self):return self
            def __exit__(self,*args):pass
        with patch('tools.observability.observer.access_preflight.urllib.request.urlopen',return_value=Reply()):
            with self.assertRaisesRegex(ValueError,'authentication'):metrics_access(12345,'private-ops')

    def test_first_zero_counter_is_a_baseline_but_missing_data_is_not(self):
        def response(rows):return io.BytesIO(json.dumps({'data':{'result':rows}}).encode())
        with patch('tools.observability.observer.access_preflight.urllib.request.urlopen',
                   return_value=response([{'value':[123,'0']}])):
            self.assertIsNone(prometheus_baseline(19091,18094,0))
        with patch('tools.observability.observer.access_preflight.urllib.request.urlopen',return_value=response([])):
            with self.assertRaisesRegex(ValueError,'no baseline'):prometheus_baseline(19091,18094,0)

    def test_retention_only_inactive_marked_telemetry(self):
        expired=self.root/'old';expired.mkdir();mark(expired,1)
        for name in ('events.sqlite','observer.log','original-business.dump','export.json'):(expired/name).touch()
        unmarked=self.root/'unmarked';unmarked.mkdir();(unmarked/'observer.log').touch()
        with patch('tools.observability.retention.active',return_value=True):
            self.assertEqual(prune(self.root,9*86400,True)['expiredTelemetry'],[])
        with patch('tools.observability.retention.active',return_value=False):
            plan=prune(self.root,9*86400)
            self.assertEqual(len(plan['expiredTelemetry']),2);self.assertTrue((expired/'events.sqlite').exists())
            prune(self.root,9*86400,True)
        self.assertTrue((expired/'original-business.dump').exists())
        self.assertTrue((expired/'export.json').exists())
        self.assertTrue((unmarked/'observer.log').exists())


if __name__=='__main__':unittest.main()
