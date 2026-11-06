/** Animate only the content surface; the scene, navigation and data retain their identities. */
export class TreePageTransition {
  constructor({surface,current,apply,viewport=window,root=document}) {
    Object.assign(this,{surface,current,apply,viewport,root});this.positions=new Map();this.animations=[];this.revision=0;
    this.cue=root.createElement('div');this.cue.className='tree-page-cue';this.cue.hidden=true;
    this.cue.setAttribute('role','status');this.cue.setAttribute('aria-live','polite');
    this.label=root.createElement('span');this.bar=root.createElement('i');this.cue.append(this.label,this.bar);root.body.append(this.cue);
  }
  progress(value) {
    this.cue.hidden=!value;if(!value)return;
    const name={tree:'神树全景',feed:'故事广场',rank:'七日心动榜'}[value.target];
    const text=`继续${value.direction>0?'下':'上'}滑 · ${name}`;
    if(this.label.textContent!==text)this.label.textContent=text;
    this.bar.style.transform=`scaleX(${value.amount})`;
  }
  cancel(){for(const animation of this.animations)animation.cancel();this.animations=[];}
  async navigate(view,{direction=0,edge='restore',source='tab',animate=true}={}) {
    if(this.disposed)return false;
    if(view===this.current()){++this.revision;this.cancel();this.progress(null);this.surface.removeAttribute('aria-busy');return false;}
    const revision=++this.revision;this.cancel();this.progress(null);
    this.positions.set(this.current(),this.viewport.scrollY);
    const reduced=this.viewport.matchMedia?.('(prefers-reduced-motion: reduce)').matches;
    const motion=animate&&!reduced&&!!this.surface.animate;
    this.surface.setAttribute('aria-busy','true');
    const run=async(frames,duration)=>{
      const a=this.surface.animate(frames,{duration,easing:'cubic-bezier(.2,.75,.25,1)',fill:'both'});this.animations.push(a);
      try{await a.finished;}catch{/* Superseded navigation/dispose cancels the old animation. */}
    };
    try {
      if(motion)await run([{opacity:1,transform:'translateY(0)'},{opacity:.2,transform:`translateY(${-direction*12}px)`}],100);
      if(this.disposed||revision!==this.revision)return false;
      this.apply(view);
      const top=edge==='end'?Math.max(0,this.root.documentElement.scrollHeight-this.viewport.innerHeight):edge==='start'?0:this.positions.get(view)||0;
      this.viewport.scrollTo({top,behavior:'instant'});this.cancel();
      if(source==='keyboard')this.surface.querySelector('h1')?.focus({preventScroll:true});
      if(motion)await run([{opacity:.2,transform:`translateY(${direction*18}px)`},{opacity:1,transform:'translateY(0)'}],200);
      return !this.disposed&&revision===this.revision;
    } finally {
      if(revision===this.revision){this.cancel();this.surface.removeAttribute('aria-busy');}
    }
  }
  dispose(){this.disposed=true;++this.revision;this.cancel();this.surface.removeAttribute('aria-busy');this.cue.remove();}
}
