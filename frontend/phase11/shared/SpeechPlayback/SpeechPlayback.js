/** Validate bytes before giving them to a media element: a JSON/HTML error is not audio. */
export async function requireWave(blob) {
  if(!blob||blob.size<44||blob.size>6*1024*1024)throw new Error('音频为空或超过试听大小限制，请检查语音服务。');
  const bytes=new Uint8Array(await blob.arrayBuffer()),view=new DataView(bytes.buffer);
  const tag=at=>String.fromCharCode(...bytes.slice(at,at+4));
  if(tag(0)!=='RIFF'||tag(8)!=='WAVE'||view.getUint32(4,true)+8!==bytes.length)throw new Error('服务没有返回有效 WAV 音频，可能是登录过期或合成失败。');
  let offset=12,rate=0,data=false,chunks=0;
  while(offset+8<=bytes.length&&++chunks<=16){
    const size=view.getUint32(offset+4,true),start=offset+8;
    if(size>bytes.length-start)throw new Error('WAV 音频不完整，请重新生成。');
    if(tag(offset)==='fmt '){
      if(size<16||view.getUint16(start,true)!==1||view.getUint16(start+2,true)!==1||view.getUint16(start+14,true)!==16||view.getUint16(start+12,true)!==2)throw new Error('试听只支持单声道 16 位 PCM WAV。');
      rate=view.getUint32(start+4,true);if(rate<16000||rate>48000||view.getUint32(start+8,true)!==rate*2)throw new Error('WAV 采样率无效。');
    }
    if(tag(offset)==='data'){if(!rate||!size||size%2||size>rate*2*60)throw new Error('语音数据为空或超过一分钟。');data=true;}
    offset=start+size+(size&1);
  }
  if(!data||offset!==bytes.length)throw new Error('WAV 音频缺少完整声音数据。');
  return new Blob([bytes],{type:'audio/wav'});
}
export async function boundedSpeechBody(response,limit=6*1024*1024) {
  if(Number(response.headers.get('Content-Length'))>limit)throw new Error('语音响应超过大小限制。');
  if(!response.body){const blob=await response.blob();if(blob.size>limit)throw new Error('语音响应超过大小限制。');return blob;}
  const reader=response.body.getReader(),chunks=[];let size=0;
  try{while(true){const {done,value}=await reader.read();if(done)break;size+=value.byteLength;if(size>limit)throw new Error('语音响应超过大小限制。');chunks.push(value);}}
  catch(error){await reader.cancel().catch(()=>{});throw error;}finally{reader.releaseLock();}
  return new Blob(chunks,{type:response.headers.get('Content-Type')||'application/octet-stream'});
}
export async function fetchWave(url,{signal,fetcher=fetch}={}) {
  const chunks=[];let offset=0,total=0;
  do {
    const response=await fetcher(url,{credentials:'same-origin',cache:'no-store',signal,headers:{Range:`bytes=${offset}-${offset+512*1024-1}`}});
    if(!response.ok)throw new Error(response.status===401?'登录已过期，请重新登录后试听。':`音频加载失败（${response.status}），请检查语音服务。`);
    if(response.status===200){if(offset)throw new Error('音频分段状态已改变，请重新试听。');return requireWave(await boundedSpeechBody(response));}
    const range=/^bytes (\d+)-(\d+)\/(\d+)$/.exec(response.headers.get('Content-Range')||'');
    if(response.status!==206||!range)throw new Error('音频分段响应无效。');
    const [start,end,size]=range.slice(1).map(Number);
    if(!Number.isSafeInteger(size)||size<44||size>6*1024*1024||start!==offset||end<start||end>=size||(total&&total!==size))throw new Error('音频分段不完整或超过大小限制。');
    total=size;const chunk=await boundedSpeechBody(response,512*1024);
    if(chunk.size!==end-start+1)throw new Error('音频分段下载中断，请重新试听。');
    chunks.push(chunk);offset=end+1;
  }while(offset<total);
  return requireWave(new Blob(chunks,{type:'audio/wav'}));
}
/** Media playback permission can expire while synthesis runs. Keep an explicit Play button. */
export async function playSpeech(audio) {
  audio.muted=false;audio.volume=1;
  try{await audio.play();return {playing:true,message:'正在播放，可在播放器中暂停或调整音量。'};}
  catch(error){
    if(error.name==='NotAllowedError')return {playing:false,message:'音频已准备好，浏览器需要你再次点击“播放声音”或播放器。'};
    if(error.name==='AbortError')return {playing:false,message:'播放已暂停，可点击“播放声音”继续。'};
    throw new Error('浏览器无法播放这段音频，请重新生成试听。');
  }
}
