"""Reject driver/protocol includes in Function3 business code and misplaced components."""
from pathlib import Path
import re

root=Path(__file__).resolve().parents[3]
modules=root/'backend/application/modules'
errors=[]
for file in modules.rglob('*'):
    if file.suffix not in ('.h','.cpp'):continue
    parts=file.relative_to(modules).parts
    if file.parent.name!=file.stem and file.name not in ('BusinessHttp.h',):
        errors.append(f'{file.relative_to(root)}: component and directory must share a name')
    business=any(part in ('application','domain','ports') for part in parts)
    if not business:continue
    for include in re.findall(r'^\s*#include\s*[<"]([^>"]+)',file.read_text(encoding='utf-8'),re.M):
        if any(token in include for token in ('/infrastructure/','/transport/','libpq','sherpa-onnx','rocketmq/',
                'backend/server/http/','backend/server/http2/','backend/server/websocket/','backend/server/sse/','backend/server/Reactor','nghttp2/')):
            errors.append(f'{file.relative_to(root)}: business depends on driver/transport: {include}')
for file in modules.rglob('*.cpp'):
    if file.stat().st_size==0:errors.append(f'{file.relative_to(root)}: empty implementation')
# 私人推理不能悄悄复用会落库的旧 AI 聊天/训练管道。
private=root/'backend/ai/private'
for file in private.rglob('*'):
    if file.suffix not in ('.h','.cpp'):continue
    content=file.read_text(encoding='utf-8')
    for forbidden in ('/AiChatService/','/ChatService/','/TrainingDataService/',
                      'appendMessage(', 'appendOutbox(', 'upsertGenerationAudit('):
        if forbidden in content:errors.append(f'{file.relative_to(root)}: private inference persistence dependency: {forbidden}')
if errors:raise SystemExit('\n'.join(errors))
print('Function3 component naming and business/driver dependency boundaries passed')
