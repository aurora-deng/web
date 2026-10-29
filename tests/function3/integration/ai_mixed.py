"""Exercise existing ONNX inference alongside ordinary chat; no model download/training.

Only the explicitly seeded private runtime database is used. This is an integration
and resource isolation check, not model answer-quality evaluation or a capacity claim.
"""
import argparse
import json
from pathlib import Path
import sys
import time
import uuid
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'benchmarks'))
from phase15_business_benchmark import BusinessBenchmark,ResourceSampler
from runtime_blackbox import BrowserClient,expect,sql,wait_for

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--ai-user-id',type=int,required=True)
p.add_argument('--server-pid',type=int,required=True)
p.add_argument('--output',type=Path,required=True)
selected=p.parse_args()
assert selected.ai_user_id>0 and selected.server_pid>0
assert sql('SELECT current_database()')=='function3_runtime_test'
# 私有库夹具：通过真实后台 API 试装、发布已有模型，不生成虚假的质量分数。
admin=BrowserClient('127.0.0.1',18094)
admin.register_and_login('f3model_'+uuid.uuid4().hex[:8],'Function3-test-only!')
sql("INSERT INTO site_roles VALUES("+str(admin.user_id)+",'administrator')")
node,version=sql(f"SELECT n.id,v.id FROM model_bindings b JOIN model_nodes n ON n.id=b.model_node_id JOIN model_versions v ON v.node_id=n.id WHERE b.ai_user_id={selected.ai_user_id} ORDER BY v.id DESC LIMIT 1").split('|')
started=time.monotonic()
job=expect(admin,'POST',f'/api/admin/models/{node}/versions/{version}',{'action':'activate','reason':'隔离验证：真实已安装模型的冷试装'},202)
state=wait_for(lambda:expect(admin,'GET','/api/admin/model-operations/'+job['operationId']),lambda value:value['state']!='pending',180)
assert state['state']=='completed',state
activation={'operationId':job['operationId'],'seconds':round(time.monotonic()-started,3),'state':state['state']}
args=SimpleNamespace(host='127.0.0.1',port=18094,timeout=120,workers=2,
    ai_user_id=selected.ai_user_id,ai_jobs=1,ai_concurrency=1,
    ai_mixed_jobs=1,ai_mixed_concurrency=1,ai_mixed_chat_operations=200,
    ai_max_events=2048,ai_prompt='只回答一个词：你好')
benchmark=BusinessBenchmark(args);sampler=ResourceSampler(selected.server_pid)
before=benchmark.application_metrics();sampler.start()
try:
    benchmark.benchmark_ai()
    benchmark.benchmark_ai_mixed()
    report={'model':'existing phi3-mini-4k-instruct-cpu-int4','qualityEvaluation':False,'activation':activation,
            'results':benchmark.results,'resources':sampler.stop(),
            'applicationMetrics':{'before':before,'after':benchmark.application_metrics()}}
    selected.output.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
    print(json.dumps(report,ensure_ascii=False))
    assert all(item['errors']==0 for item in benchmark.results),'AI/chat integration failed'
finally:
    for user in benchmark.users:user.api.close()
