"""Same authenticated 24-fruit page, fixed fixture, sequential keep-alive reads.

Only for the existing isolated benchmark server. Does not configure a database,
load an AI model, install dependencies, or change the daily service.
"""
import argparse
import json
from pathlib import Path
import time
import uuid
from phase15_business_benchmark import ApiClient, percentile

p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
client=ApiClient('127.0.0.1',18094,20)
try:
    username='treeperf_'+uuid.uuid4().hex[:12];client.register(username,'Tree-perf-only!');client.login(username,'Tree-perf-only!')
    status,categories=client.request('GET','/api/tree/categories');assert status==200
    for i in range(24):
        status,value=client.request('POST','/api/tree/posts',{'requestId':'page-'+str(i),'body':'一页24枚果实的相同测试内容','categoryId':categories[0]['id'],'tags':'生活\n性能'})
        assert status==201,(status,value)
    samples=[]
    for i in range(70):
        start=time.perf_counter();status,page=client.request('GET','/api/tree/posts?limit=24')
        assert status==200 and len(page['posts'])==24,(status,page)
        assert all(row['tags']==['性能','生活'] or sorted(row['tags'])==sorted(['生活','性能']) for row in page['posts'])
        if i>=10:samples.append((time.perf_counter()-start)*1000)
    result={'samples':len(samples),'rows':24,'warmup':10,'errors':0,'latencyMs':{f'p{int(q*100)}':round(percentile(samples,q),3) for q in (.5,.95,.99)}}
    a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(json.dumps(result))
finally:client.close()
