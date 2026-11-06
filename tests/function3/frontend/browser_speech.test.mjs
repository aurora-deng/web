import test from 'node:test';
import assert from 'node:assert/strict';
import { BrowserSpeech } from '../../../frontend/phase11/speech/BrowserSpeech/BrowserSpeech.js';
import { WebSpeechApi } from '../../../frontend/phase11/speech/WebSpeechApi/WebSpeechApi.js';

const cn=(id='local-cn',local=true)=>({voiceURI:id,name:id,lang:'zh-CN',localService:local});
function fixture(rows=[cn()]){
  const engine=new EventTarget();let voices=rows;
  engine.getVoices=()=>voices;engine.speak=u=>{engine.calls.push(u);engine.current=u;u.onstart?.();};
  engine.calls=[];engine.cancels=0;engine.cancel=()=>{engine.cancels++;engine.current=null;};
  const speaker=new BrowserSpeech({engine,Utterance:class{constructor(text){this.text=text;}},voiceTimeout:10,speechTimeout:100});
  return {engine,speaker,setVoices:rows=>{voices=rows;engine.dispatchEvent(new Event('voiceschanged'));}};
}
const disk=()=>{const data=new Map();return {data,get length(){return data.size;},key:i=>[...data.keys()][i],getItem:k=>data.get(k)||null,setItem:(k,v)=>data.set(k,v)};};
const profile={provider:'browser',voice:'zf_001',style:'calm',speed:.95,pitch:-2,browserVoice:''};

test('device voice discovery waits for actual Chinese voices and exposes local/remote metadata',async()=>{
  const x=fixture([]),pending=x.speaker.voices();x.setVoices([cn(),cn(),cn('online-cn',false),{...cn('english'),lang:'en-US'}]);
  const rows=await pending;assert.equal(rows.length,2);assert.equal(rows[0].local,true);assert.equal(rows[1].local,false);
  assert.equal((await x.speaker.voices()).length,2);
});
test('no engine/no Chinese voice/aborted discovery are explicit, without downloads',async()=>{
  const missing=new BrowserSpeech({engine:null});await assert.rejects(missing.voices(),/不支持/);
  const x=fixture([]);assert.deepEqual(await x.speaker.voices(),[]);
  await assert.rejects(x.speaker.speak('你好'),/没有可用中文音色/);assert.equal(x.engine.calls.length,0);
  const c=new AbortController();c.abort();await assert.rejects(x.speaker.voices({signal:c.signal}));
});
test('selected voice and prosody really reach the engine; success requires onend',async()=>{
  const x=fixture([cn(),cn('chosen',false)]);let finished=false;
  const pending=x.speaker.speak('你好，星云。',{voiceId:'chosen',speed:.85,pitch:-10}).then(result=>{finished=true;return result;});
  await new Promise(resolve=>setImmediate(resolve));assert(!finished);const u=x.engine.calls[0];
  assert.equal(u.voice.voiceURI,'chosen');assert.equal(u.rate,.85);assert.equal(u.pitch,.9);assert.equal(u.lang,'zh-CN');
  u.onend();const result=await pending;assert.equal(result.state,'spoken');assert.equal(result.local,false);assert.equal(x.speaker.active,null);
});
test('cancel/replacement reject the old job and late old events cannot finish the new job',async()=>{
  const x=fixture();const old=x.speaker.speak('旧声音');const rejected=assert.rejects(old,{name:'AbortError'});
  await new Promise(resolve=>setImmediate(resolve));const late=x.engine.calls[0].onend;
  const next=x.speaker.speak('新声音');await new Promise(resolve=>setImmediate(resolve));await rejected;
  late();assert.equal(x.speaker.active.utterance.text,'新声音');x.engine.calls[1].onend();await next;
});
test('abort/error/timeout terminate playback and never report synthetic success',async()=>{
  const x=fixture(),c=new AbortController();const p=x.speaker.speak('取消',{signal:c.signal});const rejected=assert.rejects(p,{name:'AbortError'});
  await new Promise(resolve=>setImmediate(resolve));c.abort();await rejected;assert.equal(x.speaker.active,null);
  const fail=x.speaker.speak('播放失败');const failed=assert.rejects(fail,/未允许播放/);
  await new Promise(resolve=>setImmediate(resolve));x.engine.current.onerror({error:'not-allowed'});await failed;
  const slow=x.speaker.speak('超时');await assert.rejects(slow,/超时/);assert.equal(x.speaker.active,null);assert(x.engine.cancels>=2);
});
test('invalid or removed device voices reject, rather than silently switching to a cloud API',async()=>{
  const x=fixture();await assert.rejects(x.speaker.speak('x'.repeat(2001)),/无效/);
  await assert.rejects(x.speaker.speak('你好',{voiceId:'removed'}),/已不可用/);assert.equal(x.engine.calls.length,0);
});
test('web browser preview makes no fetch/business calls and cannot pretend to return a WAV',async()=>{
  const x=fixture(),storage=disk();let network=0;
  const api=new WebSpeechApi({user:{id:'device'},request:()=>{network++;throw Error('unexpected business call');}},{storage,speaker:x.speaker,fetcher:()=>{network++;}});
  await api.request('/speech/settings',{method:'PATCH',data:{...profile,apiKey:'not-a-browser-key'}});
  assert.equal(api.keys.browser,undefined);assert.equal((await api.request('/speech/voices')).length,1);
  const preview=api.request('/speech/preview',{method:'POST'});await new Promise(resolve=>setImmediate(resolve));x.engine.current.onend();
  assert.equal((await preview).actualProvider,'browser');assert.equal(network,0);
  await assert.rejects(api.audio('/speech/preview/zf_001/audio'),/不提供音频文件/);
  assert(![...storage.data.values()].join('').includes('not-a-browser-key'));
});
test('old proxy profiles for all accounts are purged without changing unrelated preferences',async()=>{
  const storage=disk();for(const id of ['owner','other'])storage.setItem('nebula.speech.profile:'+id,JSON.stringify({provider:'edge',edgeEndpoint:'https://retired.invalid',speed:1.1,apiKey:'legacy-must-not-keep'}));
  storage.setItem('nebula.theme','dark');const api=new WebSpeechApi({user:{id:'owner'}},{storage,speaker:fixture().speaker});
  assert.equal((await api.request('/speech/settings')).provider,'local');assert.equal(storage.getItem('nebula.theme'),'dark');
  for(const id of ['owner','other']){const p=JSON.parse(storage.getItem('nebula.speech.profile:'+id));assert.equal(p.provider,'local');assert(!('edgeEndpoint' in p));assert(!('apiKey' in p));}
  assert(!storage.getItem('nebula.speech.profile:owner').includes('legacy-must-not-keep'));
});
