/** A committed ACK can arrive before (or without) the Outbox broadcast.
 * Only the exact pending operation and persisted message may become terminal.
 * Do not derive a successful recall from a button click or a missing reply.
 */
export function matchesRecallReceipt(receipt,pending){
  return Boolean(pending&&receipt?.type==='chat.recall.accepted'&&
    ['recalled','already_recalled'].includes(receipt.status)&&
    String(receipt.replyTo)===String(pending.commandId)&&
    String(receipt.id)===String(pending.id)&&
    String(receipt.conversationId)===String(pending.conversationId)&&
    String(receipt.sequence)===String(pending.sequence));
}

export function recalledFromReceipt(receipt,pending,existing){
  if(!matchesRecallReceipt(receipt,pending)||!existing)return null;
  if(String(existing.id)!==String(pending.id)||
     String(existing.conversationId)!==String(pending.conversationId)||
     String(existing.sequence)!==String(pending.sequence))return null;
  return {...existing,state:'recalled',body:'',attachments:[]};
}

/** One timer for the bounded visible DOM, not one timer per stored message.
 * Removing an expired action leaves the persisted message and its receipts intact.
 */
export function watchRecallExpiry(list,{signal,Observer=globalThis.MutationObserver,clock=Date.now,timers=globalThis}={}){
  if(!list||!Observer)return()=>{};
  let timer,disposed=false;
  const update=()=>{
    timers.clearTimeout(timer);if(disposed)return;
    const now=clock();let next=Infinity;
    for(const button of list.querySelectorAll('.message-recall[data-expires-at]')){
      const deadline=Number(button.dataset.expiresAt);
      if(!Number.isFinite(deadline)||now>deadline)button.remove();else next=Math.min(next,deadline+1-now);
    }
    if(Number.isFinite(next))timer=timers.setTimeout(update,Math.min(next,2147483647));
  };
  const observer=new Observer(update);observer.observe(list,{childList:true,subtree:true});
  const dispose=()=>{disposed=true;timers.clearTimeout(timer);observer.disconnect();signal?.removeEventListener('abort',dispose);};
  signal?.addEventListener('abort',dispose,{once:true});if(signal?.aborted)dispose();else update();
  return dispose;
}
