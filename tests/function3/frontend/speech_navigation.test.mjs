import test from 'node:test';
import assert from 'node:assert/strict';
import { cloudRequest,pcmWave } from '../../../frontend/phase11/speech/WebSpeechApi/CloudSpeech.js';
import { WebSpeechApi } from '../../../frontend/phase11/speech/WebSpeechApi/WebSpeechApi.js';
import { requireWave,fetchWave,playSpeech,boundedSpeechBody } from '../../../frontend/phase11/shared/SpeechPlayback/SpeechPlayback.js';
import { TreePageNavigation } from '../../../frontend/phase11/tree/TreePageNavigation/TreePageNavigation.js';
const profile={provider:'azure',voice:'zf_001',style:'calm',region:'eastasia',speed:.95,pitch:-2,freeTierConfirmed:true,dataUseConsent:false};
const wave=()=>pcmWave('AAAMAA==','audio/L16;codec=pcm;rate=24000');
const memory=()=>{const values=new Map();return {getItem:k=>values.get(k),setItem:(k,v)=>values.set(k,v),values};};
test('web settings persist no credentials and isolate user keys; Azure test returns real bytes',async()=>{
  const storage=memory(),calls=[];
  const api=new WebSpeechApi({user:{id:'voice-test-owner'}},{storage,fetcher:async(url,options)=>{calls.push([url,options]);return new Response(wave(),{headers:{'Content-Type':'audio/wav'}});}});
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,apiKey:'test-secret'}});
  assert(![...storage.values.values()].join('').includes('test-secret'));
  assert.equal((await api.request('/speech/settings')).azureKeyConfigured,true);
  const stranger=new WebSpeechApi({user:{id:'voice-test-stranger'}},{storage});
  assert.equal((await stranger.request('/speech/settings')).azureKeyConfigured,false);
  const state=await api.request('/speech/preview',{method:'POST'});assert.equal(state.actualProvider,'azure');
  assert.equal(calls[0][0],'https://eastasia.tts.speech.microsoft.com/cognitiveservices/v1');
  assert.equal(calls[0][1].headers['Ocp-Apim-Subscription-Key'],'test-secret');assert.equal(calls[0][1].credentials,'omit');
  assert.equal((await api.audio('/speech/preview/zf_001/audio')).size,48);
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,removeKey:true}});assert.equal((await api.request('/speech/settings')).azureKeyConfigured,false);
});
test('missing consent/key and provider failures never masquerade as successful cloud tests',async()=>{
  const storage=memory();let calls=0;
  const api=new WebSpeechApi({user:{id:'voice-test-failure'}},{storage,fetcher:async()=>{calls++;return new Response('{}',{status:429});}});
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,freeTierConfirmed:false}});
  await assert.rejects(api.request('/speech/preview',{method:'POST'}),/套餐/);assert.equal(calls,0);
  await api.request('/speech/settings',{method:'PATCH',data:profile});await assert.rejects(api.request('/speech/preview',{method:'POST'}),/Key/);assert.equal(calls,0);
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,apiKey:'fake-test-key'}});
  await assert.rejects(api.request('/speech/preview',{method:'POST'}),/限流/);assert.equal(calls,1);
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,provider:'gemini'}});
  await assert.rejects(api.request('/speech/preview',{method:'POST'}),/数据用途/);assert.equal(calls,1);
});
test('fixed provider targets, voice syntax and SSML prevent arbitrary endpoint/key forwarding',()=>{
  assert.throws(()=>cloudRequest({...profile,region:'eastasia.evil.example/path'},'key'),/区域/);
  assert.throws(()=>cloudRequest({...profile,cloudVoice:'voice"/><evil>'},'key'),/音色/);
  assert.throws(()=>cloudRequest(profile,'bad\nkey'),/Key/);
  const request=cloudRequest({...profile,cloudVoice:'zh-CN-XiaoxiaoNeural'},'test-key');
  assert(request.options.body.includes('zh-CN-XiaoxiaoNeural'));assert(!request.options.body.includes('express-as style'));
});
test('Gemini default uses supported 3.1 wire, PCM decoding and explicit legacy selection',async()=>{
  const calls=[],storage=memory(),api=new WebSpeechApi({user:{id:'gemini-wire'}},{storage,fetcher:async(url,options)=>{
    calls.push([url,options]);return new Response(JSON.stringify({candidates:[{content:{parts:[{inlineData:{data:'AAAMAA==',mimeType:'audio/L16;codec=pcm;rate=24000'}}]}}]}));
  }});
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,provider:'gemini',dataUseConsent:true,apiKey:'test-only-key'}});
  assert.equal((await api.request('/speech/preview',{method:'POST'})).actualProvider,'gemini');
  assert(calls[0][0].includes('/gemini-3.1-flash-tts-preview:generateContent'));
  assert.equal((await api.audio('/speech/preview/zf_001/audio')).size,48);
  assert(cloudRequest({...profile,provider:'gemini',model:'gemini-2.5-flash-preview-tts'},'test').url.includes('/gemini-2.5-flash-preview-tts:'));
  assert.throws(()=>cloudRequest({...profile,provider:'gemini',model:'unknown'},'test'),/模型/);
});
test('Gemini PCM is wrapped as WAV, invalid/oversized/HTML responses are rejected',async()=>{
  assert.equal((await requireWave(wave())).type,'audio/wav');
  await assert.rejects(requireWave(new Blob(['<html>login required</html>'])),/音频/);
  await assert.rejects(requireWave(new Blob(['x'.repeat(60)])),/WAV/);
  assert.throws(()=>pcmWave('AAAMAA==','text/html'),/格式/);
  assert.equal(pcmWave('AAAMAA==','audio/l16; channels=1; rate=24000').size,48);
  assert.throws(()=>pcmWave('AAAMAA==','audio/l16;channels=2;rate=24000'),/格式/);
  await assert.rejects(boundedSpeechBody(new Response('abc'),2),/大小限制/);
  await assert.rejects(fetchWave('/audio',{fetcher:async()=>new Response('{}',{status:401})}),/登录已过期/);
});
test('autoplay rejection remains an actionable manual play state',async()=>{
  const audio={muted:true,volume:0,play:async()=>{const e=new Error();e.name='NotAllowedError';throw e;}};
  const result=await playSpeech(audio);assert.equal(result.playing,false);assert(result.message.includes('再次点击'));
  audio.play=async()=>{};assert.equal((await playSpeech(audio)).playing,true);assert.equal(audio.muted,false);assert.equal(audio.volume,1);
});
test('large H2 speech downloads in bounded contiguous ranges without losing WAV bytes',async()=>{
  const original=pcmWave(Buffer.alloc(24000*2*30,5).toString('base64'),'audio/pcm;rate=24000');let count=0;
  const result=await fetchWave('/large-audio',{fetcher:async(url,options)=>{
    const match=/bytes=(\d+)-(\d+)/.exec(options.headers.Range);const start=Number(match[1]),end=Math.min(Number(match[2]),original.size-1);count++;
    return new Response(original.slice(start,end+1),{status:206,headers:{'Content-Range':`bytes ${start}-${end}/${original.size}`,'Content-Type':'audio/wav'}});
  }});
  assert(count>=3);assert.deepEqual(Buffer.from(await result.arrayBuffer()),Buffer.from(await original.arrayBuffer()));
  await assert.rejects(fetchWave('/bad-range',{fetcher:async()=>new Response(wave(),{status:206,headers:{'Content-Range':'bytes 1-48/49'}})}),/分段/);
});
test('local preview preserves speed, does not call cloud and validates server WAV',async()=>{
  const calls=[],business={user:{id:'voice-local'},request:async(path,options)=>{calls.push([path,options]);return {state:'ready',url:'/api/me/speech/audio/key'};}};
  const api=new WebSpeechApi(business,{storage:memory(),fetcher:async()=>new Response(wave())});
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,provider:'local',speed:.83}});
  await api.request('/speech/preview',{method:'POST'});assert.equal(calls[0][1].data.speed,'0.83');
  assert.equal((await api.audio('/speech/preview/zf_001/audio')).size,48);
});
function navigation(){
  let current='tree',blocked=false;const listeners=new Map(),root={documentElement:{scrollHeight:900},querySelector:()=>blocked,addEventListener:(name,fn)=>listeners.set(name,fn),removeEventListener:name=>listeners.delete(name)};
  const viewport={scrollY:0,innerHeight:900,scrollTo:({top})=>viewport.scrollY=top},changes=[];
  const nav=new TreePageNavigation({current:()=>current,change:v=>{current=v;changes.push(v);},root,viewport});
  return {nav,root,viewport,changes,listeners,block:v=>blocked=v};
}
test('tree advances once, respects cooldown, and a long feed scrolls before changing page',()=>{
  const x=navigation();x.nav.step(160,100);assert.equal(x.changes.length,0);
  x.nav.step(160,200);assert.deepEqual(x.changes,['feed']);
  x.nav.step(160,300);assert.equal(x.changes.length,1);x.root.documentElement.scrollHeight=2400;
  assert.equal(x.nav.step(160,1200),false);assert.deepEqual(x.changes,['feed']);
  x.viewport.scrollY=1500;x.nav.step(160,1250);assert.equal(x.changes.length,1);
  x.nav.step(160,1600);x.nav.step(160,1700);assert.deepEqual(x.changes,['feed','rank']);
  x.viewport.scrollY=0;x.nav.step(-160,2500);x.nav.step(-160,2600);assert.equal(x.changes.at(-1),'feed');x.nav.dispose();
});
test('dialog, editable target and Alt zoom do not navigate; dispose removes listeners',()=>{
  const x=navigation();let prevented=false;
  const event={deltaY:180,timeStamp:100,preventDefault:()=>prevented=true,target:{closest:()=>false}};
  x.block(true);x.listeners.get('wheel')(event);assert.equal(prevented,false);
  x.block(false);x.listeners.get('wheel')({...event,altKey:true});assert.equal(x.changes.length,0);
  x.listeners.get('wheel')({...event,target:{closest:()=>true}});assert.equal(x.changes.length,0);
  x.listeners.get('wheel')(event);x.listeners.get('wheel')({...event,timeStamp:200});assert.equal(prevented,true);assert.equal(x.changes[0],'feed');
  x.nav.dispose();assert.equal(x.listeners.size,0);
});
test('mobile upward swipe changes page; horizontal rotation gesture stays in tree',()=>{
  const x=navigation(),target={closest:()=>false};
  x.listeners.get('touchstart')({touches:[{clientX:100,clientY:300}]});
  x.listeners.get('touchend')({target,timeStamp:100,changedTouches:[{clientX:230,clientY:200}]});assert.equal(x.changes.length,0);
  x.listeners.get('touchstart')({touches:[{clientX:100,clientY:300}]});
  x.listeners.get('touchend')({target,timeStamp:300,changedTouches:[{clientX:110,clientY:150}]});assert.equal(x.changes[0],'feed');
});
