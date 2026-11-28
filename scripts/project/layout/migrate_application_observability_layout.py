'''Group application observability C++ components by responsibility.

Read-only by default. Applying requires an exact reviewed digest and a new
verified backup. No server, database, network or generated runtime data is
read during planning.
'''
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('app_observability_layout_safety',Path(__file__).with_name('migrate_application_layout.py'))
safety=importlib.util.module_from_spec(spec);spec.loader.exec_module(safety)
MANIFEST='docs/architecture/source-application-observability-migration.json'
PREFIX='backend/application/observability'
MOVES=[
    (f'{PREFIX}/OperationCatalog/OperationCatalog.def',f'{PREFIX}/catalog/OperationCatalog.def'),
    (f'{PREFIX}/OperationCatalog/OperationCatalog.h',f'{PREFIX}/catalog/OperationCatalog.h'),
    (f'{PREFIX}/BusinessLog/BusinessLog.cpp',f'{PREFIX}/logging/BusinessLog.cpp'),
    (f'{PREFIX}/BusinessLog/BusinessLog.h',f'{PREFIX}/logging/BusinessLog.h'),
    (f'{PREFIX}/BusinessMetrics/BusinessMetrics.h',f'{PREFIX}/metrics/BusinessMetrics.h'),
    (f'{PREFIX}/BusinessMetricsFormatter/BusinessMetricsFormatter.cpp',f'{PREFIX}/metrics/BusinessMetricsFormatter.cpp'),
    (f'{PREFIX}/BusinessMetricsFormatter/BusinessMetricsFormatter.h',f'{PREFIX}/metrics/BusinessMetricsFormatter.h'),
    (f'{PREFIX}/BusinessOperation/BusinessOperation.cpp',f'{PREFIX}/operations/BusinessOperation.cpp'),
    (f'{PREFIX}/BusinessOperation/BusinessOperation.h',f'{PREFIX}/operations/BusinessOperation.h'),
    (f'{PREFIX}/ChatAckTrace/ChatAckTrace.h',f'{PREFIX}/tracing/ChatAckTrace.h'),
    (f'{PREFIX}/CommitWakeClock/CommitWakeClock.h',f'{PREFIX}/runtime/CommitWakeClock.h'),
    (f'{PREFIX}/DetachedSpan/DetachedSpan.cpp',f'{PREFIX}/tracing/DetachedSpan.cpp'),
    (f'{PREFIX}/DetachedSpan/DetachedSpan.h',f'{PREFIX}/tracing/DetachedSpan.h'),
    (f'{PREFIX}/Telemetry/Telemetry.cpp',f'{PREFIX}/tracing/Telemetry.cpp'),
    (f'{PREFIX}/Telemetry/Telemetry.h',f'{PREFIX}/tracing/Telemetry.h'),
]
REWRITES={old:new for old,new in MOVES}
for old_dir,new_dir in (
    (f'{PREFIX}/OperationCatalog',f'{PREFIX}/catalog'),
    (f'{PREFIX}/BusinessLog',f'{PREFIX}/logging'),
    (f'{PREFIX}/BusinessMetricsFormatter',f'{PREFIX}/metrics'),
    (f'{PREFIX}/BusinessMetrics',f'{PREFIX}/metrics'),
    (f'{PREFIX}/BusinessOperation',f'{PREFIX}/operations'),
    (f'{PREFIX}/ChatAckTrace',f'{PREFIX}/tracing'),
    (f'{PREFIX}/CommitWakeClock',f'{PREFIX}/runtime'),
    (f'{PREFIX}/DetachedSpan',f'{PREFIX}/tracing'),
    (f'{PREFIX}/Telemetry',f'{PREFIX}/tracing'),
):
    REWRITES.setdefault(old_dir,new_dir)


def rewrite(text):
    for old,new in sorted(REWRITES.items(),key=lambda item:-len(item[0])):
        text=text.replace(old,new)
    return text


def semantic_digest(text):
    lines=[line for line in text.splitlines(keepends=True) if not line.lstrip().startswith('#include')]
    return safety.sha(''.join(lines).encode('utf-8'))


def plan(root,git):
    if safety.bounded(root,MANIFEST).exists(): raise RuntimeError('Application observability migration already applied; do not replay')
    names=subprocess.check_output([git,'-c','core.fsmonitor=false','ls-files','--cached','--others','--exclude-standard','-z'],cwd=root).decode('utf-8').split('\0')
    moved=dict(MOVES);files={};semantic={};references={}
    for old,new in MOVES:
        if not safety.bounded(root,old).is_file() or safety.bounded(root,new).exists(): raise RuntimeError('Missing source or destination conflict: '+old)
    suffixes={'.h','.cpp','.def','.cmake','.txt','.md','.json','.py','.sh'}
    for name in sorted(set(names)|set(moved)):
        if not name or name.startswith(safety.PROTECTED) or safety.private_source_name(name): continue
        if name.startswith(('backend/server/','docs/architecture/source-','tests/project/fixtures/')): continue
        if not name.endswith(tuple(suffixes)) and name!='CMakeLists.txt': continue
        path=safety.bounded(root,name)
        if not path.is_file(): continue
        before=path.read_bytes();text=before.decode('utf-8');after_text=rewrite(text)
        if name in moved:
            if semantic_digest(after_text)!=semantic_digest(text): raise RuntimeError('C++ behavior changed: '+name)
            semantic[moved[name]]=semantic_digest(text)
        elif after_text!=text:
            references[name]=[old for old in REWRITES if old in text]
        if after_text!=text or name in moved:
            files[name]={'before':before,'after':after_text.encode('utf-8'),'destination':moved.get(name,name)}
    generated={
        'backend/application/observability/README.md': '# 应用层可观测性\n\n本目录提供固定操作目录、业务日志、指标和链路追踪支持，不承接 HTTP 路由、数据库业务或进程启停。\n\n| 目录 | 职责 |\n| --- | --- |\n| `catalog/` | 固定操作名称、标签与声明 |\n| `logging/` | 业务日志格式和输出 |\n| `metrics/` | 业务指标与文本格式化 |\n| `operations/` | 业务操作生命周期和关联上下文 |\n| `tracing/` | Trace、Span、聊天 ACK 追踪与遥测 |\n| `runtime/` | 提交唤醒等运行时协调工具 |\n\n目录迁移只调整 include、CMake 和工具引用，不改变指标语义、日志字段或资源所有权。`backend/server/` 保持冻结。精确迁移记录见 `docs/architecture/source-application-observability-migration.json`。\n'.encode('utf-8'),
        'docs/architecture/overview/application-observability-layout.md': '# 应用观测目录规范\n\n`backend/application/observability/` 按 `catalog`、`logging`、`metrics`、`operations`、`tracing` 和 `runtime` 划分职责，不以同名类目录重复嵌套，也不把观测工具混入业务模块。\n\n## 边界\n\n- `catalog` 维护固定操作标识；Python Observer 读取同一份定义。\n- `logging` 管理业务日志，不处理业务事务。\n- `metrics` 管理指标与格式化，不承担采样服务的生命周期。\n- `operations` 负责业务操作状态与关联 Span。\n- `tracing` 提供 Trace、Span 和 ACK 追踪支持。\n- `runtime` 只保留运行时协调工具，不扩展为杂项工具箱。\n\n迁移同步更新 include、CMake、测试、运行包和 Observer 引用。历史迁移清单保持原样，后续合法路径变化通过显式迁移链校验；不得忽略任意路径或重写旧摘要来通过测试。\n\n`backend/server/` 不属于此次整理范围。源码检查不能代替完整 C++ 编译；服务和性能验收以 Linux 隔离构建与运行证据为准。\n'.encode('utf-8'),
    }
    for name,data in generated.items():
        if safety.bounded(root,name).exists(): raise RuntimeError('Generated destination exists: '+name)
    doc={'schema':'application-observability-layout-v1','date':'2026-11-28','moves':MOVES,
         'behaviorChangeIntended':False,'semanticSha256':semantic,'referenceFiles':references,
         'frozenCoreUnchanged':True,'historicalEvidenceRewritten':False,
         'files':[{'old':n,'new':e['destination'],'beforeSha256':safety.sha(e['before']),'afterSha256':safety.sha(e['after'])} for n,e in sorted(files.items())],
         'generated':[{'path':n,'sha256':safety.sha(d)} for n,d in sorted(generated.items())]}
    return files,generated,doc


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--git',default='git');p.add_argument('--apply',action='store_true');p.add_argument('--backup',type=Path);p.add_argument('--expected-plan-sha256');a=p.parse_args()
    files,generated,doc=plan(ROOT,a.git);digest=safety.sha(json.dumps(doc,sort_keys=True).encode())
    print(json.dumps({'planSha256':digest,'movedComponents':len(MOVES),'modifiedFiles':len(files),'semanticMatches':len(doc['semanticSha256'])}))
    if a.apply:
        if a.backup is None:p.error('--apply requires --backup')
        safety.apply(ROOT,a.backup,a.expected_plan_sha256,files,generated,doc,moves=MOVES,manifest=MANIFEST)
        print('APPLICATION_OBSERVABILITY_LAYOUT_APPLIED_WITH_VERIFIED_BACKUP')

if __name__=='__main__':main()
