"""Offline CI catalog contracts; no PyYAML or network dependency."""
from copy import deepcopy
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('workflow_policy', ROOT / 'scripts/project/check_workflows.py')
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class WorkflowCatalog(unittest.TestCase):
    def setUp(self):
        self.catalog, self.sources = policy.load()
        self.caller = '.github/workflows/ci.yml'

    def reject(self, error):
        with self.assertRaisesRegex(ValueError, error):
            policy.check(self.catalog, self.sources)

    def test_registered_workflows_and_manual_lanes(self):
        result = policy.check(self.catalog, self.sources)
        self.assertTrue(result['passed'])
        self.assertGreater(result['reusable'], 1)
        self.assertEqual(set(result['manualLanes']), {'business', 'native', 'fleet-grpc'})

    def test_new_workflow_registers_without_engine_or_fixed_count_changes(self):
        path = '.github/workflows/verify-extra-contract.yml'
        template = self.sources['.github/workflows/verify-source.yml']
        self.sources[path] = template.replace('  source:\n', '  extra-contract:\n')
        row = deepcopy(next(row for row in self.catalog['workflows'] if row['id'] == 'source'))
        row.update(id='extra-contract', path=path, jobs=['extra-contract'])
        self.catalog['workflows'].append(row)
        next(row for row in self.catalog['workflows'] if row['id'] == 'ci')['jobs'].append('extra-contract')
        self.sources[self.caller] += '\n  extra-contract:\n    uses: ./' + path + '\n'
        result = policy.check(self.catalog, self.sources)
        self.assertEqual(result['workflows'], len(self.catalog['workflows']))

    def test_duplicate_id_and_path_traversal_are_rejected(self):
        original = deepcopy(self.catalog)
        self.catalog['workflows'][1]['id'] = self.catalog['workflows'][0]['id']
        self.reject('duplicate workflow id')
        self.catalog = original
        self.catalog['workflows'][1]['path'] = '../outside.yml'
        self.reject('workflow path')

    def test_unregistered_workflow_is_rejected(self):
        self.sources['.github/workflows/unregistered.yml'] = 'name: Unregistered\n'
        self.reject('Unregistered')

    def test_unknown_local_target_is_rejected(self):
        self.sources[self.caller] = self.sources[self.caller].replace('verify-source.yml', 'missing.yml')
        self.reject('Unknown local')

    def test_inner_and_caller_manual_gates_are_both_required(self):
        original = dict(self.sources)
        self.sources[self.caller] = self.sources[self.caller].replace("    if: github.event_name == 'workflow_dispatch' && inputs.run_fleet\n", '')
        self.reject('caller manual')
        self.sources = original
        path = '.github/workflows/verify-fleet-grpc.yml'
        self.sources[path] = self.sources[path].replace("    if: github.event_name == 'workflow_dispatch'\n", '')
        self.reject('inner manual')

    def test_permission_escalation_and_secret_inheritance_are_rejected(self):
        original = dict(self.sources)
        self.sources[self.caller] = self.sources[self.caller].replace('contents: read', 'contents: write')
        self.reject('read-only')
        self.sources = original
        self.sources[self.caller] += '    secrets: inherit\n'
        self.reject('secret inheritance')

    def test_business_and_native_dependencies_cannot_be_lost(self):
        self.sources[self.caller] = self.sources[self.caller].replace('    needs: business\n', '')
        self.reject('required lane dependencies')

    def test_dependency_cycles_and_missing_nodes_are_rejected(self):
        for graph in ({'a': ['b'], 'b': ['a']}, {'a': ['missing']}):
            with self.subTest(graph=graph), self.assertRaises(ValueError):
                policy.acyclic(graph)

    def test_action_commit_pins_are_required(self):
        path = '.github/workflows/verify-source.yml'
        self.sources[path] = self.sources[path].replace('checkout@3d3c42e5aac5ba805825da76410c181273ba90b1', 'checkout@main')
        self.reject('full commit pin')

    def test_dispatch_input_types_are_checked(self):
        self.sources[self.caller] = self.sources[self.caller].replace('type: boolean', 'type: string', 1)
        self.reject('input contract')

    def test_reusable_lane_has_no_automatic_or_privileged_trigger(self):
        path = '.github/workflows/verify-source.yml'
        self.sources[path] = self.sources[path].replace('  workflow_call:', '  pull_request_target:')
        self.reject('workflow trigger')

    def test_arbitrary_commands_cannot_be_exposed_as_inputs(self):
        path = '.github/workflows/verify-source.yml'
        self.sources[path] += '      - run: ${{ inputs.command }}\n'
        self.reject('command interface')


if __name__ == '__main__':
    unittest.main()