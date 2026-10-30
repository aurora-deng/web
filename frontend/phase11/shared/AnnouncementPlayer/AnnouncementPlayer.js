import { ReconnectBackoff } from '../RecoveryBudget/RecoveryBudget.js?v=227af12a6c379d08';
/** 文字独立于音频。自动播报先取得服务器一次性播放权，多个标签页不会重复播放。 */
export class AnnouncementPlayer {
  constructor(api, container,{connectEvents=false}={}) { this.connectEvents=connectEvents;this.api=api;this.container=container;this.after='0';this.pending=new Map();this.playing=false;this.recording=false;this.started=Date.now();this.settings={sound:false,voice:'zf_001'};this.backoff=new ReconnectBackoff(); }
  async start() {
    try { this.settings=await this.api.request('/api/me/notification-settings'); }
    catch { return; } // Function3 未启用时保持旧聊天页正常工作。
    if(this.disposed)return;
    if(typeof this.settings?.sound!=='boolean'||!['zf_001','zm_009'].includes(this.settings.voice))return;
    this.controls=document.createElement('div');this.controls.className='announcement-sound';
    this.toggle=document.createElement('button');this.voice=document.createElement('select');this.voice.setAttribute('aria-label','公告音色');
    for(const [value,label] of [['zf_001','温柔女声'],['zm_009','沉稳男声']]){const option=new Option(label,value);this.voice.add(option);}
    this.voice.value=this.settings.voice;this.note=document.createElement('small');this.resume=document.createElement('button');this.resume.textContent='继续播报';this.resume.hidden=true;this.resume.onclick=()=>this.playAudio();this.preview=document.createElement('button');this.preview.className='preview-button';this.preview.textContent='▶ 试听当前音色';this.preview.onclick=()=>this.previewVoice();this.controls.append(this.toggle,this.voice,this.preview,this.resume,this.note);const panel=document.getElementById('global-audio-panel');if(panel){panel.querySelector('.audio-placeholder')?.remove();panel.append(this.controls);}else document.body.append(this.controls);
    try{const info=await this.api.request('/api/me/speech/status');if(typeof info.enabled==='boolean')this.speechInfo=info;}catch{}
    if(this.disposed){this.controls.remove();return;}
    const save=async()=>{try{await this.api.request('/api/me/notification-settings',{method:'PATCH',data:{sound:String(this.settings.sound),voice:this.settings.voice}});this.paint();}catch(e){this.note.textContent=e.message;}};
    this.toggle.onclick=async()=>{this.settings.sound=!this.settings.sound;if(!this.settings.sound){this.audio?.pause();this.playing=false;}await save();this.tick?.();};
    this.voice.onchange=async()=>{this.settings.voice=this.voice.value;await save();};this.paint();
    this.recordingListener=event=>{this.recording=Boolean(event.detail);if(this.recording){this.audio?.pause();this.playing=false;}else this.tick();};document.addEventListener('nebula:recording',this.recordingListener);
    this.mediaListener=event=>{if(event.target===this.audio)return;if(event.type==='play'){this.audio?.pause();this.playing=false;}else this.tick();};
    document.addEventListener('play',this.mediaListener,true);document.addEventListener('ended',this.mediaListener,true);
    this.tick=()=>this.poll().catch(()=>{});this.timer=setInterval(this.tick,5000);this.tick();
    window.addEventListener('nebula:announcement',this.tick);
    if(this.connectEvents)this.connect();
    addEventListener('pagehide',()=>this.dispose(),{once:true});
  }
  paint(){this.toggle.textContent=this.settings.sound?'♫ 公告声音已开':'♩ 开启公告声音';this.toggle.setAttribute('aria-pressed',String(this.settings.sound));this.note.textContent=this.speechInfo?.enabled===false?'当前服务未启用语音引擎，只提供文字公告。请使用日常启动入口。':'开启后播报新公告，可先试听确认声音。';}
  connect(){
    if(this.disposed)return;this.events=new EventSource('/events',{withCredentials:true});
    this.events.onopen=()=>this.backoff.reset();
    for(const name of ['announcement.created','announcement.audio.ready'])this.events.addEventListener(name,this.tick);
    this.events.onerror=()=>{this.events.close();this.reconnect=setTimeout(()=>this.connect(),this.backoff.next());};
  }
  async poll(){
    if(this.disposed||this.polling||document.hidden)return;this.polling=true;
    try{
      const rows=await this.api.request('/api/announcements?after='+this.after);
      if(this.disposed)return;
      for(const row of rows){this.after=row.id;this.show(row);if(row.createdAt>=this.started-5000&&row.expiresAt>Date.now())this.pending.set(row.id,row);}
      for(const [id,row] of this.pending)if(row.expiresAt<=Date.now())this.pending.delete(id);
      if(!this.settings.sound||this.playing||this.recording||[...document.querySelectorAll('audio,video')].some(a=>!a.paused))return;
      // 已取得播放权的标签页在录音/用户音频结束后续播，无需再次领取或重复从头播放。
      if(this.audio&&!this.audio.ended&&this.audioExpires>Date.now()){await this.playAudio();return;}
      const row=this.pending.values().next().value;if(!row)return;
      // 音频生成可能晚于文字到达；未就绪时保留有界等待，文字无需等待 TTS。
      const status=await this.api.request(`/api/announcements/${row.id}/audio?voice=${this.settings.voice}`);
      if(status.state!=='ready'){this.note.textContent=status.state==='failed'?'语音生成失败，文字公告不受影响。':this.speechInfo?.enabled===false?'语音引擎未启用，当前仅有文字公告。':'公告语音正在生成，稍后自动播报…';return;}
      if(this.disposed)return;
      const claim=await this.api.request(`/api/announcements/${row.id}/claim`,{method:'POST',data:{}});this.pending.delete(row.id);
      if(this.disposed)return;
      if(!claim.claimed)return;
      this.createAudio(status.url,row.expiresAt);
      await this.playAudio();
    }finally{this.polling=false;}
  }
  createAudio(url,expires){
    this.audio?.pause();this.controls?.querySelector('audio')?.remove();this.audio=new Audio(url);this.audio.controls=true;this.audio.preload='auto';this.audio.volume=1;this.audio.muted=false;this.audioExpires=expires;this.audio.setAttribute('aria-label','公告语音播放器');this.controls?.append(this.audio);
    this.audio.onended=()=>{this.playing=false;this.resume.hidden=true;};this.audio.onerror=()=>{this.playing=false;this.resume.hidden=false;this.note.textContent='音频加载失败，请重试试听或检查语音服务。';};
  }
  async previewVoice(){
    if(this.disposed||this.preview.disabled)return;
    if(this.speechInfo?.enabled===false){this.note.textContent='语音引擎未启用。声音开关只是订阅设置，不能启动服务端引擎。';return;}
    this.preview.disabled=true;this.note.textContent='正在准备试听，首次加载可能需要几十秒…';const voice=this.settings.voice;
    try{
      let status=await this.api.request('/api/me/speech/preview',{method:'POST',data:{voice}});
      for(let i=0;status.state==='pending'&&i<90&&!this.disposed;i++){await new Promise(r=>setTimeout(r,800));if(this.disposed)return;status=await this.api.request('/api/me/speech/preview/'+voice);}
      if(this.disposed)return;if(status.state!=='ready'||!status.url)throw new Error('语音准备失败或超时，请检查语音引擎状态。');
      // 试听是主动操作，与“是否自动播公告”分开，不改写用户的通知偏好。
      this.createAudio(status.url,Date.now()+120000);this.playing=true;
      try{await this.audio.play();this.resume.hidden=true;this.note.textContent='正在试听；也可使用下方播放器调整音量。';}
      catch{this.playing=false;this.note.textContent='音频已就绪，请点击下方播放器的播放按钮。';}
    }catch(error){if(!this.disposed)this.note.textContent=error.message;}finally{if(!this.disposed)this.preview.disabled=false;}
  }
  async playAudio(){
    if(this.disposed||!this.audio||!this.settings.sound||this.recording||this.audioExpires<=Date.now())return;
    this.playing=true;if(this.audio.error)this.audio.load();
    try{await this.audio.play();this.resume.hidden=true;this.note.textContent='';}
    catch{this.playing=false;this.resume.hidden=false;this.note.textContent='浏览器限制自动播放，可点击继续播报。';}
  }
  show(row){
    const banner=document.createElement('div');banner.className='announcement';const text=document.createElement('span');text.textContent='✧ '+row.text;
    const close=document.createElement('button');close.textContent='知道了';close.onclick=()=>banner.remove();banner.append(text,close);this.container.append(banner);
    while(this.container.children.length>3)this.container.firstElementChild.remove();setTimeout(()=>banner.remove(),18000);
  }
  dispose(){this.disposed=true;clearInterval(this.timer);clearTimeout(this.reconnect);this.events?.close();this.audio?.pause();this.audio?.removeAttribute('src');this.audio?.load();this.controls?.remove();document.removeEventListener('nebula:recording',this.recordingListener);document.removeEventListener('play',this.mediaListener,true);document.removeEventListener('ended',this.mediaListener,true);window.removeEventListener('nebula:announcement',this.tick);}
}
