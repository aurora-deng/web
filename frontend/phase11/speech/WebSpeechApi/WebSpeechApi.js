import { cloudRequest, pcmWave } from './CloudSpeech.js?v=144f1e6009a3d461';
import { fetchWave, requireWave, boundedSpeechBody } from '../../shared/SpeechPlayback/SpeechPlayback.js?v=144f1e6009a3d461';
import { speechApi } from '../SpeechApi/SpeechApi.js?v=144f1e6009a3d461';
import { fetchAzureVoices } from '../AzureVoiceCatalog/AzureVoiceCatalog.js?v=144f1e6009a3d461';
import { browserSpeech } from '../BrowserSpeech/BrowserSpeech.js?v=144f1e6009a3d461';
const defaults={provider:'local',voice:'zf_001',style:'calm',region:'eastasia',speed:.95,pitch:-2,freeTierConfirmed:false,dataUseConsent:false,cloudVoice:'',browserVoice:'',model:'gemini-3.1-flash-tts-preview'};
const providers=['local','browser','azure','gemini'];
const sessions=new Map();
const clearKeys=()=>{for(const keys of sessions.values())for(const provider of Object.keys(keys))delete keys[provider];sessions.clear();browserSpeech.cancel();};
globalThis.addEventListener?.('pagehide',clearKeys);
globalThis.addEventListener?.('nebula:user',clearKeys);
/** Web mode keeps non-secret preferences per account. Keys live only in this page's memory. */
export class WebSpeechApi {
  constructor(api,{fetcher=fetch,storage=localStorage,speaker=browserSpeech}={}) {
    this.api=api;this.fetcher=fetcher;this.storage=storage;this.mode='web';this.blob=null;
    this.account=String(api.user.id);this.storageKey='nebula.speech.profile:'+this.account;
    if(!sessions.has(this.account))sessions.set(this.account,{});
    this.keys=sessions.get(this.account);
    this.speaker=speaker;
    // Remove retired proxy addresses for every account on this origin. Never
    // touch another feature's data, or migrate an old proxy to a paid provider.
    try{for(let i=0;i<(storage.length||0);i++){
      const key=storage.key(i);if(!key?.startsWith('nebula.speech.profile:'))continue;
      try{const old=JSON.parse(storage.getItem(key));if(old&&typeof old==='object'&&('edgeEndpoint' in old||old.provider==='edge')){
        const clean=Object.fromEntries(Object.entries(old).filter(([name])=>name in defaults));if(!providers.includes(clean.provider))clean.provider='local';storage.setItem(key,JSON.stringify(clean));
      }}catch{} // Invalid preference data is handled by profile().
    }
    const raw=storage.getItem(this.storageKey);
    if(raw)storage.setItem(this.storageKey,JSON.stringify(this.profile()));
    }catch{} // Storage denial must not prevent reading the free default profile.
  }
  profile(){try{const value=JSON.parse(this.storage.getItem(this.storageKey)||'{}');const p=Object.fromEntries(Object.entries({...defaults,...value}).filter(([key])=>key in defaults));if(!providers.includes(p.provider))p.provider='local';return p;}catch{return {...defaults};}}
  cancelPreview(){this.previewEpoch=(this.previewEpoch||0)+1;this.speaker.cancel();}
  async request(path,{method='GET',data={},signal}={}) {
    if(path==='/speech/settings'&&method==='GET')return {...this.profile(),mode:'web',azureKeyConfigured:!!this.keys.azure,geminiKeyConfigured:!!this.keys.gemini};
    if(path==='/speech/voices'&&method==='GET') {
      const p=this.profile();if(p.provider==='browser')return this.speaker.voices({signal});
      if(p.provider!=='azure')throw new Error('当前服务不提供音色目录。');
      return fetchAzureVoices(p,this.keys.azure,{fetcher:this.fetcher,signal});
    }
    if(path==='/speech/settings'&&method==='PATCH') {
      if(!providers.includes(data.provider)||!['zf_001','zm_009'].includes(data.voice)||!Number.isFinite(data.speed)||data.speed<.75||data.speed>1.25||!Number.isFinite(data.pitch)||data.pitch<-20||data.pitch>20||typeof (data.browserVoice??'')!=='string'||(data.browserVoice||'').length>512)throw new Error('语音设置无效。');
      if(data.apiKey&&(typeof data.apiKey!=='string'||data.apiKey.length>512||/[\r\n]/.test(data.apiKey)))throw new Error('Key 格式无效。');
      const profile=Object.fromEntries(Object.keys(defaults).map(k=>[k,data[k]??defaults[k]]));
      // Never serialize apiKey/removeKey into browser storage.
      this.storage.setItem(this.storageKey,JSON.stringify(profile));
      if(data.removeKey)delete this.keys[data.provider];else if(data.apiKey&&['azure','gemini'].includes(data.provider))this.keys[data.provider]=data.apiKey;
      // Changing settings invalidates an earlier asynchronous preview, including its cached Blob.
      this.cancelPreview();this.result=null;this.blob=null;
      return {ok:true};
    }
    if(path==='/speech/preview'&&method==='POST') {
      const p=this.profile();this.result=null;this.blob=null;const epoch=this.previewEpoch=(this.previewEpoch||0)+1;
      const current=()=>{signal?.throwIfAborted();if(epoch!==this.previewEpoch)throw new Error('语音设置已改变，请重新试听。');};
      if(p.provider==='browser'){
        const result=await this.speaker.speak('欢迎来到星云，神树将为你播报新的故事。',{voiceId:p.browserVoice,speed:p.speed,pitch:p.pitch,signal});
        current();return result;
      }
      if(p.provider!=='local') {
        if(!p.freeTierConfirmed)throw new Error('请先确认所用云服务的额度与套餐。');
        if(p.provider==='gemini'&&!p.dataUseConsent)throw new Error('请先确认 Gemini 的数据用途。');
        const request=cloudRequest(p,this.keys[p.provider]);
        const timeout=AbortSignal.timeout(25000),combined=signal?AbortSignal.any([signal,timeout]):timeout;
        let response;
        try{response=await this.fetcher(request.url,{...request.options,signal:combined,credentials:'omit',redirect:'error',cache:'no-store'});}
        catch(error){if(signal?.aborted)throw error;throw new Error('云语音连接失败或超时。请检查网络和浏览器跨域限制，或改用 Windows 客户端。');}
        if(!response.ok)throw new Error(response.status===401||response.status===403?'云服务拒绝了 Key，请检查 Key 与区域是否匹配。':response.status===429?'云服务限流或额度不足，请稍后重试或选择本地语音。':`云语音返回 ${response.status}，连接测试未通过。`);
        let blob;
        if(p.provider==='azure')blob=await requireWave(await boundedSpeechBody(response));
        else {
          const body=await (await boundedSpeechBody(response,4*1024*1024)).text();
          let value;try{value=JSON.parse(body);}catch{throw new Error('云服务返回了无效 JSON。');}
          const audio=value.candidates?.[0]?.content?.parts?.find(x=>x.inlineData)?.inlineData;
          if(!audio)throw new Error('云服务没有返回语音数据。');blob=await requireWave(pcmWave(audio.data,audio.mimeType));
        }
        current();this.blob=blob;
        return this.result={state:'ready',voice:p.voice,actualProvider:p.provider};
      }
      return this.api.request(speechApi.postMeSpeechPreview({}),{method:'POST',data:{voice:p.voice,speed:String(p.speed)},signal});
    }
    if(path.startsWith('/speech/preview/')&&method==='GET') {
      if(this.result)return this.result;
      return this.api.request(speechApi.getMeSpeechPreviewVoice({voice:path.split('/').at(-1)}),{signal});
    }
    throw new Error('未知语音操作。');
  }
  async audio(path,{signal}={}) {
    if(this.profile().provider==='browser')throw new Error('设备朗读直接出声，不提供音频文件。');
    if(this.blob)return this.blob;
    const voice=path.split('/')[3];if(!['zf_001','zm_009'].includes(voice))throw new Error('无效音色。');
    const state=await this.api.request(speechApi.getMeSpeechPreviewVoice({voice}),{signal});
    if(state.state!=='ready'||!state.url?.startsWith('/api/me/speech/audio/'))throw new Error('试听尚未准备好。');
    return fetchWave(state.url,{signal,fetcher:this.fetcher});
  }
}
