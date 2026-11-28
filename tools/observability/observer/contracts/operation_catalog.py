"""The C++ fixed-name registry also owns Observer labels, queries and UI metadata."""
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[4]
CATALOG=ROOT/'backend/application/observability/catalog/OperationCatalog.def'
ENTRY=re.compile(r'NEBULA_OPERATION\((\w+), (\w+), "([a-z]+)", "([^"\\]+)", "([A-Za-z0-9_:]+)"\)')
FUNCTION_DEFINITIONS=[]
for line in CATALOG.read_text(encoding='utf-8').splitlines():
    if not line.strip() or line.startswith('//'):continue
    match=ENTRY.fullmatch(line)
    if not match:raise ValueError('Invalid fixed operation catalog')
    enum,key,domain,title,symbol=match.groups()
    FUNCTION_DEFINITIONS.append({'key':key,'domain':domain,'title':title,'symbol':symbol})
if (not 4<=len(FUNCTION_DEFINITIONS)<=32 or any(len({row[field] for row in FUNCTION_DEFINITIONS})!=len(FUNCTION_DEFINITIONS)
        for field in ('key','symbol'))):
    raise ValueError('Duplicate or excessive operation catalog')
FUNCTIONS={'function_'+row['key']:row['symbol'] for row in FUNCTION_DEFINITIONS}
_routes=json.loads((ROOT/'docs/contracts/business-routes.json').read_text(encoding='utf-8'))['routes']
HTTP_OPERATIONS=frozenset(row['method']+' '+row['path'] for row in _routes if row['path'].startswith('/api/'))
FUNCTION_CALLS='nebula_function_calls_total'
FUNCTION_IN_FLIGHT='nebula_function_in_flight'
