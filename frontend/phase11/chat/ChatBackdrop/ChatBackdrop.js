import { CHAT_BACKDROP_ARTWORK } from './ChatBackdropArtwork.js?v=144f1e6009a3d461';
/** Shared workspace scenery. Only twenty CSS particles move. */
export function backdropMarkup(){
  const particles=Array.from({length:20},(_,i)=>{
    const kind=i%5===0?'leaf':i%3===0?'catkin':'petal';
    // Willow down stays around the middle; petals drift away from the left canopy.
    const x=kind==='catkin'?35+(i*7)%30:7+(i*13)%54;
    return `<i class="chat-tree-drift ${kind}" style="--x:${x}%;--y:${(i*29)%79}%;--delay:-${i*1.83}s;--duration:${19+i%7*3}s;--size:${i%3+5}px;--turn:${i*47}deg"></i>`;
  }).join('');
  return `<img class="chat-tree-landscape" src="${CHAT_BACKDROP_ARTWORK.uri}" width="${CHAT_BACKDROP_ARTWORK.width}" height="${CHAT_BACKDROP_ARTWORK.height}" loading="lazy" decoding="async" alt="">
    <div class="chat-tree-veil"></div><div class="chat-tree-particles">${particles}</div>`;
}

export function mountChatBackdrop(pane,{signal,doc=pane?.ownerDocument}={}){
  if(!pane||signal?.aborted)return()=>{};
  // The document owns one backdrop across its workspace panels.
  const existing=pane.querySelector('.chat-backdrop');if(existing)return()=>{};
  const art=doc.createElement('div');art.className='chat-backdrop';
  art.setAttribute('aria-hidden','true');art.setAttribute('inert','');
  art.innerHTML=backdropMarkup();pane.prepend(art);
  let visible=false,disposed=false;
  const sync=()=>art.classList.toggle('is-paused',doc.hidden||!visible);
  const Observer=doc.defaultView?.IntersectionObserver;
  const observer=Observer?new Observer(entries=>{visible=entries.some(e=>e.isIntersecting);sync();}):null;
  if(observer)observer.observe(art);else visible=true;
  doc.addEventListener('visibilitychange',sync);sync();
  const dispose=()=>{if(disposed)return;disposed=true;observer?.disconnect();doc.removeEventListener('visibilitychange',sync);signal?.removeEventListener('abort',dispose);art.remove();};
  signal?.addEventListener('abort',dispose,{once:true});return dispose;
}
