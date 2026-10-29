// Key 仅保存在当前页面模块内存；不进入 Cookie、Web Storage、URL 或本站服务器。
let connection={provider:'server',endpoint:'',model:'',key:''};
export function modelConnection(){return {...connection};}
export function resetModelConnection(){connection={provider:'server',endpoint:'',model:'',key:''};}
export function validateConnection(value){
  if(value.provider==='server')return;
  if(value.provider!=='custom'||value.endpoint.length>2048)throw Error('无效的模型连接方式或地址。');
  if(typeof location!=='undefined'&&location.protocol!=='https:'&&!['127.0.0.1','localhost','[::1]'].includes(location.hostname))throw Error('请通过 HTTPS 页面或 Windows 本机入口配置 API Key。');
  const url=new URL(value.endpoint);
  const loopback=['localhost','127.0.0.1','[::1]'].includes(url.hostname);
  if(url.username||url.password||url.search||url.hash||(!loopback&&url.protocol!=='https:')||!['https:','http:'].includes(url.protocol))throw Error('使用 HTTPS 的完整 chat/completions 地址；仅本机地址允许 HTTP，不允许 URL 内包含 Key。');
  if(!value.model.trim()||value.model.length>160||value.key.length>4096||/[\r\n]/.test(value.key))throw Error('请填写有效模型名和 Key。');
}
/** 自带 API 由浏览器直接连接，不让服务器成为任意网络代理；服务商须允许本站 Origin 的 CORS。 */
export async function requestOwnModel(value,messages,{signal,maxTokens=512}={}){
  validateConnection(value);const headers={'Content-Type':'application/json'};if(value.key)headers.Authorization='Bearer '+value.key;
  const response=await fetch(value.endpoint,{method:'POST',credentials:'omit',referrerPolicy:'no-referrer',redirect:'error',signal,headers,
    body:JSON.stringify({model:value.model,messages,stream:false,max_tokens:maxTokens})});
  if(!response.ok)throw Error(`模型服务返回 HTTP ${response.status}，请检查地址、Key、模型权限与额度。`);
  const reader=response.body.getReader(),decoder=new TextDecoder();let text='',bytes=0;try{while(true){const part=await reader.read();if(part.done)break;bytes+=part.value.length;if(bytes>262144){await reader.cancel();throw Error('模型响应超过 256 KiB。');}text+=decoder.decode(part.value,{stream:true});}text+=decoder.decode();}finally{reader.releaseLock();}
  let data;try{data=JSON.parse(text);}catch{throw Error('接口未返回有效 JSON，请确认使用 Chat Completions 兼容协议。');}
  const reply=data?.choices?.[0]?.message?.content;
  if(typeof reply!=='string'||!reply.trim())throw Error('连接到了接口，但未收到有效模型回答，不能判定连接成功。');return reply;
}
export function openModelConnection({testServer,onChange=()=>{}}={}){
  const dialog=document.createElement('dialog');dialog.className='model-connection';
  dialog.innerHTML='<header><div><small>MODEL CONNECTION</small><h2>模型连接</h2></div><button type=button data-close aria-label="关闭模型连接">×</button></header><p>网页版仅用于非保密对话。本站模型：内容发送到 Linux；自带 API：内容与 Key 由浏览器直接发送到所填服务商，不经本站代理。</p><form><label>连接方式<select name=provider><option value=server>本站 AI 模型</option><option value=custom>自带 API · Chat Completions 兼容</option></select></label><section data-custom><label>完整接口地址<input name=endpoint type=url placeholder="https://your-provider.example/v1/chat/completions" maxlength=2048></label><label>模型名称<input name=model maxlength=160 placeholder="填写服务商提供的 model ID"></label><label>API Key<input name=key type=password autocomplete=new-password maxlength=4096 placeholder="仅保留在当前页面内存"></label><p>Key 不会保存到浏览器存储；刷新页面需重填。请仅在可信设备填写自己的 Key。服务商需开放 CORS；本地 Ollama 还需允许当前网页来源。地址、模型或 Key 改变后需重新测试。</p></section><label><input name=consent type=checkbox>我了解输入将离开浏览器，且只发送非保密信息；测试会发送固定的简短文案，服务商可能计费。</label><p role=status aria-live=polite></p><footer><button type=button data-test>测试连接</button><button type=submit>使用此连接</button></footer></form>';
  const style=document.querySelector('[data-model-connection-css]');if(!style){const css=document.createElement('link');css.rel='stylesheet';css.href='/phase11/shared/ModelConnection/ModelConnection.css?v=4ec58ab3c1e38adb';css.dataset.modelConnectionCss='';document.head.append(css);}
  document.body.append(dialog);const form=dialog.querySelector('form'),status=dialog.querySelector('[role=status]'),f=name=>form.elements.namedItem(name);let tested='',controller;
  for(const name of ['provider','endpoint','model','key'])f(name).value=connection[name];
  const payload=()=>({provider:f('provider').value,endpoint:f('endpoint').value.trim(),model:f('model').value.trim(),key:f('key').value});
  const signature=v=>JSON.stringify(v);const update=()=>{dialog.querySelector('[data-custom]').hidden=f('provider').value!=='custom';tested='';status.textContent='';};form.oninput=update;update();
  dialog.querySelector('[data-close]').onclick=()=>dialog.close();dialog.addEventListener('close',()=>{controller?.abort();f('key').value='';dialog.remove();},{once:true});
  dialog.querySelector('[data-test]').onclick=async()=>{const button=dialog.querySelector('[data-test]');try{
    if(!f('consent').checked)throw Error('请先确认非保密信息提示。');const value=payload();validateConnection(value);controller=new AbortController();button.disabled=true;status.textContent='正在测试真实连接…';const start=performance.now();
    const timeout=setTimeout(()=>controller.abort(),120000);let reply;try{reply=value.provider==='server'?await testServer?.(controller.signal):await requestOwnModel(value,[{role:'user',content:'Reply briefly with OK.'}],{signal:controller.signal,maxTokens:8});}finally{clearTimeout(timeout);}
    if(!reply)throw Error('未收到模型响应。');tested=signature(value);status.textContent=`连接成功 · ${Math.round(performance.now()-start)} ms · ${String(reply).slice(0,80)}`;
  }catch(e){tested='';status.textContent=e.name==='TypeError'?'无法连接：检查网络、CORS、HTTPS 证书或浏览器对本地网络的访问限制。':e.name==='AbortError'?'测试已取消或超时，未确认连接成功。':e.message;}finally{button.disabled=false;}};
  form.onsubmit=e=>{e.preventDefault();try{const value=payload();if(!f('consent').checked)throw Error('请先确认非保密信息提示。');validateConnection(value);if(tested!==signature(value))throw Error('请先测试当前连接并确认成功。');connection=value;onChange(value);dialog.close();}catch(error){status.textContent=error.message;}};
  dialog.showModal();return dialog;
}
