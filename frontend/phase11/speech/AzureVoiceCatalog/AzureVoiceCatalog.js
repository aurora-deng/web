import { boundedSpeechBody } from '../../shared/SpeechPlayback/SpeechPlayback.js?v=144f1e6009a3d461';

/** Query the user's own resource, never a relay. Only voice metadata leaves this helper. */
export async function fetchAzureVoices(profile,key,{fetcher=fetch,signal}={}) {
  signal?.throwIfAborted();
  if(!/^[a-z0-9-]{1,32}$/.test(profile.region))throw new Error('Azure 区域格式无效。');
  if(typeof key!=='string'||!key||key.length>512||/[\r\n]/.test(key))throw new Error('请先填写自己的 Azure Key。');
  const timeout=AbortSignal.timeout(15000),combined=signal?AbortSignal.any([signal,timeout]):timeout;
  let response;
  try {
    response=await fetcher(`https://${profile.region}.tts.speech.microsoft.com/cognitiveservices/voices/list`,{
      headers:{'Ocp-Apim-Subscription-Key':key},signal:combined,
      credentials:'omit',redirect:'error',referrerPolicy:'no-referrer',cache:'no-store',
    });
    if(!response.ok)throw new Error(response.status===401||response.status===403?'Azure 拒绝了 Key，请检查 Key 与资源区域。':response.status===429?'Azure 正在限流，请稍后再获取音色。':`音色查询返回 ${response.status}。`);
    const raw=await (await boundedSpeechBody(response,1024*1024)).text();
    combined.throwIfAborted();
    let rows;try{rows=JSON.parse(raw);}catch{throw new Error('Azure 音色列表格式无效。');}
    if(!Array.isArray(rows)||rows.length>4096)throw new Error('Azure 音色列表超出支持范围。');
    const seen=new Set(),voices=[];
    for(const row of rows) {
      if(!row||typeof row.Locale!=='string'||!row.Locale.startsWith('zh-'))continue;
      const id=row.ShortName;
      if(typeof id!=='string'||!/^[A-Za-z0-9-]{1,80}$/.test(id)||seen.has(id))continue;
      seen.add(id);
      voices.push({id,name:typeof row.LocalName==='string'?row.LocalName.slice(0,80):id,
        gender:row.Gender==='Female'?'女':row.Gender==='Male'?'男':'其他',locale:row.Locale.slice(0,32)});
      if(voices.length>512)throw new Error('中文音色数量超出支持范围。');
    }
    if(!voices.length)throw new Error('该资源未返回可用中文音色。');
    return voices;
  } catch(error) {
    if(signal?.aborted)throw error;
    if(error.name==='TimeoutError'||error.name==='AbortError')throw new Error('获取音色超时，请检查网络。');
    if(error instanceof TypeError)throw new Error('无法连接 Azure，请检查网络或浏览器跨域限制。');
    throw error;
  }
}
