import test from 'node:test';
import assert from 'node:assert/strict';
import { AnnouncementPlayer } from '../../../frontend/phase11/shared/AnnouncementPlayer/AnnouncementPlayer.js';
import { pcmWave } from '../../../frontend/phase11/speech/WebSpeechApi/CloudSpeech.js';

const deferred=()=>{let resolve;const promise=new Promise(r=>resolve=r);return {promise,resolve};};
test('device speech pauses announcements without consuming a playback claim',async()=>{
  let paused=0,ticks=0;const player=new AnnouncementPlayer({},{});player.note={textContent:''};player.audio={pause:()=>paused++,play:()=>assert.fail('overlapping speech')};player.tick=()=>ticks++;
  player.deviceSpeechChanged(true);await player.playAudio({manual:true});assert.equal(paused,1);assert.match(player.note.textContent,/设备正在朗读/);
  player.deviceSpeechChanged(false);assert.equal(ticks,1);
});
async function setup(run){
  const originalDocument=globalThis.document,originalFetch=globalThis.fetch;
  const begun=deferred(),ready=deferred(),calls={created:0,played:0,claims:0,downloads:0};
  globalThis.document={hidden:false,querySelectorAll:()=>[]};
  globalThis.fetch=async()=>{calls.downloads++;return new Response(pcmWave('AAAMAA==','audio/pcm;rate=24000'));};
  let delivered=false;
  const row={id:'1',text:'公开公告',createdAt:Date.now(),expiresAt:Date.now()+120000};
  const api={request:async route=>{
    if(route.operation==='getAnnouncements'){if(delivered)return [];delivered=true;return [row];}
    if(route.operation==='postAnnouncementsAnnouncementClaim'){calls.claims++;return {claimed:true};}
    begun.resolve();return ready.promise;
  }};
  const player=new AnnouncementPlayer(api,{});player.settings={sound:true,voice:'zf_001'};
  player.preview={disabled:false};player.resume={hidden:true};player.note={textContent:''};player.controls={closest:()=>null,querySelector:()=>null};
  player.show=()=>{};
  player.createAudio=async()=>{calls.created++;player.audio={play:async()=>{},pause:()=>{}};};
  player.playAudio=async()=>{calls.played++;};
  try{await run({player,api,calls,begun,ready});}
  finally{globalThis.document=originalDocument;globalThis.fetch=originalFetch;}
}

test('switching voice while announcement metadata is pending never plays the old voice',async()=>setup(async({player,calls,begun,ready})=>{
  const pending=player.poll();await begun.promise;player.settings.voice='zm_009';
  ready.resolve({state:'ready',url:'/old-female.wav'});await pending;
  assert.equal(calls.created,0,'a late female response replaced the selected male voice');
  assert.equal(calls.claims,0,'stale voice must not consume one-use playback rights');
}));

test('switching voice while a manual preview is pending discards the obsolete sample',async()=>setup(async({player,calls,begun,ready})=>{
  const pending=player.previewVoice();await begun.promise;player.settings.voice='zm_009';
  ready.resolve({state:'ready',url:'/old-female.wav'});await pending;
  assert.equal(calls.created,0);assert.equal(player.preview.disabled,false);
}));

test('failed audio download preserves pending announcement and does not consume a claim',async()=>setup(async({player,calls,ready})=>{
  ready.resolve({state:'ready',url:'/announcement.wav'});
  const valid=globalThis.fetch;globalThis.fetch=async()=>new Response('{}',{status:503});
  await assert.rejects(player.poll(),/503/);assert.equal(calls.claims,0);assert.equal(player.pending.size,1);
  globalThis.fetch=valid;await player.poll();
  assert.equal(calls.claims,1);assert.equal(calls.created,1);assert.equal(calls.played,1);
}));

test('a tab that loses the server playback claim creates no audio',async()=>setup(async({player,api,calls,ready})=>{
  const request=api.request;api.request=route=>route.operation==='postAnnouncementsAnnouncementClaim'?Promise.resolve({claimed:false}):request(route);
  ready.resolve({state:'ready',url:'/announcement.wav'});await player.poll();
  assert.equal(calls.created,0);assert.equal(calls.played,0);
}));

test('changing voice aborts an in-flight download without claiming or replacing audio',async()=>setup(async({player,calls,ready})=>{
  ready.resolve({state:'ready',url:'/announcement.wav'});const downloading=deferred();let captured;
  globalThis.fetch=async(url,{signal})=>{captured=signal;downloading.resolve();return new Promise((resolve,reject)=>signal.addEventListener('abort',()=>reject(new DOMException('cancelled','AbortError')),{once:true}));};
  const pending=player.poll();await downloading.promise;player.settings.voice='zm_009';player.invalidateAudio();await pending;
  assert.equal(captured.aborted,true);assert.equal(calls.claims,0);assert.equal(calls.created,0);assert.equal(player.pending.size,1);
}));

test('autoplay rejection stays manual until a user resumes, rather than retrying every poll',async()=>setup(async({player})=>{
  let attempts=0;player.audio={play:async()=>{if(++attempts===1)throw new DOMException('gesture needed','NotAllowedError');}};player.audioExpires=Date.now()+10000;
  await AnnouncementPlayer.prototype.playAudio.call(player);
  assert.equal(player.awaitingManual,true);assert.equal(player.resume.hidden,false);
  await AnnouncementPlayer.prototype.playAudio.call(player);assert.equal(attempts,1);
  await AnnouncementPlayer.prototype.playAudio.call(player,{manual:true});assert.equal(attempts,2);assert.equal(player.playing,true);assert.equal(player.awaitingManual,false);
}));

test('manual playback explains recording, expired audio and disabled announcements instead of silently ignoring clicks',async()=>setup(async({player})=>{
  let attempts=0;player.audio={play:async()=>{attempts++;}};player.audioExpires=Date.now()+10000;
  player.recording=true;await AnnouncementPlayer.prototype.playAudio.call(player,{manual:true});assert.match(player.note.textContent,/录音/);
  player.recording=false;player.audioExpires=0;await AnnouncementPlayer.prototype.playAudio.call(player,{manual:true});assert.match(player.note.textContent,/过期/);
  player.audioExpires=Date.now()+10000;player.settings.sound=false;await AnnouncementPlayer.prototype.playAudio.call(player,{manual:true});assert.match(player.note.textContent,/已关闭/);assert.equal(attempts,0);
  player.audioKind='preview';await AnnouncementPlayer.prototype.playAudio.call(player,{manual:true});assert.equal(attempts,1);
}));
