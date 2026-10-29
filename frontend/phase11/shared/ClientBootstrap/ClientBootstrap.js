let ready;
let watching=false;
function watchIdentity(session){
  if(watching||!session.authenticated)return;watching=true;let account=String(session.accountId||'');
  const originalFetch=window.fetch.bind(window);const channel=typeof BroadcastChannel==='function'?new BroadcastChannel('nebula:companion-identity'):null;
  const changed=(next,broadcast=true)=>{next=String(next||'');if(next===account)return;const previous=account;account=next;session.accountId=next;
    window.dispatchEvent(new CustomEvent('nebula:companion-identity',{detail:{accountId:next,previousAccountId:previous}}));
    if(broadcast)channel?.postMessage({accountId:next});
    // 同源多个页面共用本机登录身份；切号/退出立即清掉旧页面内存与私人DOM。
    if(previous)location.reload();
  };
  window.fetch=async(...args)=>{const response=await originalFetch(...args);if(new URL(response.url,location.href).origin===location.origin&&response.headers.has('X-Nebula-Session-Version'))changed(response.headers.get('X-Nebula-Account')||'');return response;};
  channel?.addEventListener('message',event=>{if(event.data&&typeof event.data.accountId==='string')changed(event.data.accountId,false);});
  const timer=setInterval(async()=>{try{const response=await originalFetch('/api/local/session/info',{credentials:'same-origin',cache:'no-store',signal:AbortSignal.timeout(2000)});if(response.ok){const info=await response.json();changed(info.accountId||'');}}catch{}},5000);
  window.addEventListener('pagehide',()=>{clearInterval(timer);channel?.close();},{once:true});
}
/** 只访问当前网页自己的入口，远程页面不会偷偷探测或上传到本机服务。 */
export function companionSession() {
  if(ready)return ready;
  ready=(async()=>{
    if(!['localhost','127.0.0.1'].includes(location.hostname))return{available:false};
    const url=new URL(location.href),fragment=new URLSearchParams(url.hash.slice(1)),token=fragment.get('companion');
    if(token){
      url.hash='';history.replaceState(null,'',url);
      const response=await fetch('/api/local/session/bootstrap',{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/json'},body:JSON.stringify({token})});
      if(!response.ok)throw new Error('本机入口会话无效，请从本机入口重新打开网页。');
    }
    try{const response=await fetch('/api/local/session/info',{credentials:'same-origin',cache:'no-store'});if(!response.ok)return{available:false};
      const result=await response.json();if(result?.available===true){watchIdentity(result);return result;}return{available:false};
    }catch{return{available:false};}
  })();return ready;
}
