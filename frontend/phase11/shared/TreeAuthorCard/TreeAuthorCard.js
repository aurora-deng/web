const node=(tag,text)=>{const n=document.createElement(tag);if(text)n.textContent=text;return n;};
/** 果实只提供作者入口；是否允许申请、是否已是好友都由服务端返回。 */
export async function openTreeAuthor(api,post){
  const author=await api.request(`/api/tree/posts/${post.id}/author`);
  if(!document.querySelector('[data-author-card-css]')){const css=node('link');css.rel='stylesheet';css.href='/phase11/shared/TreeAuthorCard/TreeAuthorCard.css?v=227af12a6c379d08';css.dataset.authorCardCss='';document.head.append(css);}
  const dialog=node('dialog');dialog.className='tree-author-card';
  const close=node('button','×');close.type='button';close.className='author-close';close.setAttribute('aria-label','关闭作者卡');close.onclick=()=>dialog.close();
  const avatar=node('div',[...author.displayName][0]||'友');avatar.className='author-avatar';
  if(author.avatarUrl){const img=node('img');img.src=author.avatarUrl;img.alt='作者头像';avatar.replaceChildren(img);}
  const link=node('a',author.self?'我的主页':author.friendship?'查看好友动态':'查看公开树上故事');link.href='/phase11/profile/?user='+encodeURIComponent(author.id);
  const hint=node('p',author.self?'你可以在个人主页设置是否允许通过果实加好友。':author.friendship?'你们已经是好友。仅自己可见的内容仍然不会公开。':'非好友只能查看作者公开到神树的故事，不开放完整个人动态。');
  const request=node('button',author.self?'这是你自己的果实':author.friendship?'已是好友':author.pendingRequest?'已有待处理申请':author.canRequestFriendship?'申请加为好友':'作者未开放树上好友申请');request.type='button';request.disabled=!author.canRequestFriendship;
  const status=node('p');status.setAttribute('role','status');
  request.onclick=async()=>{request.disabled=true;try{await api.request(`/api/tree/posts/${post.id}/friend-request`,{method:'POST',data:{}});request.textContent='申请已发送';status.textContent='请等待对方接受，尚未成为好友。';}catch(e){status.textContent=e.message;request.disabled=false;}};
  dialog.append(close,avatar,node('h2',author.displayName),node('small','@'+author.username),node('p',author.biography||'把故事留在花开处。'),hint,link,request,status);
  dialog.addEventListener('close',()=>dialog.remove(),{once:true});dialog.addEventListener('click',e=>{if(e.target===dialog)dialog.close();});document.body.append(dialog);dialog.showModal();
}
