import test from 'node:test';
import assert from 'node:assert/strict';
import { contactsApi } from '../../../frontend/phase11/contacts/ContactsApi/ContactsApi.js';
import { chatApi } from '../../../frontend/phase11/chat/ChatApi/ChatApi.js';
import { activitiesApi } from '../../../frontend/phase11/activities/ActivitiesApi/ActivitiesApi.js';
import { webAiApi } from '../../../frontend/phase11/ai/WebAiApi/WebAiApi.js';
import { normalizeApplicationRequest } from '../../../frontend/phase11/shared/ApiContracts/ApiContracts.js';
import { BusinessApi } from '../../../frontend/phase11/shared/BusinessApi/BusinessApi.js';

test('domain bindings encode parameter values and retain decimal strings without precision loss',()=>{
  assert.equal(contactsApi.deleteFriendsId({id:'9007199254740993'}).path,'/api/friends/9007199254740993');
  assert.equal(webAiApi.getAiWebGenerationsGeneration({generation:'中文/a?b'}).path,'/api/ai/web/generations/%E4%B8%AD%E6%96%87%2Fa%3Fb');
  assert.throws(()=>contactsApi.deleteFriendsId({}),/参数缺失/);
});
test('literal collection resources resolve ahead of generic identifier routes',()=>{
  const input=activitiesApi.getActivitiesFavorites({},new URLSearchParams({before:'12'}));
  assert.equal(normalizeApplicationRequest(input).contract.operation,'getActivitiesFavorites');
  assert.equal(input.path,'/api/activities/favorites?before=12');
});
test('method, route and domain mismatch fail before network access',async()=>{
  const original=globalThis.fetch;let calls=0;globalThis.fetch=async()=>{calls++;return new Response('{}');};
  try{
    const api=new BusinessApi();
    await assert.rejects(api.request(contactsApi.getFriends(),{method:'DELETE'}),/方法/);
    await assert.rejects(api.request('/api/not-registered'),/未登记/);
    await assert.rejects(api.request('https://untrusted.invalid/api/me'),/站内/);
    await assert.rejects(api.request({...contactsApi.getFriends(),domain:'admin'}),/领域/);
    assert.equal(calls,0);
  }finally{globalThis.fetch=original;}
});
test('descriptor method drives CSRF while body and cancellation are preserved',async()=>{
  const original=globalThis.fetch;const controller=new AbortController();let seen;
  globalThis.fetch=async(path,options)=>{seen={path,options};return new Response('{"version":"2"}');};
  try{
    const api=new BusinessApi();api.csrf='fixture';
    const body={notificationsMuted:'true'};
    const result=await api.request(chatApi.patchConversationsIdPreferences({id:'17'}),{data:body,signal:controller.signal});
    assert.equal(seen.path,'/api/conversations/17/preferences');assert.equal(seen.options.method,'PATCH');
    assert.equal(seen.options.headers.get('X-CSRF-Token'),'fixture');assert.equal(seen.options.body,JSON.stringify(body));
    assert.equal(seen.options.signal,controller.signal);assert.equal(result.version,'2');
  }finally{globalThis.fetch=original;}
});
test('operational reads and legacy request aliases retain their registered contracts',()=>{
  assert.equal(normalizeApplicationRequest('/health/ready').contract.domain,'operations');
  assert.equal(normalizeApplicationRequest('/api/friends',{method:'POST'}).contract.operation,'postFriends');
  assert.equal(normalizeApplicationRequest('/api/ai/web/models').contract.operation,'getAiWebModels');
});
