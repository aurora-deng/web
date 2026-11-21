(() => {
    async function call(path, body, signal){
        const timeout=new AbortController(),abort=()=>timeout.abort();signal?.addEventListener('abort',abort,{once:true});
        const timer=setTimeout(abort,20000);if(signal?.aborted)abort();
        try{const response=await PrepTransport.fetch(path,{credentials:'same-origin',cache:'no-store',signal:timeout.signal,...(body===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})});const result=await response.json();if(!response.ok)throw new Error(result.error||'请求未完成，草稿仍保留');return result;}
        finally{clearTimeout(timer);signal?.removeEventListener('abort',abort);}
    }
    const delay=(milliseconds,signal)=>new Promise((resolve,reject)=>{const abort=()=>{clearTimeout(timer);reject(new DOMException('已停止','AbortError'));},timer=setTimeout(()=>{signal?.removeEventListener('abort',abort);resolve();},milliseconds);signal?.addEventListener('abort',abort,{once:true});if(signal?.aborted)abort();});
    async function request(kind,body,{signal}={}){
        if(!['generate','preview','confirm','undo'].includes(kind))throw new Error('不支持的助手操作');
        await PrepLocal.flush();if(PrepLocal.hasPending())throw new Error('请先完成资料同步；思考和草稿仍保留在本机');
        if(kind!=='generate')return call('/api/workspace/assistant-'+kind,body,signal);
        let job;
        const stop=()=>{if(job)call('/api/workspace/assistant-cancel',{id:job.id}).catch(()=>{});};
        signal?.addEventListener('abort',stop,{once:true});
        try{
            job=await call('/api/workspace/assistant-generate',{requestId:uid(),body},signal);
            if(signal?.aborted){stop();throw new DOMException('已停止','AbortError');}
            const until=performance.now()+310000;
            while(job.status==='running'){
                if(performance.now()>until){stop();throw new Error('生成超过等待时间，未接纳迟到输出；草稿仍保留');}
                await delay(600,signal);job=await call('/api/workspace/assistant-job?id='+encodeURIComponent(job.id),undefined,signal);
            }
            if(job.status!=='done')throw new Error(job.error||'生成已停止，未写入学习目标');return job.result;
        }finally{signal?.removeEventListener('abort',stop);}
    }
    function setup(){
        if(!Workspace.capabilities()?.features?.includes('assistant-drafts-v1'))return;
        AssistantWorkspace.configure({request,accept:snapshot=>{PrepLocal.accept(snapshot);dispatchEvent(new CustomEvent('workspace-change'));}});
        AssistantWorkspace.install();
    }
    addEventListener('workspace-ready',setup,{once:true});window.AssistantDraftTransport={request};
})();
