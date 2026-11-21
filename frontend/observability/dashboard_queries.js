/** Optional dashboard reads have their own lifetime, separate from metric refreshes. */
export class DashboardQueries {
  constructor(api,paths){
    this.api=api;this.paths=Object.keys(paths);this.controller=new AbortController();
    this.states=new Map(this.paths.map(path=>[path,{pending:false,loaded:false,value:null,error:null}]));
  }
  listen(changed){this.changed=changed;this.notify();}
  notify(){if(!this.controller.signal.aborted)this.changed?.(this.states);}
  refresh(bounds){
    for(const path of this.paths)if(!this.states.get(path).pending)this.start(path,bounds);
  }
  retry(bounds){
    for(const [path,state] of this.states)if(state.error&&!state.pending)this.start(path,bounds);
  }
  start(path,bounds){
    if(this.controller.signal.aborted)return;
    const state={...this.states.get(path),pending:true,error:null};
    this.states.set(path,state);this.notify();
    const args=path==='markers'?{...bounds}:null;
    Promise.resolve().then(()=>{
      if(!this.controller.signal.aborted)return this.api(path,args,this.controller.signal);
    }).then(value=>this.finish(path,state,{value,error:null}),
      error=>this.finish(path,state,{value:null,error:error.message||'查询失败'}));
  }
  finish(path,state,result){
    if(this.controller.signal.aborted||this.states.get(path)!==state)return;
    this.states.set(path,{...result,pending:false,loaded:true});this.notify();
  }
  dispose(){this.controller.abort();this.changed=null;}
}
