import { webAiApi } from "../../ai/WebAiApi/WebAiApi.js?v=144f1e6009a3d461";
import { BusinessApi } from '../BusinessApi/BusinessApi.js?v=144f1e6009a3d461';
import { modelConnection,openModelConnection,requestOwnModel,resetModelConnection } from '../ModelConnection/ModelConnection.js?v=144f1e6009a3d461';
const node=(tag,text,cls)=>{const n=document.createElement(tag);if(text)n.textContent=text;if(cls)n.className=cls;return n;};
const id=()=>Array.from(crypto.getRandomValues(new Uint8Array(20)),n=>n.toString(16).padStart(2,'0')).join('');

/** 普通网页可用的非保密助手：与 Windows 的私人资料/记忆/历史完全分离。 */
export class WebAssistantPanel {
  constructor(host,{surface='sidebar',close}={}){
    this.host=host;this.api=new BusinessApi();this.surface=surface;this.close=close;this.shortcut='Alt+j';this.messages=[];
    host.innerHTML='<header class="assistant-heading"><span class="assistant-orb">✦</span><div><strong>个人 AI 好友</strong><small>网页版 · 非保密模式</small></div><button type=button data-close aria-label="关闭助手">×</button></header><p class="assistant-status" role=status>正在读取账号与可用模型…</p><div class="web-assistant-notice"><strong>发送前请确认信息范围</strong><p>本站模型会收到输入及本段对话上下文。自带 API 会收到输入及 Key。不要发送密码、隐私文件或保密信息。</p><label><input data-web-consent type=checkbox>我了解信息会上传，只使用非保密内容</label><p>需要私人资料与本地模型，请使用 Windows 客户端并确认推理目标为本机；仅使用客户端并不等于推理完全离线。</p></div><div class="assistant-threadbar"><select data-server-model aria-label="本站模型"></select><button type=button data-connection>模型连接 / 测试</button><button type=button data-clear>新对话</button></div><div class="assistant-messages" aria-live=polite></div><form class="assistant-composer"><textarea required maxlength=8000 rows=3 placeholder="发送非保密的问题，开始与 AI 好友聊天…"></textarea><footer><small data-provider>使用本站模型</small><button type=button data-stop disabled>停止</button><button type=submit>发送 ↗</button></footer></form>';
    host.querySelector('[data-close]').onclick=()=>close?.();host.querySelector('[data-close]').hidden=!close;
    this.status=host.querySelector('[role=status]');this.list=host.querySelector('.assistant-messages');this.input=host.querySelector('textarea');
    host.querySelector('[data-connection]').onclick=()=>openModelConnection({testServer:signal=>this.generateServer([{role:'user',content:'Reply briefly with OK.'}],signal,8),onChange:()=>this.provider()});
    host.querySelector('[data-clear]').onclick=()=>{if(this.controller){this.status.textContent='请先停止当前生成';return;}this.messages=[];this.paint();this.save();};
    host.querySelector('[data-stop]').onclick=()=>this.stop();host.querySelector('form').onsubmit=e=>{e.preventDefault();this.send();};
    this.input.onkeydown=e=>{if(e.key==='Enter'&&!e.shiftKey&&!e.isComposing){e.preventDefault();host.querySelector('form').requestSubmit();}};
  }
  async start(){if(this.started)return;try{const user=await this.api.restore();this.owner=String(user.id);this.storageKey='nebula:web-chat:'+this.owner+':'+this.surface;
    try{const saved=JSON.parse(sessionStorage.getItem(this.storageKey)||'[]');if(Array.isArray(saved))this.messages=saved.filter(m=>['user','assistant'].includes(m.role)&&typeof m.content==='string').slice(-30);}catch{}
    const models=await this.api.request(webAiApi.getAiWebModels({}));const select=this.host.querySelector('[data-server-model]');select.replaceChildren();for(const m of models){const option=node('option',m.name+' · '+m.runtime);option.value=m.accountId;select.append(option);}if(!models.length){const option=node('option','本站暂无已配置模型，可自带 API');option.value='';select.append(option);}
    this.status.textContent='网页登录已就绪 · 历史仅保留在本标签页会话存储，不与 Windows 私人空间混用';this.started=true;this.paint();this.provider();
  }catch(e){this.status.textContent=e.status===401?'请先前往消息页面登录，随后重新打开助手。':e.message;}}
  provider(){const c=modelConnection();this.host.querySelector('[data-provider]').textContent=c.provider==='server'?'本站模型 · 输入发送到 Linux':'自带 API · '+c.model;}
  save(){try{sessionStorage.setItem(this.storageKey,JSON.stringify(this.messages.slice(-30)));}catch{/* 禁用存储时仍可当前页面对话。 */}}
  paint(){this.list.replaceChildren(...this.messages.map(m=>{const b=node('article',undefined,'assistant-bubble '+m.role);b.append(node('small',m.role==='user'?'我':'AI 好友'),node('p',m.content));return b;}));if(!this.messages.length)this.list.append(node('div','AI 好友一直在这里，写下你的第一个问题吧。','assistant-empty'));this.list.scrollTop=this.list.scrollHeight;}
  async generateServer(messages,signal,maxOutputTokens=512,onText=()=>{}){
    if(!this.host.querySelector('[data-web-consent]').checked)throw Error('请先在助手中确认非保密信息提示。');
    const aiUserId=this.host.querySelector('[data-server-model]').value;if(!aiUserId)throw Error('本站暂无可用模型，请打开模型连接配置自己的 API。');
    const requestId=id(),payload={requestId,aiUserId,nonConfidentialConsent:'true',messageCount:String(messages.length),maxOutputTokens:String(maxOutputTokens)};
    messages.forEach((m,i)=>{payload['role'+i]=m.role;payload['content'+i]=m.content;});let created=false,text='',after=0;
    try{await this.api.request(webAiApi.postAiWebGenerations({}),{method:'POST',data:payload,signal});created=true;
      while(true){if(signal.aborted)throw new DOMException('cancelled','AbortError');const result=await this.api.request(webAiApi.getAiWebGenerationsGeneration({"generation":(requestId)}, "after=" + String(after)),{signal});text+=result.pieces.join('');after=result.next;onText(text);
        if(!['queued','running'].includes(result.state)&&after>=result.next&&result.pieces.length<64){if(result.state!=='completed')throw Error(result.state==='cancelled'?'生成已停止':'本站模型暂时不可用，请检查运行状态或使用自己的 API。');return text;}
        await new Promise((resolve,reject)=>{const abort=()=>{clearTimeout(timer);reject(new DOMException('cancelled','AbortError'));};const timer=setTimeout(()=>{signal.removeEventListener('abort',abort);resolve();},450);signal.addEventListener('abort',abort,{once:true});});
      }
    }finally{if(created)await this.api.request(webAiApi.deleteAiWebGenerationsGeneration({"generation":(requestId)}),{method:'DELETE'}).catch(()=>{});}
  }
  async send(){if(this.controller)return;const text=this.input.value.trim();if(!text)return;if(!this.host.querySelector('[data-web-consent]').checked){this.status.textContent='请先确认信息会上传，只使用非保密内容。';return;}
    if(!this.started){await this.start();if(!this.started)return;}const context=[...this.messages.slice(-12),{role:'user',content:text}];
    if(context.reduce((n,m)=>n+new TextEncoder().encode(m.content).length,0)>30000){this.status.textContent='当前上下文过长，请新建对话。';return;}
    this.messages.push({role:'user',content:text});this.input.value='';this.paint();const reply=node('article',undefined,'assistant-bubble assistant');reply.append(node('small','AI 好友'),node('p','正在思考…'));this.list.append(reply);
    this.controller=new AbortController();this.host.querySelector('[data-stop]').disabled=false;this.status.textContent='正在生成…';const timeout=setTimeout(()=>this.controller?.abort(),180000);
    try{const value=modelConnection();const output=value.provider==='custom'?await requestOwnModel(value,context,{signal:this.controller.signal}):await this.generateServer(context,this.controller.signal,512,t=>reply.querySelector('p').textContent=t||'正在思考…');
      this.messages.push({role:'assistant',content:output});this.messages=this.messages.slice(-30);this.paint();this.save();this.status.textContent='回答完成 · 网页非保密模式';
    }catch(e){reply.querySelector('p').textContent=e.name==='AbortError'?'已停止本次生成。':e.name==='TypeError'?'连接失败，请检查网络、CORS 和模型连接设置。':e.message;this.save();this.status.textContent='本次回答未完成，可检查模型连接后重试。';}
    finally{clearTimeout(timeout);this.controller=null;this.host.querySelector('[data-stop]').disabled=true;}
  }
  stop(){this.controller?.abort();}
  dispose(){this.stop();resetModelConnection();}
}
