"""Evidence semantics, not a benchmark. No service, database or package installation."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
FILE=ROOT/'scripts/function3/validation/measurement_noise.py'
spec=importlib.util.spec_from_file_location('measurement_noise',FILE)
noise=importlib.util.module_from_spec(spec);spec.loader.exec_module(noise)


def calibration(rounds=10):
    doc={'mode':'calibration','roundsRequested':rounds,'configuration':{'workers':4},'workload':{'operations':12000},
         'baselineBinarySha256':'a'*64,'currentBinarySha256':'a'*64,'calibrationCompleted':True,'gateEvaluated':False,'passed':None}
    for variant in ('baseline','current'):
        doc[variant]=[{'round':n,'ackP99Ms':10.0,'fastQps':20000.0,'errors':0,'ackSamples':3600,
            'binarySha256':'a'*64,'configurationSha256':noise.digest(doc['configuration']),
            'workloadSha256':noise.digest(doc['workload']),'fixtureSha256':'b'*64} for n in range(1,rounds+1)]
    return doc


class EvidenceTests(unittest.TestCase):
    def test_identical_evidence_is_descriptive_not_acceptance(self):
        report=noise.analyse(calibration())
        self.assertEqual(report['level'],'DESCRIPTIVE_ONLY')
        self.assertTrue(report['identityVerified']);self.assertFalse(report['acceptanceReinterpreted'])
        self.assertEqual(noise.analyse({'performance':calibration()})['level'],'DESCRIPTIVE_ONLY')

    def test_labels_and_stage_names_do_not_prove_same_binary(self):
        doc=calibration();doc['currentBinarySha256']='c'*64
        with self.assertRaises(ValueError):noise.analyse(doc)
        doc=calibration();doc['mode']='acceptance';doc['stage']=doc['baselineStage']='same';doc['passed']=False
        report=noise.analyse(doc)
        self.assertEqual(report['level'],'NOT_CALIBRATION');self.assertIs(report['publishedPassedUnchanged'],False)
        wrapped=noise.analyse({'performance':doc,'performanceGatePassed':False})
        self.assertIs(wrapped['publishedPassedUnchanged'],False)

    def test_no_extreme_sample_censoring_or_threshold_relaxation(self):
        doc=calibration();doc['current'][-1]['ackP99Ms']=1000
        result=noise.analyse(doc)
        self.assertEqual(result['pairedAckIncreasePercent'][-1],9900)
        self.assertGreater(result['absolutePairDifferenceP95Percent'],100)
        self.assertEqual(result['outliersRemoved'],0);self.assertEqual(result['gatePercentUnchanged'],20)
        self.assertEqual(result['level'],'NOISY')

    def test_missing_duplicate_round_and_invalid_metric_rejected(self):
        broken=[]
        doc=calibration();doc['current'].pop();broken.append(doc)
        doc=calibration();doc['current'][1]['round']=1;broken.append(doc)
        for value in (None,0,-1,float('nan'),float('inf'),True):
            doc=calibration();doc['current'][0]['ackP99Ms']=value;broken.append(doc)
        doc=calibration();doc['current'][0]['errors']=1;broken.append(doc)
        for doc in broken:
            with self.subTest(doc=doc):
                with self.assertRaises(ValueError):noise.analyse(doc)

    def test_config_workload_fixture_and_samples_checked_per_round(self):
        for key,value in [('configurationSha256','f'*64),('workloadSha256','f'*64),('fixtureSha256','f'*64),('ackSamples',999),('binarySha256','')]:
            doc=calibration();doc['current'][3][key]=value
            with self.subTest(key=key):
                with self.assertRaises(ValueError):noise.analyse(doc)
        self.assertEqual(noise.analyse(calibration(3))['level'],'INSUFFICIENT')

    def test_named_aa_acceptance_still_fails_original_gate(self):
        doc=calibration();doc.update(mode='acceptance',label='aa-anything',fastRegressionPercent=0,pairedAckP99IncreasePercent=[78.47])
        doc['current'][-1]['ackP99Ms']=17.847
        self.assertEqual(noise.acceptance_result(doc),(True,False))
        doc['mode']='calibration';self.assertEqual(noise.acceptance_result(doc),(False,None))

    def test_cli_keeps_input_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);source=root/'summary.json';source.write_text(json.dumps(calibration()),encoding='utf-8')
            original=source.read_bytes();output=root/'analysis'
            command=[sys.executable,str(FILE),'--summary',str(source),'--out-dir',str(output)]
            first=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(first.returncode,0,first.stderr);self.assertEqual(source.read_bytes(),original)
            self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)

    def test_harness_invalid_calibration_fails_before_runtime_access(self):
        command=[sys.executable,str(ROOT/'scripts/function3/validation/phase47_benchmark.py'),'--calibration','--rounds','1']
        result=subprocess.run(command,capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0);self.assertIn('Calibration needs',result.stderr)

    def test_presets_are_explicit_linux_only_and_no_empty_green(self):
        presets=json.loads((ROOT/'CMakePresets.json').read_text(encoding='utf-8'))
        base=next(p for p in presets['configurePresets'] if p['name']=='base')
        self.assertEqual(base['condition']['rhs'],'Linux')
        self.assertEqual(base['cacheVariables']['WEBSERVER_ENABLE_POSTGRES'],'OFF')
        self.assertEqual(base['cacheVariables']['WEBSERVER_ENABLE_PHASE11'],'ON')
        self.assertTrue(all(p['execution']['noTestsAction']=='error' for p in presets['testPresets']))


if __name__=='__main__':unittest.main()
