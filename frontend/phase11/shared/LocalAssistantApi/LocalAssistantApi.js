import { companionSession } from '../ClientBootstrap/ClientBootstrap.js?v=144f1e6009a3d461';
/** 私人资料只允许走本机专用前缀；不回退到服务器聊天或公开知识上传接口。 */
export class LocalAssistantApi {
  constructor(){this.session=null;this.account='';}
  async start(){this.session=await companionSession();if(!this.session.available||!this.session.authenticated)return{available:false};
    const result=await this.request('/capabilities');this.account=String(result.accountId||'');return result;}
  async request(path,{method='GET',data,body,headers={},signal}={}){
    if(!this.session?.available)throw new Error('请从这台电脑的本机入口打开网页，使用私人助手。');
    headers={...headers,'X-Local-CSRF':this.session.csrfToken||'','X-Nebula-Account':this.account};
    if(data!==undefined)headers['Content-Type']='application/json';
    const response=await fetch('/api/local/assistant'+path,{method,credentials:'same-origin',cache:'no-store',headers,signal,body:data!==undefined?JSON.stringify(data):body});
    const value=await response.json().catch(()=>({}));if(!response.ok){const error=new Error(value.message||`本机助手请求失败（${response.status}）`);error.status=response.status;throw error;}return value;
  }
  async audio(path,{signal}={}){
    if(!path.startsWith('/speech/preview/'))throw new Error('无效的私人试听地址');
    const response=await fetch('/api/local/assistant'+path,{credentials:'same-origin',cache:'no-store',signal,headers:{'X-Local-CSRF':this.session.csrfToken||'','X-Nebula-Account':this.account}});
    if(!response.ok)throw new Error('试听音频尚未准备完成');return response.blob();
  }
}
