import test from 'node:test';
import assert from 'node:assert/strict';
import {DashboardQueries} from '../../../frontend/observability/dashboard_queries.js';
const flush=()=>new Promise(resolve=>setImmediate(resolve));
const deferred=()=>{let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject};};

test('metric refreshes coalesce slow optional reads without cancelling or starving them',async()=>{
  const calls=[];
  const loader=new DashboardQueries((path,args,signal)=>{
    const task=deferred();calls.push({path,args,signal,...task});return task.promise;
  },{overview:1,alerts:1,version:1,markers:1});
  loader.refresh({from:10,to:20});await flush();
  loader.refresh({from:11,to:21});loader.refresh({from:12,to:22});await flush();
  assert.equal(calls.length,4);assert(calls.every(row=>!row.signal.aborted));
  let newest=0;loader.listen(()=>newest++);
  for(const row of calls)row.resolve(row.path==='alerts'?[]:{reading:0});
  await flush();
  assert(newest>=4);assert([...loader.states.values()].every(state=>state.loaded&&!state.pending&&!state.error));
  assert.deepEqual(calls.find(row=>row.path==='markers').args,{from:10,to:20});
  loader.refresh({from:15,to:25});await flush();
  assert.equal(calls.length,8);
  assert.deepEqual(calls[7].args,{from:15,to:25});loader.dispose();
});

test('a failed source retries alone while successful zero and empty results remain real data',async()=>{
  const counts={};let failed=true;
  const loader=new DashboardQueries(async path=>{
    counts[path]=(counts[path]||0)+1;
    if(path==='alerts'&&failed)throw new Error('source unavailable');
    return path==='alerts'?[]:0;
  },{overview:1,alerts:1});
  loader.refresh({from:1,to:2});await flush();
  assert.equal(loader.states.get('overview').value,0);
  assert.equal(loader.states.get('alerts').error,'source unavailable');
  failed=false;loader.retry({from:1,to:2});await flush();
  assert.deepEqual(counts,{overview:1,alerts:2});
  assert.deepEqual(loader.states.get('alerts').value,[]);loader.dispose();
});

test('dispose aborts transport and ignores late responses even if transport ignores cancellation',async()=>{
  const task=deferred();let calls=0,notifications=0,signal;
  const loader=new DashboardQueries((_path,_args,token)=>{calls++;signal=token;return task.promise;},{version:1});
  loader.listen(()=>notifications++);loader.refresh({from:1,to:2});await flush();
  loader.dispose();const before=notifications;assert(signal.aborted);
  task.resolve({revision:'old'});await flush();loader.refresh({from:2,to:3});await flush();
  assert.equal(calls,1);assert.equal(notifications,before);assert.equal(loader.states.get('version').loaded,false);
});

test('synchronous failures become retryable state and disposing before dispatch prevents network work',async()=>{
  let calls=0;
  const loader=new DashboardQueries(()=>{calls++;throw new Error('sync');},{version:1});
  loader.refresh({from:1,to:2});await flush();
  assert.equal(loader.states.get('version').error,'sync');loader.dispose();
  const cancelled=new DashboardQueries(()=>{calls++;},{version:1});
  cancelled.refresh({from:1,to:2});cancelled.dispose();await flush();assert.equal(calls,1);
});
