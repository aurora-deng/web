/** Browser-owned speech: no application API key, proxy, package or model download.
 * A remote voice can still send text to the browser vendor. The UI exposes
 * localService instead of promising that every browser voice works offline.
 */
export class BrowserSpeech {
  constructor({engine=globalThis.speechSynthesis,Utterance=globalThis.SpeechSynthesisUtterance,voiceTimeout=2500,speechTimeout=120000,notify=active=>globalThis.dispatchEvent?.(new CustomEvent('nebula:device-speech',{detail:{active}}))}={}) {
    this.engine=engine;this.Utterance=Utterance;this.voiceTimeout=voiceTimeout;this.speechTimeout=speechTimeout;this.notify=notify;this.epoch=0;
  }
  supported(){return !!this.engine&&typeof this.Utterance==='function';}
  snapshot(){
    if(!this.supported())return [];
    const seen=new Set();
    return Array.from(this.engine.getVoices()).filter(voice=>{
      if(!/^zh(?:[-_]|$)/i.test(voice.lang)||!voice.voiceURI||voice.voiceURI.length>512||voice.name.length>160||seen.has(voice.voiceURI))return false;
      seen.add(voice.voiceURI);return true;
    }).slice(0,256);
  }
  /** Voices often arrive after page load. All temporary listeners are released. */
  async voices({signal}={}){
    signal?.throwIfAborted();
    if(!this.supported())throw new Error('当前浏览器不支持设备朗读，请选择服务器 Kokoro。');
    let voices=this.snapshot();
    if(!voices.length)voices=await new Promise((resolve,reject)=>{
      const cleanup=()=>{clearTimeout(timer);this.engine.removeEventListener('voiceschanged',changed);signal?.removeEventListener('abort',aborted);};
      const changed=()=>{const rows=this.snapshot();if(rows.length){cleanup();resolve(rows);}};
      const aborted=()=>{cleanup();reject(signal.reason||new DOMException('已取消','AbortError'));};
      const timer=setTimeout(()=>{cleanup();resolve(this.snapshot());},this.voiceTimeout);
      this.engine.addEventListener('voiceschanged',changed);signal?.addEventListener('abort',aborted,{once:true});
      if(signal?.aborted)aborted();else changed();
    });
    signal?.throwIfAborted();
    return voices.map(v=>({id:v.voiceURI,name:v.name,locale:v.lang,local:v.localService===true}));
  }
  cancel(){this.epoch++;this.active?.cancel();}
  /** Resolve only after onend, never merely because speak() accepted the job.
   * Retain the utterance while playing and cap both queue length and duration.
   */
  async speak(text,{voiceId='',speed=.95,pitch=0,signal,onStart}={}){
    if(typeof text!=='string'||!text.trim()||text.length>2000||typeof voiceId!=='string'||voiceId.length>512||!Number.isFinite(speed)||speed<.75||speed>1.25||!Number.isFinite(pitch)||pitch<-20||pitch>20)throw new Error('设备朗读内容或设置无效。');
    this.cancel();const epoch=this.epoch;
    const rows=await this.voices({signal});
    signal?.throwIfAborted();if(epoch!==this.epoch)throw new DOMException('试听已停止','AbortError');
    if(!rows.length)throw new Error('当前设备没有可用中文音色，请选择服务器 Kokoro；不会自动下载音色。');
    const selected=voiceId?rows.find(v=>v.id===voiceId):(rows.find(v=>v.local&&/^zh[-_]CN$/i.test(v.locale))||rows.find(v=>v.local)||rows[0]);
    if(!selected)throw new Error('所选音色已不可用，请重新获取设备中文音色。');
    const voice=this.snapshot().find(v=>v.voiceURI===selected.id);
    if(!voice)throw new Error('设备音色已变化，请重新选择。');
    const utterance=new this.Utterance(text);utterance.voice=voice;utterance.lang=voice.lang;utterance.rate=speed;utterance.pitch=1+pitch/100;utterance.volume=1;
    return new Promise((resolve,reject)=>{
      let settled=false;
      const finish=(error)=>{
        if(settled)return;settled=true;clearTimeout(timer);signal?.removeEventListener('abort',aborted);
        utterance.onstart=null;utterance.onend=null;utterance.onerror=null;
        if(this.active?.utterance===utterance)this.active=null;
        this.notify(false);
        if(error)reject(error);else resolve({state:'spoken',actualProvider:'browser',voiceName:selected.name,local:selected.local});
      };
      const stop=error=>{finish(error);this.engine.cancel();};
      const aborted=()=>stop(signal.reason||new DOMException('试听已停止','AbortError'));
      const timer=setTimeout(()=>stop(new Error('设备朗读超时，请检查系统音量或改用服务器 Kokoro。')),this.speechTimeout);
      this.active={utterance,cancel:()=>stop(new DOMException('试听已停止','AbortError'))};
      utterance.onstart=()=>{this.notify(true);onStart?.(selected);};
      utterance.onend=()=>finish();
      utterance.onerror=event=>finish(new Error(event.error==='not-allowed'?'浏览器未允许播放，请再次点击试听。':'设备朗读失败，请重试或选择服务器 Kokoro。'));
      signal?.addEventListener('abort',aborted,{once:true});
      if(signal?.aborted)return aborted();
      try{this.engine.speak(utterance);}catch{stop(new Error('设备朗读启动失败，请选择服务器 Kokoro。'));}
    });
  }
}
// One page owns at most one device utterance, including repeated dialog opens.
export const browserSpeech=new BrowserSpeech();
