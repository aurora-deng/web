"""Require a business reference and a learning guide for every registered domain."""
from collections import Counter
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
MODULE_ID = re.compile(r'[a-z][a-z0-9-]*')


def check(root):
    contract_path = root / 'docs/contracts'
    business = root / 'docs/business'
    routes = json.loads((contract_path / 'business-routes.json').read_text(encoding='utf-8'))
    registry = json.loads((contract_path / 'business-docs.json').read_text(encoding='utf-8'))
    if registry['version'] != 1 or not isinstance(registry['modules'], dict) or not registry['modules']:
        raise ValueError('Unsupported business documentation registry')

    expected = {route['domain'] for route in routes['routes']}
    listed = Counter()
    problems = []
    modules = registry['modules']
    index = (business / 'README.md').read_text(encoding='utf-8')
    release = (business / 'release-matrix.md').read_text(encoding='utf-8')
    for module, domains in modules.items():
        if not MODULE_ID.fullmatch(module) or not isinstance(domains, list) or not all(
                isinstance(domain, str) and domain for domain in domains):
            problems.append(f'Invalid business module declaration: {module}')
            continue
        listed.update(domains)
        for document in ('README.md', 'learning.md'):
            relative = f'{module}/{document}'
            path = business / relative
            if not path.is_file() or not path.read_text(encoding='utf-8').startswith('# '):
                problems.append(f'Missing titled business document: {relative}')
            if f']({relative})' not in index:
                problems.append(f'Document omitted from the business index: {relative}')
        if f']({module}/README.md)' not in release:
            problems.append(f'Module omitted from the release matrix: {module}')

    for domain in sorted(set(listed) ^ expected):
        status = 'documentation missing' if domain in expected else 'unknown HTTP domain'
        problems.append(f'{domain}: {status}')
    for domain, count in sorted(listed.items()):
        if count > 1:
            problems.append(f'{domain}: listed {count} times')
    for folder in business.iterdir():
        if folder.is_dir() and folder.name not in modules:
            problems.append(f'Unregistered business documentation: {folder.name}')
    return {'passed': not problems, 'modules': len(modules), 'httpDomains': len(expected), 'problems': problems}


if __name__ == '__main__':
    result = check(ROOT)
    print(json.dumps(result, ensure_ascii=False))
    raise SystemExit(not result['passed'])
