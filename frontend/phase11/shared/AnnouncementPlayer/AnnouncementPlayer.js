import { fetchWave, playSpeech } from '../SpeechPlayback/SpeechPlayback.js?v=144f1e6009a3d461';
import { speechApi } from "../../speech/SpeechApi/SpeechApi.js?v=144f1e6009a3d461";
import { notificationsApi } from "../../announcements/NotificationsApi/NotificationsApi.js?v=144f1e6009a3d461";
import { announcementsApi } from "../../announcements/AnnouncementsApi/AnnouncementsApi.js?v=144f1e6009a3d461";
import { ReconnectBackoff } from '../RecoveryBudget/RecoveryBudget.js?v=144f1e6009a3d461';
/** 文字独立于音频。自动播报先取得服务器一次性播放权，多个标签页不会重复播放。 */
export class AnnouncementPlayer {
  constructor(api, container,{connectEvents=false}={}) { this.connectEvents=connectEvents;this.api=api;this.container=container;this.after='0';this.pending=new Map();this.playing=false;this.recording=false;this.started=Date.now();this.settings={sound:false,voice:'zf_001'};this.backoff=new ReconnectBackoff();this.audioController=new AbortController();this.audioEpoch=0; }
  async start() {
    try { this.settings=await this.api.request(notificationsApi.getMeNotificationSettings({})); }
    catch { return; } // Function3 未启用时保持旧聊天页正常工作。
    if(this.disposed)return;
    if(typeof this.settings?.sound!=='boolean'||!['zf_001','zm_009'].includes(this.settings.voice))return;
    this.controls=document.createElement('div');this.controls.className='announcement-sound';
    this.toggle=document.createElement('button');this.voice=document.createElement('select');this.voice.setAttribute('aria-label','公告音色');
    for(const [value,label] of [['zf_001','温柔女声'],['zm_009','沉稳男声']]){const option=new Option(label,value);this.voice.add(option);}
    this.voice.value=this.settings.voice;this.note=document.createElement('small');this.resume=document.createElement('button');this.resume.textContent='播放声音 / 继续';this.resume.hidden=true;this.resume.onclick=()=>this.playAudio({manual:true});this.preview=document.createElement('button');this.preview.className='preview-button';this.preview.textContent='▶ 试听当前音色';this.preview.onclick=()=>this.previewVoice();this.controls.append(this.toggle,this.voice,this.preview,this.resume,this.note);const panel=document.getElementById('global-audio-panel');if(panel){panel.querySelector('.audio-placeholder')?.remove();panel.append(this.controls);}else document.body.append(this.controls);
    try{const info=await this.api.request(speechApi.getMeSpeechStatus({}));if(typeof info.enabled==='boolean')this.speechInfo=info;}catch{}
    if(this.disposed){this.controls.remove();return;}
    const save=async()=>{try{await this.api.request(notificationsApi.patchMeNotificationSettings({}),{method:'PATCH',data:{sound:String(this.settings.sound),voice:this.settings.voice}});this.paint();}catch(e){this.note.textContent=e.message;}};
    this.toggle.onclick=async()=>{this.settings.sound=!this.settings.sound;if(!this.settings.sound&&!this.previewBusy&&this.audioKind!=='preview')this.invalidateAudio({clear:false});await save();this.tick?.();};
    this.voice.onchange=async()=>{this.settings.voice=this.voice.value;this.invalidateAudio();await save();this.tick?.();};this.paint();
    this.recordingListener=event=>{this.recording=Boolean(event.detail);if(this.recording){this.audio?.pause();this.playing=false;if(this.audio)this.note.textContent='正在录音，公告与试听已暂停；录音结束后可继续播放。';}else this.tick();};document.addEventListener('nebula:recording',this.recordingListener);
    this.mediaListener=event=>{if(event.target===this.audio)return;if(event.type==='play'){this.audio?.pause();this.playing=false;if(this.audio)this.note.textContent='正在播放其他音频，公告与试听暂缓播放。';}else this.tick();};
    document.addEventListener('play',this.mediaListener,true);document.addEventListener('ended',this.mediaListener,true);
    this.tick=()=>this.poll().catch(e=>{if(!this.disposed)this.note.textContent=e.message||'公告语音暂不可用，可手动试听检查。';});this.timer=setInterval(this.tick,5000);this.tick();
    this.deviceListener=event=>this.deviceSpeechChanged(Boolean(event.detail?.active));window.addEventListener('nebula:device-speech',this.deviceListener);
    window.addEventListener('nebula:announcement',this.tick);
    if(this.connectEvents)this.connect();
    addEventListener('pagehide',()=>this.dispose(),{once:true});
  }
  paint(){this.toggle.textContent=this.settings.sound?'♫ 公告声音已开':'♩ 开启公告声音';this.toggle.setAttribute('aria-pressed',String(this.settings.sound));this.note.textContent=this.speechInfo?.enabled===false?'当前服务未启用语音引擎，只提供文字公告。请使用日常启动入口。':'开启后播报新公告，可先试听确认声音。';}
  /** Device speech has no HTML audio element; use an explicit page event so
   * it still participates in the existing one-at-a-time announcement policy. */
  deviceSpeechChanged(active){this.deviceSpeaking=active;if(active){this.audio?.pause();this.playing=false;this.note.textContent='设备正在朗读，公告暂缓播放。';}else this.tick?.();}
  /** A changed voice owns a new generation. Late metadata/downloads must not
   * install an obsolete clip or consume a one-use announcement playback claim. */
  invalidateAudio({clear=true}={}){
    this.audioEpoch++;this.audioController.abort();this.audioController=new AbortController();this.audio?.pause();this.playing=false;
    if(clear){this.audio?.removeAttribute('src');this.audio?.load();this.controls?.querySelector('audio')?.remove();if(this.audioUrl)URL.revokeObjectURL(this.audioUrl);this.audio=null;this.audioUrl='';this.audioKind='';this.awaitingManual=false;if(this.resume)this.resume.hidden=true;}
  }
  connect(){
    if(this.disposed)return;this.events=new EventSource('/events',{withCredentials:true});
    this.events.onopen=()=>this.backoff.reset();
    for(const name of ['announcement.created','announcement.audio.ready'])this.events.addEventListener(name,this.tick);
    this.events.onerror=()=>{this.events.close();this.reconnect=setTimeout(()=>this.connect(),this.backoff.next());};
  }
  async poll(){
    if(this.disposed||this.polling||this.preview?.disabled||document.hidden)return;this.polling=true;const epoch=this.audioEpoch,signal=this.audioController.signal,voice=this.settings.voice;
    try{
      const rows=await this.api.request(announcementsApi.getAnnouncements({}, "after=" + String(this.after)),{signal});
      if(this.disposed||signal.aborted)return;
      for(const row of rows){this.after=row.id;this.show(row);if(row.createdAt>=this.started-5000&&row.expiresAt>Date.now())this.pending.set(row.id,row);}
      for(const [id,row] of this.pending)if(row.expiresAt<=Date.now())this.pending.delete(id);
      if(!this.settings.sound||this.playing||this.recording||this.deviceSpeaking||[...document.querySelectorAll('audio,video')].some(a=>!a.paused))return;
      // 已取得播放权的标签页在录音/用户音频结束后续播，无需再次领取或重复从头播放。
      if(this.audio&&!this.audio.ended&&this.audioExpires>Date.now()){await this.playAudio();return;}
      const row=this.pending.values().next().value;if(!row)return;
      // 音频生成可能晚于文字到达；未就绪时保留有界等待，文字无需等待 TTS。
      const status=await this.api.request(announcementsApi.getAnnouncementsAnnouncementAudio({"announcement":(row.id)}, "voice=" + String(voice)),{signal});
      if(this.disposed||signal.aborted||epoch!==this.audioEpoch||voice!==this.settings.voice)return;
      if(status.state!=='ready'){this.note.textContent=status.state==='failed'?'语音生成失败，文字公告不受影响。':this.speechInfo?.enabled===false?'语音引擎未启用，当前仅有文字公告。':'公告语音正在生成，稍后自动播报…';return;}
      const blob=await fetchWave(status.url,{signal});
      if(this.disposed||signal.aborted||epoch!==this.audioEpoch||voice!==this.settings.voice||!this.settings.sound||this.recording||this.deviceSpeaking)return;
      const claim=await this.api.request(announcementsApi.postAnnouncementsAnnouncementClaim({"announcement":(row.id)}),{method:'POST',data:{}});this.pending.delete(row.id);
      if(this.disposed||epoch!==this.audioEpoch||voice!==this.settings.voice)return;
      if(!claim.claimed)return;
      await this.createAudio(blob,row.expiresAt,'announcement');
      await this.playAudio();
    }catch(error){if(!signal.aborted)throw error;}finally{this.polling=false;}
  }
  async createAudio(source,expires,kind='announcement'){
    const epoch=this.audioEpoch;
    const blob=typeof source==='string'?await fetchWave(source,{signal:this.audioController.signal}):source;if(this.disposed||epoch!==this.audioEpoch)return;
    this.audio?.pause();this.controls?.querySelector('audio')?.remove();if(this.audioUrl)URL.revokeObjectURL(this.audioUrl);
    this.audioUrl=URL.createObjectURL(blob);this.audio=new Audio(this.audioUrl);this.audioKind=kind;this.audio.controls=true;this.audio.preload='auto';this.audio.volume=1;this.audio.muted=false;this.audioExpires=expires;this.audio.setAttribute('aria-label','公告与试听播放器');this.controls?.append(this.audio);
    this.audio.onplay=()=>{this.playing=true;this.awaitingManual=false;this.resume.hidden=true;};
    this.audio.onended=()=>{this.playing=false;this.awaitingManual=false;this.resume.hidden=true;this.note.textContent=this.audioKind==='preview'?'试听播放结束，可重新试听或切换音色。':'公告播放结束。';};
    this.audio.onerror=()=>{this.playing=false;this.resume.hidden=false;this.note.textContent='音频无法解码，请重新生成试听。';this.controls?.closest('details')?.setAttribute('open','');};
  }
  async previewVoice(){
    if(this.disposed||this.preview.disabled)return;
    if(this.speechInfo?.enabled===false){this.note.textContent='语音引擎未启用。声音开关只是订阅设置，不能启动服务端引擎。';return;}
    this.invalidateAudio();this.preview.disabled=true;this.previewBusy=true;this.note.textContent='正在准备试听，首次加载可能需要几十秒…';const voice=this.settings.voice,epoch=this.audioEpoch,signal=this.audioController.signal;
    try{
      let status=await this.api.request(speechApi.postMeSpeechPreview({}),{method:'POST',data:{voice},signal});
      for(let i=0;status.state==='pending'&&i<155&&!this.disposed;i++){await new Promise(r=>setTimeout(r,800));if(this.disposed||signal.aborted||epoch!==this.audioEpoch||voice!==this.settings.voice)return;status=await this.api.request(speechApi.getMeSpeechPreviewVoice({"voice":(voice)}),{signal});}
      if(this.disposed||signal.aborted||epoch!==this.audioEpoch||voice!==this.settings.voice)return;if(status.state!=='ready'||!status.url)throw new Error('语音准备失败或超时，请检查语音引擎状态。');
      // 试听是主动操作，与“是否自动播公告”分开，不改写用户的通知偏好。
      await this.createAudio(status.url,Date.now()+120000,'preview');if(this.disposed||signal.aborted||epoch!==this.audioEpoch||voice!==this.settings.voice)return;this.playing=true;this.controls?.closest('details')?.setAttribute('open','');
      await this.playAudio({manual:true});
    }catch(error){if(!this.disposed&&!signal.aborted&&epoch===this.audioEpoch)this.note.textContent=error.message;}finally{this.previewBusy=false;if(!this.disposed)this.preview.disabled=false;}
  }
  async playAudio({manual=false}={}){
    if(this.disposed||!this.audio)return;
    if(this.deviceSpeaking){if(manual)this.note.textContent='设备正在朗读，请停止设备试听后再播放公告。';return;}
    if(this.recording){if(manual)this.note.textContent='正在录音，结束后可继续播放。';return;}
    if(this.audioExpires<=Date.now()){this.resume.hidden=true;this.note.textContent='这段音频已过期，请重新生成试听或等待新公告。';return;}
    if(this.audioKind!=='preview'&&!this.settings.sound){if(manual)this.note.textContent='公告声音已关闭，请先开启公告声音；主动试听不受此开关影响。';return;}
    // A denied autoplay needs a real user action, not a retry every polling tick.
    if(this.awaitingManual&&!manual)return;
    this.playing=true;if(this.audio.error)this.audio.load();
    try{const result=await playSpeech(this.audio);this.playing=result.playing;this.awaitingManual=!result.playing;this.resume.hidden=result.playing;this.note.textContent=result.message;if(!result.playing)this.controls?.closest('details')?.setAttribute('open','');}
    catch(e){this.playing=false;this.awaitingManual=true;this.resume.hidden=false;this.note.textContent=e.message;}
  }
  show(row){
    const banner=document.createElement('div');banner.className='announcement';const text=document.createElement('span');text.textContent='✧ '+row.text;
    const close=document.createElement('button');close.textContent='知道了';close.onclick=()=>banner.remove();banner.append(text,close);this.container.append(banner);
    while(this.container.children.length>3)this.container.firstElementChild.remove();setTimeout(()=>banner.remove(),18000);
  }
  dispose(){this.disposed=true;this.audioController.abort();if(this.audioUrl)URL.revokeObjectURL(this.audioUrl);clearInterval(this.timer);clearTimeout(this.reconnect);this.events?.close();this.audio?.pause();this.audio?.removeAttribute('src');this.audio?.load();this.controls?.remove();document.removeEventListener('nebula:recording',this.recordingListener);document.removeEventListener('play',this.mediaListener,true);document.removeEventListener('ended',this.mediaListener,true);window.removeEventListener('nebula:announcement',this.tick);window.removeEventListener('nebula:device-speech',this.deviceListener);}
}
