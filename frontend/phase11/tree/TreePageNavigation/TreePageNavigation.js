/** Normal scrolling first; a deliberate boundary gesture turns one page, never its momentum. */
export class TreePageNavigation {
  constructor({current,change,progress=()=>{},root=document,viewport=window}) {
    Object.assign(this,{current,change,progress,root,viewport});
    this.amount=0;this.last=-Infinity;this.lockedUntil=0;this.pending=false;this.waitForQuiet=false;
    this.wheel=event=>{
      if(event.defaultPrevented||event.altKey||event.ctrlKey||event.metaKey||event.shiftKey||Math.abs(event.deltaX||0)>Math.abs(event.deltaY)||this.blocked(event.target))return this.reset();
      const delta=event.deltaY*(event.deltaMode===1?16:event.deltaMode===2?viewport.innerHeight:1);
      if(this.step(delta,event.timeStamp))event.preventDefault();
    };
    this.key=event=>{
      if(event.defaultPrevented||this.blocked(event.target)||event.altKey||event.ctrlKey||event.metaKey)return;
      if(['PageDown','PageUp'].includes(event.key)&&this.step(event.key==='PageDown'?180:-180,event.timeStamp,{explicit:true,source:'keyboard'}))event.preventDefault();
    };
    // The 3D canvas owns orbit gestures. Page swipes start outside it.
    this.touchStart=event=>{this.touch=event.touches.length===1&&!event.target?.closest?.('canvas')&&!this.blocked(event.target)?{x:event.touches[0].clientX,y:event.touches[0].clientY}:null;};
    this.touchEnd=event=>{
      const start=this.touch;this.touch=null;if(!start||this.blocked(event.target))return;
      const end=event.changedTouches[0];if(!end)return;
      const distance=start.y-end.clientY;
      if(Math.abs(distance)>90&&Math.abs(end.clientX-start.x)<Math.abs(distance)*.6)this.step(Math.sign(distance)*180,event.timeStamp,{explicit:true,source:'touch'});
    };
    root.addEventListener('wheel',this.wheel,{passive:false});root.addEventListener('keydown',this.key);
    root.addEventListener('touchstart',this.touchStart,{passive:true});root.addEventListener('touchend',this.touchEnd,{passive:true});
  }
  blocked(target) {
    return this.root.querySelector('dialog[open],.assistant-drawer:not([hidden])')||target?.closest?.('input,textarea,select,button,a,audio,video,[contenteditable=true],#nebula-navigation,.community-sidebar');
  }
  reset(){clearTimeout(this.timer);this.amount=0;this.progress(null);}
  step(delta,now,{explicit=false,source='wheel'}={}) {
    if(!Number.isFinite(delta)||!delta||!Number.isFinite(now)||this.disposed)return false;
    const pages=['tree','feed','rank'],index=pages.indexOf(this.current());if(index<0)return false;
    const down=delta>0,doc=this.root.documentElement;
    const edge=down?this.viewport.scrollY+this.viewport.innerHeight>=doc.scrollHeight-6:this.viewport.scrollY<=6;
    const gap=now-this.last;this.last=now;
    if(!edge){this.reset();this.arriving=true;return false;}
    const next=index+(down?1:-1);
    if(next<0||next>=pages.length){this.reset();return false;}
    if(this.pending||now<this.lockedUntil){this.reset();return true;}
    // After a transition or scrolling to the edge, let the old gesture settle.
    if(!explicit&&(this.waitForQuiet||this.arriving)&&gap<220){this.reset();return true;}
    this.waitForQuiet=false;this.arriving=false;
    if(!explicit){
      if(gap>650||Math.sign(delta)!==Math.sign(this.amount))this.amount=0;
      this.amount+=Math.sign(delta)*Math.min(Math.abs(delta),120);
      this.progress({target:pages[next],direction:down?1:-1,amount:Math.min(1,Math.abs(this.amount)/180)});
      clearTimeout(this.timer);this.timer=setTimeout(()=>this.reset(),650);this.timer.unref?.();
      if(Math.abs(this.amount)<180)return true;
    }
    this.reset();this.lockedUntil=now+420;this.waitForQuiet=true;
    const result=this.change(pages[next],{direction:down?1:-1,edge:down?'start':'end',source});
    if(result?.then){this.pending=true;Promise.resolve(result).catch(()=>{}).finally(()=>{this.pending=false;});}
    return true;
  }
  dispose(){this.disposed=true;this.reset();this.root.removeEventListener('wheel',this.wheel);this.root.removeEventListener('keydown',this.key);this.root.removeEventListener('touchstart',this.touchStart);this.root.removeEventListener('touchend',this.touchEnd);}
}
