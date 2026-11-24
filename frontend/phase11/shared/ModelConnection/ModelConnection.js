// Key 仅保存在当前页面模块内存；不进入 Cookie、Web Storage、URL 或本站服务器。
let connection={provider:'server',endpoint:'',model:'',key:''};
export function modelConnection(){return {...connection};}
export function resetModelConnection(){connection={provider:'server',endpoint:'',model:'',key:''};}
const localHosts=['localhost','127.0.0.1','[::1]'];

export function restoreModelMode(mode, ollama) {
  resetModelConnection();
  if(mode==='ollama'&&ollama){
    const value={provider:'ollama',endpoint:ollama.endpoint,model:ollama.model,key:''};
    try { validateConnection(value); connection=value; } catch { /* Invalid browser preferences fall back to site models. */ }
  }else if(mode==='custom')connection.provider='custom';
}

export function validateConnection(value){
  if(value.provider==='server')return;
  if(!['custom','ollama'].includes(value.provider)||value.endpoint.length>2048)throw Error('无效的模型连接方式或地址。');
  if(typeof location!=='undefined'&&location.protocol!=='https:'&&!localHosts.includes(location.hostname))throw Error('请通过 HTTPS 页面或 Windows 本机入口设置模型连接。');
  const url=new URL(value.endpoint);
  const loopback=localHosts.includes(url.hostname);
  if(url.username||url.password||url.search||url.hash||(!loopback&&url.protocol!=='https:')||!['https:','http:'].includes(url.protocol))throw Error('请输入有效的 HTTPS 接口地址。本机地址允许 HTTP，地址内请勿填写 Key。');
  if(value.provider==='ollama'&&(!loopback||!['/','/api/chat'].includes(url.pathname)))throw Error('Ollama 请填写本机地址，例如 http://127.0.0.1:11434。');
  if(!value.model.trim()||value.model.length>160||value.key.length>4096||/[\r\n]/.test(value.key))throw Error('请填写有效的模型 ID 和凭据。');
}

async function responseText(response, limit) {
  if (!response.body) return '';
  const reader = response.body.getReader(), decoder = new TextDecoder();
  let text = '', bytes = 0;
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      bytes += value.length;
      if (bytes > limit) {
        await reader.cancel();
        throw new Error('模型响应超过读取上限。');
      }
      text += decoder.decode(value, { stream: true });
    }
    return text + decoder.decode();
  } finally { reader.releaseLock(); }
}

/** 自带 API 由浏览器直接连接，不让服务器成为任意网络代理；服务商须允许本站 Origin 的 CORS。 */
export async function requestOwnModel(value,messages,{signal,maxTokens=512}={}){
  validateConnection(value);
  const native=value.provider==='ollama';
  const target=native?new URL('/api/chat',value.endpoint).href:value.endpoint;
  const headers={'Content-Type':'application/json'};if(!native&&value.key)headers.Authorization='Bearer '+value.key;
  const limits=native?{options:{num_predict:maxTokens}}:{max_tokens:maxTokens};
  const response=await fetch(target,{method:'POST',credentials:'omit',referrerPolicy:'no-referrer',redirect:'error',signal,headers,
    body:JSON.stringify({model:value.model,messages,stream:false,...limits})});
  if(!response.ok){
    if([400,413].includes(response.status)){
      const data=await responseText(response,4096).then(JSON.parse).catch(()=>null);
      signal?.throwIfAborted();
      const code=data?.error?.code ?? data?.code;
      if(['context_length_exceeded','context_window_exceeded','max_context_length_exceeded'].includes(code)){
        const error=new Error('当前模型的上下文容量不足。');
        error.code='context_length_exceeded';
        throw error;
      }
    }
    throw Error(`模型服务返回 HTTP ${response.status}，请检查地址、Key、模型权限与额度。`);
  }
  const text=await responseText(response,262144);
  let data;try{data=JSON.parse(text);}catch{throw Error('接口未返回有效 JSON，请确认使用 Chat Completions 兼容协议。');}
  const reply=native?data?.message?.content:data?.choices?.[0]?.message?.content;
  if(typeof reply!=='string'||!reply.trim())throw Error('连接到了接口，但未收到有效模型回答，不能判定连接成功。');return reply;
}
export function openModelConnection({testServer,onChange=()=>{},serverModels=[],serverModel=''}={}){
  const dialog=document.createElement('dialog');dialog.className='model-connection';
  dialog.innerHTML='<header><h2>小研的模型连接</h2><button type=button data-close aria-label="关闭模型连接">×</button></header><form><label>连接方式<select name=provider><option value=server>本站模型 · Linux</option><option value=ollama>本机 Ollama · 浏览器直连</option><option value=custom>自带 API · OpenAI 兼容</option></select></label><label data-server>选择本站模型<select name=serverModel aria-label="检测的本站模型"></select></label><section data-custom hidden><label>接口地址<input name=endpoint type=url maxlength=2048></label><label>模型 ID<input name=model maxlength=160></label><label data-key>API Key<input name=key type=password autocomplete=new-password maxlength=4096 placeholder="仅保留在当前页面内存"></label></section><p data-route></p><label><input name=consent type=checkbox>我了解当前连接的数据流向。测试会发送固定的简短文本，外部服务商可能计费。</label><p role=status aria-live=polite></p><footer><button type=button data-test>测试连接</button><button type=submit>使用此连接</button></footer></form>';
  const style=document.querySelector('[data-model-connection-css]');if(!style){const css=document.createElement('link');css.rel='stylesheet';css.href='/phase11/shared/ModelConnection/ModelConnection.css?v=9fcaa747c305a8cc';css.dataset.modelConnectionCss='';document.head.append(css);}
  document.body.append(dialog);const form=dialog.querySelector('form'),status=dialog.querySelector('[role=status]'),f=name=>form.elements.namedItem(name);let tested='',controller;
  for(const name of ['provider','endpoint','model','key'])f(name).value=connection[name];
  for(const model of serverModels)f('serverModel').append(new Option(`${model.name} · ${model.runtime}`,String(model.accountId)));
  if(!serverModels.length)f('serverModel').append(new Option('未发现可用的本站模型',''));
  if(serverModels.some(model=>String(model.accountId)===serverModel))f('serverModel').value=serverModel;
  const payload=()=>({provider:f('provider').value,endpoint:f('endpoint').value.trim(),model:f('model').value.trim(),
    key:f('provider').value==='custom'?f('key').value:'',serverModel:f('serverModel').value});
  const signature=v=>JSON.stringify(v);
  const update=()=>{
    const mode=f('provider').value;
    dialog.querySelector('[data-server]').hidden=mode!=='server';
    dialog.querySelector('[data-custom]').hidden=mode==='server';
    dialog.querySelector('[data-key]').hidden=mode!=='custom';
    f('endpoint').placeholder=mode==='ollama'?'http://127.0.0.1:11434':'https://provider.example/v1/chat/completions';
    f('model').placeholder=mode==='ollama'?'填写 ollama list 中的模型 ID':'填写服务商提供的 model ID';
    dialog.querySelector('[data-route]').textContent={server:'输入和当前上下文发给本站 Linux 模型。',ollama:'浏览器直接连接本机 Ollama。若连接被浏览器拒绝，请检查 Ollama 的来源白名单。',custom:'输入、当前上下文和 Key 直接发给所填服务商。Key 只在当前页面内存保留。'}[mode];
    tested='';status.textContent='';
  };
  form.oninput=update;update();
  dialog.querySelector('[data-close]').onclick=()=>dialog.close();dialog.addEventListener('close',()=>{controller?.abort();f('key').value='';dialog.remove();},{once:true});
  dialog.querySelector('[data-test]').onclick=async()=>{const button=dialog.querySelector('[data-test]');try{
    if(!f('consent').checked)throw Error('请先确认非保密信息提示。');const value=payload();validateConnection(value);controller=new AbortController();button.disabled=true;status.textContent='正在测试真实连接…';const start=performance.now();
    const timeout=setTimeout(()=>controller.abort(),120000);let reply;try{reply=value.provider==='server'?await testServer?.(controller.signal,value.serverModel):await requestOwnModel(value,[{role:'user',content:'Reply briefly with OK.'}],{signal:controller.signal,maxTokens:8});}finally{clearTimeout(timeout);}
    if(signature(payload())!==signature(value))throw Error('测试期间连接已修改，请重新检测。');
    if(!reply)throw Error('未收到模型响应。');tested=signature(value);status.textContent=`连接成功 · ${Math.round(performance.now()-start)} ms · ${String(reply).slice(0,80)}`;
  }catch(e){tested='';status.textContent=e.name==='TypeError'?'无法连接：检查网络、CORS、HTTPS 证书或浏览器对本地网络的访问限制。':e.name==='AbortError'?'测试已取消或超时，未确认连接成功。':e.message;}finally{button.disabled=false;}};
  form.onsubmit=e=>{e.preventDefault();try{const value=payload();if(!f('consent').checked)throw Error('请先确认非保密信息提示。');validateConnection(value);if(tested!==signature(value))throw Error('请先测试当前连接并确认成功。');connection=value;onChange(value);dialog.close();}catch(error){status.textContent=error.message;}};
  dialog.showModal();return dialog;
}
