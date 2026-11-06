import { playSpeech, requireWave, fetchWave } from '../SpeechPlayback/SpeechPlayback.js?v=144f1e6009a3d461';
const element=(tag,text)=>{const e=document.createElement(tag);if(text)e.textContent=text;return e;};
// 云音色目录：浏览器直连时由 cloudVoice 直接决定实际音色，所以这里列出的都能真正生效。
// 站点公告仍受后端 SiteSpeechRenderer 的两档预置限制，界面上不把这份目录混进去。
const AZURE_VOICES=[
    ['zh-CN-XiaoxiaoNeural','晓晓','女','温柔'],
    ['zh-CN-XiaoyiNeural','晓伊','女','甜美'],
    ['zh-CN-XiaochenNeural','晓辰','女','知性'],
    ['zh-CN-XiaohanNeural','晓涵','女','优雅'],
    ['zh-CN-XiaomengNeural','晓梦','女','梦幻'],
    ['zh-CN-XiaomoNeural','晓墨','女','文艺'],
    ['zh-CN-XiaoqiuNeural','晓秋','女','成熟'],
    ['zh-CN-XiaoruiNeural','晓睿','女','智慧'],
    ['zh-CN-XiaoshuangNeural','晓双','女','活泼'],
    ['zh-CN-XiaoxuanNeural','晓萱','女','清新'],
    ['zh-CN-XiaoyanNeural','晓颜','女','柔美'],
    ['zh-CN-XiaoyouNeural','晓悠','女','悠扬'],
    ['zh-CN-XiaozhenNeural','晓甄','女','端庄'],
    ['zh-CN-YunxiNeural','云希','男','清朗'],
    ['zh-CN-YunyangNeural','云扬','男','阳光'],
    ['zh-CN-YunjianNeural','云健','男','稳重'],
    ['zh-CN-YunfengNeural','云枫','男','磁性'],
    ['zh-CN-YunhaoNeural','云皓','男','豪迈'],
    ['zh-CN-YunxiaNeural','云夏','男','热情'],
    ['zh-CN-YunyeNeural','云野','男','野性'],
    ['zh-CN-YunzeNeural','云泽','男','深沉']
];
const CLOUD_VOICES={azure:AZURE_VOICES,gemini:[
    ['Kore','Kore','女','沉稳'],
    ['Aoede','Aoede','女','轻快'],
    ['Leda','Leda','女','柔和'],
    ['Charon','Charon','男','低沉'],
    ['Puck','Puck','男','明亮'],
    ['Fenrir','Fenrir','男','厚重']
]};
/** Same dialog, three explicit boundaries: browser memory, Windows DPAPI, site settings. */
export async function openSpeechSettings(api,{site=false}={}){
  if(document.querySelector('.speech-settings[open]'))return;
  if(!document.querySelector('[data-speech-css]')){const css=element('link');css.rel='stylesheet';css.href='/phase11/shared/SpeechSettings/SpeechSettings.css?v=144f1e6009a3d461';css.dataset.speechCss='';document.head.append(css);}
  const web=api.mode==='web',dialog=element('dialog');dialog.className='speech-settings';
  dialog.setAttribute('aria-label',site?'站点公告语音设置':'语音 API 与音色设置');
  dialog.innerHTML=`<header><div><small>VOICE STUDIO</small><h2>${site?'站点公告音色':'语音 API 与音色设置'}</h2></div><button type="button" data-close aria-label="关闭语音设置">×</button></header>
    <p class="speech-scope">${site?'这些设置控制所有人的公告语音。默认 Kokoro 不需要云账号或 Key。选择云服务后，Key 通过 HTTPS 保存到 Linux 私有文件，不回显，也不写入数据库。':web?'默认使用服务器 Kokoro，无需绑定付款方式或申请 Key。也可试听浏览器自带中文音色；Azure/Gemini 仅供已有自有 Key 的用户选择，本站不会替你开通套餐。':'当前是 Windows 私人设置。默认本地 Kokoro 无需 Key；可选云服务的 Key 由本机客户端通过 DPAPI 保存。'}</p>
    <p class="speech-scope">${site?'个人页面的音色不会覆盖站点配置。':'个人 API 用于此处连接测试与试听；全服公告使用站点管理员配置，公告男女声在“声音与通知”中切换。'}</p>
    <form><fieldset data-fields disabled><div class="speech-grid">
      <label>语音服务<select name="provider"><option value="local">服务器 Kokoro · 免 Key</option><option value="browser">浏览器设备朗读 · 免 Key</option><option value="azure">Azure Speech · 自有 Key / 可选</option><option value="gemini">Gemini TTS · 自有 Key / 可选</option></select></label>
      <label>预置音色<select name="voice"><option value="zf_001">女声 · Kokoro</option><option value="zm_009">男声 · Kokoro</option></select></label>
      <label data-azure>表达方式<select name="style"></select></label><label data-azure>Azure 资源区域<input name="region" maxlength="32" value="eastasia" autocomplete="off"><small>需与 Key 所属资源区域一致</small></label>
      <label>语速 <output data-speed></output><input name="speed" type="range" min="0.75" max="1.25" step="0.01" value="0.95"></label>
      <label data-prosody>音调<select name="pitch"><option value="-20">很低 · -20%</option><option value="-10">偏低 · -10%</option><option value="-5">低沉 · -5%</option><option value="-2">稍低 · -2%</option><option value="0">自然 · 0%</option><option value="2">明亮 · +2%</option><option value="5">偏高 · +5%</option><option value="10">高 · +10%</option><option value="20">很高 · +20%</option></select></label></div>
      <label data-custom-voice hidden>云音色<select name="cloudVoice"></select><small>这一项决定实际发声；上面的预置档只决定男女与回退方向。站点公告不受此选择影响。</small></label>
      <button type="button" data-load-voices hidden>获取我的 Azure 可用中文音色</button>
      <small data-voice-help hidden>参考目录不保证每个音色可用。填好自己的 Key 与区域后获取实际列表；选择后再点击连接试听。Azure F0 的额度和功能以你的资源为准。</small>
      <label data-browser hidden>设备中文音色<select name="browserVoice"><option value="">自动选择可用中文音色</option></select><small>仅列出当前浏览器实际提供的音色。可优先试听带 Natural 的自然音色。“本机”由设备合成；“在线”可能把文本交给浏览器的语音服务。未安装的音色不会自动下载。</small></label>
      <button type="button" data-browser-voices hidden>获取设备中文音色</button>
      <p class="speech-endpoint" data-endpoint></p>
      <details data-azure-guide hidden><summary>第一次使用：获取并填写 Azure Key</summary><ol><li>打开 <a href="https://portal.azure.com/" target="_blank" rel="noopener noreferrer">Azure 门户</a>，创建或打开你自己的 Speech 语音资源，确认所选套餐。</li><li>在资源的“密钥和终结点”复制 KEY 1 或 KEY 2，粘贴到下面的 Key 输入框。</li><li>把资源区域填入上方区域框；例如 eastasia 只是示例，不要粘贴完整网址。</li><li>获取可用音色，选择后点下方“连接并试听当前音色”。更换音色后需重新生成试听。</li></ol><p>已有资源可直接使用，无需安装 SDK。Key 不要发到聊天里。<a href="https://learn.microsoft.com/zh-cn/azure/ai-services/speech-service/rest-text-to-speech" target="_blank" rel="noopener noreferrer">查看官方说明 ↗</a></p></details>
      <label data-key-row>自己的 API Key<input name="apiKey" type="password" maxlength="512" autocomplete="new-password" placeholder="粘贴 Speech 资源的 KEY 1 或 KEY 2"><small>保存后输入框会清空；是否已配置以下方状态为准。</small></label>
      <label data-key-row><input name="removeKey" type="checkbox">删除当前服务的 Key</label>
      <label data-cloud><input name="freeTierConfirmed" type="checkbox">我已确认自己的云服务套餐与可用额度</label>
      <label data-gemini>Gemini 模型<select name="model"><option value="gemini-3.1-flash-tts-preview">3.1 Flash TTS Preview</option><option value="gemini-2.5-flash-preview-tts">2.5 旧版 · 将于2026-11-17停用</option></select></label><label data-gemini><input name="dataUseConsent" type="checkbox">我了解 Gemini 所用套餐的数据用途，同意发送固定试听文案</label>
      <details data-site-help hidden><summary>站点 API Key 配置方法</summary><p>默认通过此页面保存到 Linux 私有目录。也可自行创建仅本人可读的 Key 文件，通过 WEB_SPEECH_AZURE_KEY_FILE 或 WEB_SPEECH_GEMINI_KEY_FILE 指定。变量只填路径，不填 Key。只有改变文件路径才需要重新启动；更新文件内的 Key 立即生效。HTTP 页面不能提交站点 Key，请改用 HTTPS 或 Windows 本机入口。不要放入 Git 或前端资源。</p></details>
      <p class="speech-limit">试听文案：“欢迎来到星云，神树将为你播报新的故事。”Kokoro 支持男女预置与语速；设备朗读支持所选中文音色、语速和音调。设备朗读直接出声，不生成可下载音频。云服务另受其套餐约束。</p>
      <p data-key-state></p><footer><p class="speech-selection" data-selection></p><button type="submit">保存设置</button><button type="button" data-preview>试听当前音色</button><button type="button" data-cancel-preview hidden>停止试听</button></footer></fieldset>
      <p role="status" aria-live="polite" data-status>正在读取设置…</p>
      <section class="speech-player" hidden><strong>试听播放器</strong><audio controls preload="auto" aria-label="语音试听播放器"></audio><div><button type="button" data-play>▶ 播放声音</button><button type="button" data-stop>停止播放</button></div><small>合成完成后若没有自动出声，请点击播放；也可检查系统音量和浏览器标签页是否静音。</small></section>
    </form>`;
  document.body.append(dialog);
  const form=dialog.querySelector('form'),status=dialog.querySelector('[data-status]'),audio=dialog.querySelector('audio'),fields=dialog.querySelector('[data-fields]');
  // Only the form body scrolls. Keep close, feedback and test actions visible on short screens.
  const header=dialog.querySelector('header'),actions=dialog.querySelector('footer'),scroll=element('section');scroll.className='speech-scroll';
  form.id='nebula-speech-settings-form';actions.className='speech-actions';actions.querySelector('[type=submit]').setAttribute('form',form.id);
  for(const node of [...dialog.childNodes])if(node!==header)scroll.append(node);
  actions.prepend(status);dialog.append(scroll,actions);
  // Device synthesis belongs to a web page, not the C++ site/native renderer.
  if(site||!web)form.elements.namedItem('provider').querySelector('option[value="browser"]')?.remove();
  // C++ site/native profiles deliberately retain their narrower validated pitch range.
  if(site||!web)for(const option of [...form.elements.namedItem('pitch').options])if(Number(option.value)<-6||Number(option.value)>3)option.remove();
  const f=name=>form.elements.namedItem(name),base=site?'/api/admin/speech':'/speech';
  f('apiKey').closest('label').after(dialog.querySelector('[data-key-state]'));
  const controller=new AbortController();let blobUrl='',disposed=false,profiles=[],busy=true,renderedProvider='',knownVoices=new Map(),previewController,previewEpoch=0;
  const signal=controller.signal;
  const setStatus=text=>{if(!disposed)status.textContent=text;};
  const setBusy=value=>{busy=value;if(!disposed){fields.disabled=value;for(const b of actions.querySelectorAll('button'))b.disabled=value;const stop=actions.querySelector('[data-cancel-preview]');stop.hidden=!value||!previewController;stop.disabled=false;}};
  const selection=()=>{const provider=f('provider').selectedOptions[0]?.textContent||'',voice=f('provider').value==='browser'?f('browserVoice').selectedOptions[0]?.textContent:web&&f('cloudVoice').value?f('cloudVoice').selectedOptions[0]?.textContent:f('voice').selectedOptions[0]?.textContent;return `${provider} · ${voice||'预置音色'}`;};
  const resetPreview=()=>{previewEpoch++;previewController?.abort();previewController=null;api.cancelPreview?.();audio.pause();audio.removeAttribute('src');audio.load();dialog.querySelector('.speech-player').hidden=true;if(blobUrl){URL.revokeObjectURL(blobUrl);blobUrl='';}};
  const styles=()=>{const current=f('style').value,male=f('voice').value==='zm_009';f('style').replaceChildren(...(male?[['narration-relaxed','轻松叙述'],['chat','自然聊天'],['serious','沉稳']]:[['calm','沉静成熟'],['gentle','柔和'],['serious','沉稳']]).map(([v,t])=>new Option(t,v)));if([...f('style').options].some(o=>o.value===current))f('style').value=current;};
  const cloudVoices=()=>{const provider=f('provider').value,list=CLOUD_VOICES[provider]||[],current=f('cloudVoice').value;
    knownVoices=new Map(list.map(([id,,gender])=>[id,gender]));
    f('cloudVoice').replaceChildren(new Option('跟随上方预置音色',''),...list.map(([id,name,gender,desc])=>new Option(`${name} · ${gender} · ${desc}`,id)));
    if(list.some(([id])=>id===current))f('cloudVoice').value=current;};
  let renderedCloud='';
  const update=()=>{styles();const provider=f('provider').value;dialog.querySelector('[data-speed]').textContent=Number(f('speed').value).toFixed(2)+'×';
    if(renderedProvider&&renderedProvider!==provider){f('apiKey').value='';f('removeKey').checked=false;}renderedProvider=provider;
    if(renderedCloud!==provider){renderedCloud=provider;cloudVoices();if(!filling)f('cloudVoice').value='';}
    const keyless=provider==='local'||provider==='browser';
    const labels=provider==='azure'?['女声 · 晓墨','男声 · 云希']:provider==='gemini'?['女声 · Kore','男声 · Charon']:['女声 · Kokoro','男声 · Kokoro'];[...f('voice').options].forEach((option,i)=>option.textContent=labels[i]);
    for(const item of dialog.querySelectorAll('[data-azure]'))item.hidden=provider!=='azure';
    for(const item of dialog.querySelectorAll('[data-key-row]'))item.hidden=keyless;
    dialog.querySelector('[data-cloud]').hidden=keyless;for(const item of dialog.querySelectorAll('[data-gemini]'))item.hidden=provider!=='gemini';f('model').disabled=site;
    dialog.querySelector('[data-browser]').hidden=provider!=='browser';dialog.querySelector('[data-browser-voices]').hidden=provider!=='browser';
    f('voice').closest('label').hidden=provider==='browser';dialog.querySelector('[data-prosody]').hidden=!['azure','browser'].includes(provider);
    dialog.querySelector('[data-load-voices]').hidden=!web||site||provider!=='azure';
    dialog.querySelector('[data-voice-help]').hidden=!web||site||provider!=='azure';
    dialog.querySelector('[data-azure-guide]').hidden=provider!=='azure';
    dialog.querySelector('[data-custom-voice]').hidden=!web||keyless;dialog.querySelector('[data-site-help]').hidden=!site||keyless;dialog.querySelector('[data-key-state]').hidden=keyless;
    for(const name of ['style','region'])f(name).disabled=provider!=='azure';f('pitch').disabled=!['azure','browser'].includes(provider);
    dialog.querySelector('[data-endpoint]').textContent=provider==='local'?'当前服务：已有服务器 Kokoro · 无云接口费用':provider==='browser'?'当前服务：浏览器/操作系统语音 · 无需应用 API Key':provider==='azure'?`接口：https://${f('region').value||'区域'}.tts.speech.microsoft.com/cognitiveservices/v1`:'接口：https://generativelanguage.googleapis.com · '+f('model').value;
    dialog.querySelector('[data-selection]').textContent='当前试听目标：'+selection();
  };
  let filling=false;
  const fill=p=>{filling=true;try{
      if(p?.provider)f('provider').value=String(p.provider);
      renderedCloud='';update(); // 先按目标 provider 重建云音色选项，再回填
      for(const[k,v]of Object.entries(p||{})){const input=f(k);if(input&&k!=='style'&&k!=='browserVoice'){if(input.type==='checkbox')input.checked=Boolean(v);else input.value=String(v);}}
      if(p?.browserVoice)f('browserVoice').replaceChildren(new Option('已保存音色 · 获取列表查看详情',String(p.browserVoice)));
      styles();if(p?.style)f('style').value=p.style;
    }finally{filling=false;}
    update();
  };
  const keyState=settings=>{dialog.querySelector('[data-key-state]').textContent=`Azure Key：${settings.azureKeyConfigured?'已配置':'未配置'} · Gemini Key：${settings.geminiKeyConfigured?'已配置':'未配置'}${web?'（仅当前页面）':''}`;};
  dialog.querySelector('[data-close]').onclick=()=>dialog.close();
  const accountChanged=()=>dialog.close();window.addEventListener('nebula:user',accountChanged);
  dialog.addEventListener('close',()=>{disposed=true;window.removeEventListener('nebula:user',accountChanged);controller.abort();resetPreview();f('apiKey').value='';dialog.remove();},{once:true});
  form.oninput=()=>{resetPreview();update();setStatus('设置已更改。点击“试听当前音色”测试新的声音。');};
  f('provider').onchange=()=>{f('freeTierConfirmed').checked=false;f('dataUseConsent').checked=false;update();};f('voice').onchange=()=>{if(site){const p=profiles.find(p=>p.voice===f('voice').value);if(p)fill(p);}f('cloudVoice').value='';update();};
  f('cloudVoice').onchange=()=>{const gender=knownVoices.get(f('cloudVoice').value);if(gender)f('voice').value=gender==='男'?'zm_009':'zf_001';update();};
  async function save(){const payload={provider:f('provider').value,voice:f('voice').value,style:f('style').value,region:f('region').value,speed:Number(f('speed').value),pitch:Number(f('pitch').value),freeTierConfirmed:f('freeTierConfirmed').checked,dataUseConsent:f('dataUseConsent').checked};
    if(!site){payload.apiKey=f('apiKey').value;payload.removeKey=f('removeKey').checked;payload.model=f('model').value;if(web){payload.cloudVoice=f('cloudVoice').value.trim();payload.browserVoice=f('browserVoice').value;}}
    try{
      if(site&&(f('apiKey').value||f('removeKey').checked)&&location.protocol!=='https:'&&!['localhost','127.0.0.1','[::1]'].includes(location.hostname))throw new Error('站点 Key 需要安全连接，请从 HTTPS 或 Windows 本机入口打开管理中心后再保存。');
      await api.request(base+'/settings',{method:'PATCH',signal,data:site?Object.fromEntries(Object.entries(payload).map(([k,v])=>[k,String(v)])):payload});if(site&&(f('apiKey').value||f('removeKey').checked)){
        await api.request(base+'/credentials',{method:'PATCH',signal,data:{provider:payload.provider,apiKey:f('apiKey').value,removeKey:String(f('removeKey').checked)}});
      }
      setStatus('设置已保存。');if(site)keyState((await api.request(base+'/settings',{signal})).runtime||{});if(!site)keyState(await api.request(base+'/settings',{signal}));}
    finally{f('apiKey').value='';f('removeKey').checked=false;}
  }
  async function playback(){const result=await playSpeech(audio);setStatus(result.message);}
  dialog.querySelector('[data-load-voices]').onclick=async()=>{
    if(busy)return;setBusy(true);
    try{await save();setStatus('正在查询你的 Azure 资源可用音色…');
      const voices=await api.request('/speech/voices',{signal});if(disposed)return;
      const selected=f('cloudVoice').value;
      knownVoices=new Map(voices.map(v=>[v.id,v.gender]));
      f('cloudVoice').replaceChildren(new Option('跟随上方预置音色',''),...voices.map(v=>new Option(`${v.name} · ${v.gender} · ${v.locale} (${v.id})`,v.id)));
      if(voices.some(v=>v.id===selected))f('cloudVoice').value=selected;
      setStatus(`已获取 ${voices.length} 个中文音色。列表查询成功不等于合成已通过，请选择音色后点击连接试听。`);
      update();
    }catch(e){if(!disposed)setStatus(e.message);}finally{setBusy(false);}
  };
  dialog.querySelector('[data-play]').onclick=()=>playback().catch(e=>setStatus(e.message));
  dialog.querySelector('[data-browser-voices]').onclick=async()=>{
    if(busy)return;setBusy(true);
    try{await save();const current=f('browserVoice').value,rows=await api.request('/speech/voices',{signal});if(disposed)return;
      f('browserVoice').replaceChildren(new Option('自动选择可用中文音色',''),...rows.map(v=>new Option(`${v.name} · ${v.locale} · ${v.local?'本机':'在线'}`,v.id)));
      if(rows.some(v=>v.id===current))f('browserVoice').value=current;
      setStatus(rows.length?`已找到 ${rows.length} 个实际可用中文音色。选择后点击“试听当前音色”。`:'当前设备没有中文音色，可直接改选服务器 Kokoro，无需安装。');update();
    }catch(e){if(!disposed)setStatus(e.message);}finally{setBusy(false);}
  };
  dialog.querySelector('[data-cancel-preview]').onclick=()=>{resetPreview();setBusy(false);setStatus('试听已停止。');};
  dialog.querySelector('[data-stop]').onclick=()=>{audio.pause();audio.currentTime=0;setStatus('播放已停止，可再次点击播放。');};
  audio.onerror=()=>setStatus('音频无法解码或加载失败，请重新生成试听。');
  audio.onended=()=>setStatus('试听播放结束。可修改设置后重新测试。');
  form.onsubmit=async event=>{event.preventDefault();if(busy)return;setBusy(true);try{await save();}catch(e){setStatus(e.message);}finally{setBusy(false);}};
  dialog.querySelector('[data-preview]').onclick=async()=>{if(busy)return;resetPreview();previewController=new AbortController();const attempt=previewEpoch,previewSignal=AbortSignal.any([signal,previewController.signal]);setBusy(true);const target=selection();
    try{await save();previewSignal.throwIfAborted();setStatus(f('provider').value==='browser'?'正在设备上朗读：'+target+'。可点击“停止试听”。':'正在连接并生成：'+target+'。首次本地加载可能需要一两分钟…');
      let state=await api.request(base+'/preview',{method:'POST',signal:previewSignal,data:{voice:f('voice').value}});
      if(previewSignal.aborted||attempt!==previewEpoch)return;
      if(state.state==='spoken'){setStatus(`设备试听播放结束。实际音色：${state.voiceName} · ${state.local?'本机合成':'浏览器在线音色'}。`);return;}
      const voice=state.voice||f('voice').value,deadline=Date.now()+125000;
      while(!disposed&&!previewSignal.aborted&&state.state==='pending'&&Date.now()<deadline){await new Promise(resolve=>{const timer=setTimeout(done,700);function done(){previewSignal.removeEventListener('abort',done);clearTimeout(timer);resolve();}previewSignal.addEventListener('abort',done,{once:true});});if(!disposed&&!previewSignal.aborted)state=await api.request(base+'/preview/'+voice,{signal:previewSignal});}
      previewSignal.throwIfAborted();
      if(disposed)return;if(state.state!=='ready')throw new Error(state.error||'合成失败或等待超时，请检查服务器语音 Worker。');
      const blob=site?await fetchWave(state.url,{signal:previewSignal}):await requireWave(await api.audio('/speech/preview/'+voice+'/audio',{signal:previewSignal}));
      if(disposed||previewSignal.aborted||attempt!==previewEpoch)return;if(blobUrl)URL.revokeObjectURL(blobUrl);blobUrl=URL.createObjectURL(blob);audio.src=blobUrl;dialog.querySelector('.speech-player').hidden=false;
      dialog.querySelector('.speech-player').scrollIntoView({block:'nearest',behavior:matchMedia('(prefers-reduced-motion: reduce)').matches?'instant':'smooth'});
      const runtime=site?(await api.request(base+'/settings',{signal})).runtime:state;const result=await playSpeech(audio);
      setStatus(`试听目标：${target}。实际服务：${runtime?.actualProvider||'local'}${runtime?.fallbackReason?'；已回退：'+runtime.fallbackReason:''}。${result.message}`);
    }catch(e){if(!disposed&&!previewSignal.aborted&&attempt===previewEpoch)setStatus(e.message);}finally{if(attempt===previewEpoch){previewController=null;setBusy(false);}}
  };
  setBusy(true);dialog.showModal();update();
  try{const settings=await api.request(base+'/settings',{signal});if(disposed)return;
    if(site){profiles=settings.profiles||[];fill(profiles[0]||{provider:'local',voice:'zf_001'});keyState(settings.runtime||{});setStatus(settings.available?'选择音色后，点击“连接并试听当前音色”。':'云语音驱动未启用，站点设置暂不可保存。');setBusy(!settings.available);}
    else{fill(settings);keyState(settings);setStatus('免费使用：选择服务器 Kokoro，或获取浏览器设备中文音色后试听。已有云 Key 的用户可选云服务。');setBusy(false);}
  }catch(e){setStatus(e.message);}
}
