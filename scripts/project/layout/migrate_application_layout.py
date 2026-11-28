"""Reviewed application layout migration; planning is read-only by default.

Apply only with the printed plan digest and a new verified backup archive inside
this checkout's evidence parent. Never connects, builds, commits or deploys.
"""
import argparse
import fnmatch
import io
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = 'docs/architecture/source-layout-migration-3.json'
MODULES = 'backend/application/modules'
MOVES = []

def component(old, module, layer, name=None):
    MOVES.append((old, f'{MODULES}/{module}/{layer}/{name or Path(old).name}'))

for module, names in {
    'accounts': ['AccountTypes'], 'contacts': ['SocialTypes'],
    'chat': ['ConversationTypes', 'MessageTypes'], 'files': ['FileTypes'],
    'messaging': ['OutboxTypes'], 'shared': ['IdentityTypes'],
    'ai': ['EvaluationTypes', 'GenerationTypes', 'KnowledgeTypes', 'ModelTypes', 'TrainingTypes'],
}.items():
    for name in names:
        component(f'backend/application/domain/{name}', module, 'domain')
for module, names in {
    'accounts': ['ICredentialCodec', 'ILoginRateLimiter', 'ISessionRepository', 'IUserRepository'],
    'contacts': ['ISocialRepository'], 'chat': ['IConversationRepository', 'IMessageRepository'],
    'files': ['IFileRepository', 'IFileStorage'], 'messaging': ['IOutboxRepository'],
    'shared': ['IDatabase', 'ITransaction'],
    'ai': ['IAiGenerationAuditRepository', 'IKnowledgeIndex', 'IKnowledgeRepository',
           'IModelEvaluationRepository', 'IModelQualityGate', 'IModelRepository',
           'IRagContextProvider', 'ITrainingRepository'],
}.items():
    for name in names:
        component(f'backend/application/ports/{name}', module, 'ports')
for name in ['AiChatService', 'AiGenerationAudit', 'AiGenerationCompletion',
             'AiGenerationPreparation', 'AiGenerationState']:
    component(f'backend/application/business/{name}', 'ai', 'application')
component('backend/application/business/OutboxDispatcher', 'messaging', 'application')
component('backend/application/business/ApplicationError', 'shared', 'domain')
for module, name in [('accounts', 'IdentityRuntime'), ('contacts', 'ContactsRuntime'),
                     ('chat', 'ChatRuntime'), ('chat', 'RealtimeRuntime'), ('files', 'FileRuntime')]:
    component(f'backend/application/bootstrap/{name}', module, 'bootstrap')
for module, name in [('accounts', 'Phase11AuthHttp'), ('files', 'Phase11FileHttp'),
                     ('ai', 'Phase11KnowledgeHttp'), ('ai', 'Phase11TrainingHttp'),
                     ('chat', 'Phase11SocialChatHttp')]:
    component(f'backend/application/transport/http/{name}', module, 'transport')
for name in ['BusinessHttpCodec', 'BusinessHttpExecutor', 'BusinessHttpObservation']:
    component(f'backend/application/transport/http/{name}', 'shared', 'transport')
for old, module, name in [('FlatJson', 'shared', 'FlatJson'), ('Contracts', 'shared', 'Contracts'),
                          ('serialization/MessageJson', 'chat', 'MessageJson'),
                          ('realtime/Phase11Realtime', 'chat', 'Phase11Realtime'),
                          ('learning/LearningRoutes', 'learning', 'LearningRoutes')]:
    component('backend/application/transport/' + old, module, 'transport', name)
for name in ['SodiumCredentialCodec', 'InMemoryLoginRateLimiter']:
    component(f'backend/application/security/{name}', 'accounts', 'infrastructure')
for old, name in [('domain/Domain', 'Domain'), ('ports/Ports', 'Ports'),
                   ('business/BusinessInterfaces', 'BusinessInterfaces')]:
    component('backend/application/' + old, 'shared', 'compatibility', name)
for name in ['BusinessContext', 'TextRules']:
    component(f'{MODULES}/shared/{name}', 'shared', 'domain')
MOVES += [('tools/phase11_model_admin.cpp', 'tools/ai/ModelAdmin/ModelAdmin.cpp'),
          ('tools/phase11_training_admin.cpp', 'tools/ai/TrainingAdmin/TrainingAdmin.cpp')]

# Each definition below is extracted byte-for-byte, not reimplemented.
CONTRACTS = {
    'LoginSession': ('accounts/domain/AuthBusinessTypes', ['accounts/domain/AccountTypes']),
    'RestoredSession': ('accounts/domain/AuthBusinessTypes', ['accounts/domain/AccountTypes']),
    'AiChatRequest': ('ai/domain/AiChatRequest', ['shared/domain/IdentityTypes']),
    'TrainingSubmission': ('ai/domain/TrainingSubmission', ['shared/domain/IdentityTypes']),
    'ConversationParticipant': ('chat/domain/ConversationParticipant', ['chat/domain/ConversationTypes', 'accounts/domain/AccountTypes']),
    'KnowledgeCitationView': ('ai/domain/KnowledgeCitationView', ['ai/domain/KnowledgeTypes']),
    'IAuthBusiness': ('accounts/ports/IAuthBusiness', ['accounts/domain/AuthBusinessTypes']),
    'ISocialBusiness': ('contacts/ports/ISocialBusiness', ['contacts/domain/SocialTypes']),
    'IChatBusiness': ('chat/ports/IChatBusiness', ['chat/domain/ConversationTypes', 'chat/domain/MessageTypes', 'chat/domain/ConversationParticipant']),
    'IFileBusiness': ('files/ports/IFileBusiness', ['files/domain/FileTypes', 'accounts/domain/AccountTypes']),
    'IKnowledgeBusiness': ('ai/ports/IKnowledgeBusiness', ['ai/domain/KnowledgeCitationView']),
    'ITrainingBusiness': ('ai/ports/ITrainingBusiness', ['ai/domain/TrainingTypes', 'ai/domain/TrainingSubmission']),
    'IAiChatBusiness': ('ai/ports/IAiChatBusiness', ['ai/domain/AiChatRequest']),
}
TEXT_SUFFIXES = {'.h', '.cpp', '.c', '.py', '.cmake', '.txt', '.json', '.sh', '.md', '.yml', '.yaml'}
PROTECTED = ('backend/server/', 'artifacts/', 'docs/evidence/', 'docs/archive/',
             'docs/reports/', 'docs/performance/', 'scripts/project/layout/')
BASELINES = ('backend/agent/domain/SkillCatalog/SkillCatalog.generated.inc',
             'frontend/phase11/asset-manifest.json', 'docs/contracts/business-routes.json')


def private_source_name(name):
    """Exclude credential, local-data and generated paths before any file read."""
    parts = Path(name).parts
    if any(part.startswith('.env') for part in parts):
        return True
    if any(part.startswith('build') or part in ('.git', '.ssh', '.aws', '.azure', '.kube', '.codex', '__pycache__', 'runtime-data', 'client-data', 'models') for part in parts[:-1]):
        return True
    if parts and parts[-1].lower() in ('credentials.json', 'secrets.json', 'auth.json'):
        return True
    lowered = name.lower()
    return any(fnmatch.fnmatch(lowered, pattern) for pattern in [
        '*.local.*', '*.key', '*.pem', '*.p12', '*.pfx', '*.sqlite*', '*.db', '*.dump',
        '*/credentials.json', '*/secrets.json', '*/auth.json'])


def sha(data):
    return hashlib.sha256(data).hexdigest()


def bounded(root, relative):
    relative = Path(relative)
    if relative.is_absolute() or '..' in relative.parts:
        raise RuntimeError('Absolute or parent path is outside migration scope')
    root = root.resolve(strict=True)
    path = root / relative
    for part in [path, *path.parents]:
        if part == root:
            break
        if part.is_symlink() or (hasattr(part, 'is_junction') and part.is_junction()):
            raise RuntimeError('Symlink or junction is not a migration target')
    if not path.resolve().is_relative_to(root):
        raise RuntimeError('Migration target escaped checkout')
    return path


def replacements(text, moves=MOVES):
    for old, new in sorted(moves, key=lambda item: -len(item[0])):
        text = text.replace(old + '/' if not Path(old).suffix else old,
                            new + '/' if not Path(old).suffix else new)
    return text


def split_contracts(text):
    definitions = dict(re.findall(r'^(?:class|struct) (\w+)\n(\{.*?^\};)', text, re.M | re.S))
    if set(definitions) != set(CONTRACTS):
        raise RuntimeError('Business contract definition set changed; review before splitting')
    by_file = {}
    for name, (component_path, dependencies) in CONTRACTS.items():
        path = f'{MODULES}/{component_path}/{Path(component_path).name}.h'
        info = by_file.setdefault(path, {'definitions': [], 'dependencies': dependencies})
        kind = 'class' if name.startswith('I') else 'struct'
        definition = kind + ' ' + name + '\n' + definitions[name]
        info['definitions'].append(definition)
    files = {}
    for path, info in by_file.items():
        includes = ''.join(f'#include "{MODULES}/{d}/{Path(d).name}.h"\n' for d in info['dependencies'])
        header = '#pragma once\n' + includes + '\n#include <chrono>\n#include <cstddef>\n#include <optional>\n#include <string>\n#include <string_view>\n#include <vector>\n\n'
        files[path] = (header + 'namespace webserver::phase11 {\n\n' + '\n\n'.join(info['definitions']) + '\n\n} // namespace webserver::phase11\n').encode('utf-8')
    umbrella = '#pragma once\n// Compatibility umbrella for assembly only; prefer the domain business port.\n'
    umbrella += ''.join(f'#include "{MODULES}/{p}/{Path(p).name}.h"\n' for name, (p, _) in CONTRACTS.items() if name.startswith('I'))
    return files, umbrella.encode('utf-8')


def plan(root, git):
    root = root.resolve(strict=True)
    if bounded(root, MANIFEST).exists():
        raise RuntimeError('Migration already applied; do not replay')
    moved = {}
    for old, new in MOVES:
        source, target = bounded(root, old), bounded(root, new)
        if not source.exists() or target.exists():
            raise RuntimeError('Missing source or conflicting destination: ' + old)
        paths = [source] if source.is_file() else sorted(p for p in source.rglob('*') if p.is_file())
        for p in paths:
            bounded(root, p.relative_to(root))
            name = p.relative_to(root).as_posix()
            moved[name] = new if source.is_file() else new + '/' + p.relative_to(source).as_posix()
    result = subprocess.run([git, '-c', 'core.fsmonitor=false', 'ls-files', '--cached', '--others',
                             '--exclude-standard', '-z'], cwd=root, capture_output=True, check=True)
    names = set(filter(None, result.stdout.decode('utf-8').split('\0'))) | set(moved)
    files = {}
    for name in sorted(names):
        if name.startswith(PROTECTED) or private_source_name(name):
            continue
        path = bounded(root, name)
        if not path.is_file() or path.suffix not in TEXT_SUFFIXES:
            continue
        if name.startswith('docs/architecture/source-') or name.startswith('tests/project/fixtures/'):
            continue
        if name.split('/')[0] not in ('backend', 'tests', 'scripts', 'tools', 'examples', 'benchmarks', 'docs') and name not in ('CMakeLists.txt', 'README.md', 'main.cpp'):
            continue
        before = path.read_bytes()
        after = replacements(before.decode('utf-8')).encode('utf-8')
        if before != after or name in moved:
            files[name] = {'before': before, 'after': after, 'destination': moved.get(name, name)}
    aggregate = 'backend/application/business/BusinessInterfaces/BusinessInterfaces.h'
    generated, umbrella = split_contracts(files[aggregate]['before'].decode('utf-8').replace('\r\n', '\n'))
    files[aggregate]['after'] = umbrella
    for name in generated:
        if bounded(root, name).exists():
            raise RuntimeError('New business contract destination exists: ' + name)
    # Existing leaves no longer pull in every business domain through the umbrella.
    symbols = {name: f'{MODULES}/{p}/{Path(p).name}.h' for name, (p, _) in CONTRACTS.items()}
    umbrella_path = f'{MODULES}/shared/compatibility/BusinessInterfaces/BusinessInterfaces.h'
    for entry in files.values():
        text = entry['after'].decode('utf-8')
        literal = '#include "' + umbrella_path + '"'
        if literal in text and entry['destination'].endswith(('.h', '.cpp')):
            needed = sorted({header for name, header in symbols.items() if re.search(r'\b' + name + r'\b', text)})
            if needed:
                text = text.replace(literal, '\n'.join('#include "' + h + '"' for h in needed))
                entry['after'] = text.encode('utf-8')
    document = {'schema': 'application-domain-layout-v1', 'date': '2026-11-27', 'moves': MOVES,
                'frozenCoreUnchanged': True, 'historicalEvidenceRewritten': False,
                'behaviorChangeIntended': False, 'contractDefinitions': {},
                'generated': [{'path': n, 'sha256': sha(b)} for n, b in sorted(generated.items())],
                'preservedArtifacts': {name: sha(bounded(root, name).read_bytes()) for name in BASELINES},
                'files': [{'old': n, 'new': v['destination'], 'beforeSha256': sha(v['before']),
                           'afterSha256': sha(v['after'])} for n, v in sorted(files.items())]}
    old_text = files[aggregate]['before'].decode('utf-8').replace('\r\n', '\n')
    for full, name in re.findall(r'^((?:class|struct) (\w+)\n\{.*?^\};)', old_text, re.M | re.S):
        document['contractDefinitions'][name] = sha(full.encode('utf-8'))
    return files, generated, document


def apply(root, backup, expected_digest, files, generated, document, *, moves=MOVES, manifest=MANIFEST):
    root = root.resolve(strict=True)
    digest = sha(json.dumps(document, sort_keys=True).encode())
    if digest != expected_digest:
        raise RuntimeError('Plan digest changed; review before applying')
    backup = Path(backup)
    if backup.exists() or backup.is_symlink() or backup.resolve().parent != root.parent:
        raise RuntimeError('Backup must be new and inside this checkout evidence parent')
    for name, entry in files.items():
        if bounded(root, name).read_bytes() != entry['before']:
            raise RuntimeError('Concurrent source change: ' + name)
        destination = entry['destination']
        if destination != name and bounded(root, destination).exists():
            raise RuntimeError('Concurrent destination conflict: ' + destination)
    for name in [manifest, *generated]:
        will_move = name in files and files[name]['destination'] != name
        if bounded(root, name).exists() and not will_move:
            raise RuntimeError('Concurrent generated destination conflict: ' + name)
    # A verified backup exists before any source write or directory move.
    with tarfile.open(backup, 'x:gz') as archive:
        for name, entry in sorted(files.items()):
            info = tarfile.TarInfo(name); info.size = len(entry['before'])
            archive.addfile(info, io.BytesIO(entry['before']))
    with tarfile.open(backup, 'r:gz') as archive:
        for name, entry in files.items():
            if archive.extractfile(name).read() != entry['before']:
                raise RuntimeError('Backup verification failed')
    for name, entry in files.items():
        path = bounded(root, name)
        if path.read_bytes() != entry['before']:
            raise RuntimeError('Concurrent source change after backup: ' + name)
    for name, entry in files.items():
        if entry['before'] != entry['after']:
            bounded(root, name).write_bytes(entry['after'])
    for old, new in moves:
        source, target = bounded(root, old), bounded(root, new)
        target.parent.mkdir(parents=True, exist_ok=True)
        source.rename(target)
    for name, content in generated.items():
        path = bounded(root, name); path.parent.mkdir(parents=True, exist_ok=True)
        with path.open('xb') as handle:
            handle.write(content)
    bounded(root, manifest).write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    for row in document['files']:
        if sha(bounded(root, row['new']).read_bytes()) != row['afterSha256']:
            raise RuntimeError('Post-migration bytes differ: ' + row['new'])
    print('APPLICATION_LAYOUT_APPLIED_WITH_VERIFIED_BACKUP')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--git', default='git')
    parser.add_argument('--apply', action='store_true')
    parser.add_argument('--backup', type=Path)
    parser.add_argument('--expected-plan-sha256')
    args = parser.parse_args()
    files, generated, document = plan(ROOT, args.git)
    digest = sha(json.dumps(document, sort_keys=True).encode())
    print(json.dumps({'planSha256': digest, 'components': len(MOVES), 'existingFiles': len(files),
                      'generatedContractHeaders': len(generated), 'applied': False}))
    if args.apply:
        if args.backup is None:
            parser.error('--apply requires --backup')
        apply(ROOT, args.backup, args.expected_plan_sha256, files, generated, document)


if __name__ == '__main__':
    main()
