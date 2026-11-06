import test from 'node:test';
import assert from 'node:assert/strict';
import { fetchAzureVoices } from '../../../frontend/phase11/speech/AzureVoiceCatalog/AzureVoiceCatalog.js';
import { WebSpeechApi } from '../../../frontend/phase11/speech/WebSpeechApi/WebSpeechApi.js';
import { cloudRequest,pcmWave } from '../../../frontend/phase11/speech/WebSpeechApi/CloudSpeech.js';

const profile={provider:'azure',voice:'zf_001',style:'calm',region:'eastasia',speed:.95,pitch:-2,freeTierConfirmed:true};
const row={ShortName:'zh-CN-XiaomoNeural',LocalName:'晓墨',Locale:'zh-CN',Gender:'Female'};
const storage=()=>{const data=new Map();return {data,getItem:k=>data.get(k),setItem:(k,v)=>data.set(k,v)};};

test('Azure voice lookup sends the owner key only to the fixed regional host',async()=>{
  let call;
  const voices=await fetchAzureVoices(profile,'test-only',{fetcher:async(url,options)=>{
    call={url,options};return Response.json([row,row,{...row,ShortName:'en-US-JennyNeural',Locale:'en-US'},null]);
  }});
  assert.equal(voices.length,1);assert.equal(voices[0].name,'晓墨');
  assert.equal(call.url,'https://eastasia.tts.speech.microsoft.com/cognitiveservices/voices/list');
  assert.equal(call.options.headers['Ocp-Apim-Subscription-Key'],'test-only');
  assert.equal(call.options.credentials,'omit');assert.equal(call.options.redirect,'error');
  assert.equal(call.options.referrerPolicy,'no-referrer');assert(!JSON.stringify(voices).includes('test-only'));
});

test('voice lookup rejects arbitrary regions, invalid keys, errors and oversized metadata',async()=>{
  let calls=0;const fetcher=async()=>{calls++;return Response.json([row]);};
  await assert.rejects(fetchAzureVoices({...profile,region:'eastasia.evil/path'},'key',{fetcher}),/区域/);
  await assert.rejects(fetchAzureVoices(profile,'key\nvalue',{fetcher}),/Key/);assert.equal(calls,0);
  for(const [status,pattern] of [[401,/Key/],[429,/限流/],[500,/500/]])
    await assert.rejects(fetchAzureVoices(profile,'key',{fetcher:async()=>new Response('',{status})}),pattern);
  await assert.rejects(fetchAzureVoices(profile,'key',{fetcher:async()=>new Response('x'.repeat(1024*1024+1))}),/大小限制/);
  await assert.rejects(fetchAzureVoices(profile,'key',{fetcher:async()=>Response.json({})}),/支持范围/);
  await assert.rejects(fetchAzureVoices(profile,'key',{fetcher:async()=>Response.json([{...row,ShortName:'bad"name'}])}),/未返回/);
});

test('cancelled voice lookup neither starts nor accepts a late response',async()=>{
  const c=new AbortController();c.abort();let called=false;
  await assert.rejects(fetchAzureVoices(profile,'key',{signal:c.signal,fetcher:async()=>{called=true;}}));assert(!called);
  const d=new AbortController();let finish;
  const pending=fetchAzureVoices(profile,'key',{signal:d.signal,fetcher:()=>new Promise(resolve=>finish=resolve)});
  d.abort();finish(Response.json([row]));await assert.rejects(pending);
});

test('web voice lookup isolates accounts and never persists the Azure key',async()=>{
  const disk=storage(),fetcher=async()=>Response.json([row]);
  const owner=new WebSpeechApi({user:{id:'azure-catalog-owner'}},{storage:disk,fetcher});
  await owner.request('/speech/settings',{method:'PATCH',data:{...profile,apiKey:'catalog-secret'}});
  assert(![...disk.data.values()].join('').includes('catalog-secret'));
  assert.equal((await owner.request('/speech/voices')).length,1);
  const other=new WebSpeechApi({user:{id:'azure-catalog-other'}},{storage:disk,fetcher});
  await other.request('/speech/settings',{method:'PATCH',data:profile});
  await assert.rejects(other.request('/speech/voices'),/Key/);
});

test('a late old cloud preview cannot replace or clear the newest validated audio',async()=>{
  let finish,number=0;const api=new WebSpeechApi({user:{id:'azure-late-preview'}},{storage:storage(),fetcher:()=>{
    if(++number===1)return new Promise(resolve=>finish=resolve);
    return Promise.resolve(new Response(pcmWave('AAABAAIA','audio/pcm;rate=24000')));
  }});
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,apiKey:'test-only'}});
  const old=api.request('/speech/preview',{method:'POST'});
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,pitch:0}});
  await api.request('/speech/preview',{method:'POST'});
  const latest=await api.audio('/speech/preview/zf_001/audio');assert.equal(latest.size,50);
  finish(new Response(pcmWave('AAAMAA==','audio/pcm;rate=24000')));
  await assert.rejects(old,/设置已改变/);
  assert.equal((await api.audio('/speech/preview/zf_001/audio')).size,50);
});

test('retired proxy settings cannot create requests or accept any provider key',async()=>{
  assert.throws(()=>cloudRequest({...profile,provider:'edge'},'azure-secret'),/不支持/);
  const api=new WebSpeechApi({user:{id:'retired-provider'}},{storage:storage()});
  await assert.rejects(api.request('/speech/settings',{method:'PATCH',data:{...profile,provider:'edge',apiKey:'must-not-be-kept'}}),/设置无效/);
  assert.equal(api.keys.edge,undefined);
});

test('changing the selected cloud voice produces a fresh request for that exact voice',async()=>{
  const bodies=[];const api=new WebSpeechApi({user:{id:'azure-selected-voice'}},{storage:storage(),fetcher:async(url,options)=>{
    bodies.push(options.body);return new Response(pcmWave('AAAMAA==','audio/pcm;rate=24000'));
  }});
  for(const cloudVoice of ['zh-CN-XiaochenNeural','zh-CN-YunxiNeural']){
    await api.request('/speech/settings',{method:'PATCH',data:{...profile,cloudVoice,apiKey:'test-only'}});
    await api.request('/speech/preview',{method:'POST'});
  }
  assert(bodies[0].includes('zh-CN-XiaochenNeural'));assert(!bodies[0].includes('express-as'));
  assert(bodies[1].includes('zh-CN-YunxiNeural'));assert(!bodies[1].includes('express-as'));
});
