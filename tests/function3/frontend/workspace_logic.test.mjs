import test from 'node:test';
import assert from 'node:assert/strict';
import { groupMessages } from '../../../frontend/phase11/chat/MessageGrouping/MessageGrouping.js';
import { ChatHistory } from '../../../frontend/phase11/chat/ChatHistory.js';
import { BusinessApi } from '../../../frontend/phase11/shared/BusinessApi/BusinessApi.js';
import { PreferenceSnapshots } from '../../../frontend/phase11/chat/PreferenceSnapshots/PreferenceSnapshots.js';
test('old list/event cannot reverse a newer per-member preference; logout drops previous account',()=>{
  const preferences=new PreferenceSnapshots();
  assert(preferences.absorb('1',true,'2'));
  assert(preferences.absorb('1',false,'1'));
  assert.equal(preferences.absorb('1',false,'3'),false);
  assert.equal(preferences.absorb('1',true,'2'),false);
  preferences.clear();assert(preferences.absorb('1',true,'1'));
});
test('grouping uses data and five-minute/system/day boundaries, keeps one row per message', () => {
  const t=new Date(2026,9,28,12).getTime();
  const rows=[0,1000,301000,302000,303000,86400000].map((d,i)=>({id:i+1,senderId:1,createdAt:t+d,state:i===3?'recalled':'active'}));
  const result=groupMessages(rows);
  assert.deepEqual(result.map(m=>m.grouped),[false,true,false,false,false,false]);
  assert.equal(result.length,rows.length);assert(result[5].dateBreak);
});
test('search window preserves synchronized cursor and recall terminal state',async()=>{
  const h=new ChatHistory(async()=>[98,99,100].map(sequence=>({id:sequence,sequence,conversationId:1,state:'active'})));
  await h.open(1);h.merge({id:99,sequence:99,conversationId:1,state:'recalled',body:''});
  assert(h.focusContext([98,99].map(sequence=>({id:sequence,sequence,conversationId:1,state:'active',body:'old'}))));
  assert.equal(h.cursor,100);assert.equal(h.latest,99);assert(h.hasNewer);assert.equal(h.records.get('99').state,'recalled');
  assert.equal(h.focusContext([],2),false);
});
test('request errors distinguish definitive failure, abort and uncertain writes without retry',async()=>{
  const original=globalThis.fetch;let calls=0;
  try{
    const api=new BusinessApi();globalThis.fetch=async()=>{calls++;throw new TypeError('offline');};
    await assert.rejects(api.request('/api/x',{method:'POST',data:{a:1}}),e=>e.uncertain===true);assert.equal(calls,1);
    globalThis.fetch=async()=>{throw new DOMException('cancelled','AbortError');};
    await assert.rejects(api.request('/api/x'),e=>e.name==='AbortError');
    globalThis.fetch=async()=>new Response('{"message":"denied"}',{status:403});
    await assert.rejects(api.request('/api/x'),e=>e.status===403&&!e.uncertain);
  }finally{globalThis.fetch=original;}
});
