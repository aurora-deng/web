const text='欢迎来到星云，神树将为你播报新的故事。';
const xml=value=>String(value).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&apos;'}[c]));
/** Provider hosts are fixed. A speech key can never be sent to a user-supplied URL. */
export function cloudRequest(profile,key) {
  if(!Number.isFinite(profile.speed)||profile.speed<.75||profile.speed>1.25||!Number.isFinite(profile.pitch)||profile.pitch<-20||profile.pitch>20)throw new Error('语速或音调超出支持范围。');
  if(!['calm','gentle','serious','chat','narration-relaxed'].includes(profile.style))throw new Error('不支持此表达方式。');
  const male=profile.voice==='zm_009';
  if(!['azure','gemini'].includes(profile.provider))throw new Error('不支持此云语音服务。');
  if(!key||key.length>512||/[\r\n]/.test(key))throw new Error('请填写有效的云服务 Key，再测试连接。');
  if(profile.provider==='azure') {
    if(!/^[a-z0-9-]{1,32}$/.test(profile.region))throw new Error('Azure 区域格式无效。');
    const voice=profile.cloudVoice||(male?'zh-CN-YunxiNeural':'zh-CN-XiaomoNeural');
    if(!/^[A-Za-z0-9-]{1,80}$/.test(voice))throw new Error('Azure 音色名称格式无效。');
    const rate=Math.round((profile.speed-1)*100),signed=n=>(n>=0?'+':'')+n+'%';
    const prosody=`<prosody rate="${signed(rate)}" pitch="${signed(profile.pitch)}">${xml(text)}</prosody>`;
    // A custom voice must not inherit the other gender preset's unsupported style.
    const preset=voice===(male?'zh-CN-YunxiNeural':'zh-CN-XiaomoNeural');
    return {url:`https://${profile.region}.tts.speech.microsoft.com/cognitiveservices/v1`,options:{method:'POST',headers:{'Ocp-Apim-Subscription-Key':key,'Content-Type':'application/ssml+xml','X-Microsoft-OutputFormat':'riff-24khz-16bit-mono-pcm'},body:`<speak version="1.0" xml:lang="zh-CN" xmlns="http://www.w3.org/2001/10/synthesis" xmlns:mstts="https://www.w3.org/2001/mstts"><voice name="${voice}">${preset?`<mstts:express-as style="${xml(profile.style)}">${prosody}</mstts:express-as>`:prosody}</voice></speak>`}};
  }
  if(profile.provider==='gemini') {
    const voice=profile.cloudVoice||(male?'Charon':'Kore');
    if(!/^[A-Za-z][A-Za-z0-9_-]{0,63}$/.test(voice))throw new Error('Gemini 音色名称格式无效。');
    const model=profile.model||'gemini-3.1-flash-tts-preview';if(!['gemini-3.1-flash-tts-preview','gemini-2.5-flash-preview-tts'].includes(model))throw new Error('不支持此 Gemini 语音模型。');
    return {url:`https://generativelanguage.googleapis.com/v1beta/models/${model}:generateContent`,options:{method:'POST',headers:{'x-goog-api-key':key,'Content-Type':'application/json'},body:JSON.stringify({contents:[{parts:[{text:`请以自然的中文${male?'青年男声':'成熟女声'}朗读，语速比例 ${profile.speed}，有自然停顿，只朗读：${text}`}]}],generationConfig:{responseModalities:['AUDIO'],speechConfig:{voiceConfig:{prebuiltVoiceConfig:{voiceName:voice}}}}})}};
  }
  throw new Error('未知的云语音服务。');
}
export function pcmWave(base64,mime) {
  const [format,...parameters]=String(mime||'').toLowerCase().split(';').map(x=>x.replace(/[ \t]/g,''));
  if(!['audio/l16','audio/pcm'].includes(format)||!parameters.includes('rate=24000')||parameters.some(x=>!['rate=24000','codec=pcm','channels=1'].includes(x))||new Set(parameters).size!==parameters.length||!base64||base64.length>4*1024*1024)throw new Error('云服务返回了不支持的音频格式。');
  const pcm=Uint8Array.from(atob(base64),c=>c.charCodeAt(0));
  if(!pcm.length||pcm.length%2||pcm.length>24000*2*60)throw new Error('语音数据为空或超过一分钟。');
  const bytes=new Uint8Array(44+pcm.length),view=new DataView(bytes.buffer),tag=(at,s)=>[...s].forEach((c,i)=>bytes[at+i]=c.charCodeAt(0));
  tag(0,'RIFF');view.setUint32(4,pcm.length+36,true);tag(8,'WAVE');tag(12,'fmt ');view.setUint32(16,16,true);view.setUint16(20,1,true);view.setUint16(22,1,true);view.setUint32(24,24000,true);view.setUint32(28,48000,true);view.setUint16(32,2,true);view.setUint16(34,16,true);tag(36,'data');view.setUint32(40,pcm.length,true);bytes.set(pcm,44);
  return new Blob([bytes],{type:'audio/wav'});
}
