import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('api_contract_index',ROOT/'scripts/project/sync_api_contracts.py')
contracts=importlib.util.module_from_spec(spec);spec.loader.exec_module(contracts)

class ApiContracts(unittest.TestCase):
    def test_registered_dynamic_bases_and_contact_owner_are_indexed(self):
        rows=contracts.discover();index={(r['method'],r['path']):r for r in rows}
        for mode in ('web','private'):
            self.assertIn(('GET',f'/api/ai/{mode}/models'),index)
            self.assertIn(('DELETE',f'/api/ai/{mode}/generations/:generation'),index)
        row=index[('GET','/api/users/search')]
        self.assertEqual(row['domain'],'contacts')
        self.assertTrue(all('/modules/contacts/' in p for p in row['sources']))
    def test_stale_frontend_operation_and_missing_module_are_rejected(self):
        base=ROOT/'build-function3/api-contract-tests';base.mkdir(parents=True,exist_ok=True)
        rows=[{'domain':'contacts','operation':'getFriends'}]
        with tempfile.TemporaryDirectory(dir=base) as folder:
            root=Path(folder);page=root/'page.js';page.write_text('contactsApi.getDeletedEndpoint()',encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError,'Unknown domain API'):contracts.validate_consumers(rows,root)
            page.write_text('import { x } from "./missing.js";',encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError,'Missing frontend module'):contracts.validate_consumers(rows,root)
            page.write_text('contactsApi.getFriends()',encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError,'not imported'):contracts.validate_consumers(rows,root)
            (root/'contacts.js').write_text('export const contactsApi={};',encoding='utf-8')
            page.write_text('import { contactsApi } from "./contacts.js"; contactsApi.getFriends()',encoding='utf-8');contracts.validate_consumers(rows,root)

if __name__=='__main__':unittest.main()
