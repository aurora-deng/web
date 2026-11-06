import { treeApi } from "../../tree/TreeApi/TreeApi.js?v=144f1e6009a3d461";
import { activitiesApi } from "../../activities/ActivitiesApi/ActivitiesApi.js?v=144f1e6009a3d461";
import { openTreeAuthor } from '../TreeAuthorCard/TreeAuthorCard.js?v=144f1e6009a3d461';
const node=(tag,text,className)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(className)n.className=className;return n;};
const key=()=>Array.from(crypto.getRandomValues(new Uint8Array(16)),n=>n.toString(16).padStart(2,'0')).join('');
export class ActivityDetail {
  constructor(api,onChanged=()=>{}) {
    this.api=api;this.onChanged=onChanged;this.dialog=node('dialog',undefined,'activity-detail');
    this.dialog.innerHTML='<header><h2>这一刻的故事</h2><button type="button" aria-label="关闭动态">×</button></header><article></article><section class="activity-comments"><h3>回应</h3><div data-comments></div><button type="button" data-older>更多回应</button></section><form><label>留下回应<textarea name="body" required maxlength="1000" rows="2" placeholder="友善交流，认真回应…"></textarea></label><small data-reply-target></small><button type="submit">发布回应</button></form><p role="status"></p>';
    this.dialog.querySelector('header button').onclick=()=>this.dialog.close();
    this.dialog.addEventListener('click',e=>{if(e.target===this.dialog)this.dialog.close();});
    this.dialog.querySelector('[data-older]').onclick=()=>this.comments(true).catch(e=>this.status(e.message));
    this.dialog.querySelector('form').onsubmit=e=>{e.preventDefault();this.reply(e.currentTarget);};document.body.append(this.dialog);
  }
  status(text){this.dialog.querySelector('[role=status]').textContent=text;}
  action(text,fn){const b=node('button',text);b.type='button';b.onclick=()=>Promise.resolve().then(fn).catch(e=>this.status(e.message));return b;}
  async open(id) {
    this.post=await this.api.request(activitiesApi.getActivitiesPost({"post":(id)}));this.rows=[];this.parent='0';this.before='0';this.status('');this.paint();this.dialog.showModal();await this.comments();
  }
  paint() {
    const p=this.post,article=this.dialog.querySelector('article');article.replaceChildren();
    const author=node('a',p.authorName,'activity-author');author.href='/phase11/profile/?user='+encodeURIComponent(p.authorId);
    article.append(author,node('time',new Date(Number(p.createdAt)).toLocaleString()),node('p',p.body,'activity-body'));
    const media=node('div',undefined,'activity-media');for(const m of p.media||[]) {
      const item=node(m.kind==='image'?'img':'audio');item.src=m.url;
      if(m.kind==='image'){item.alt='动态图片';item.loading='lazy';}else{item.controls=true;item.preload='none';}media.append(item);
    }article.append(media);if(p.treeChannel)article.append(this.action('作者资料与好友申请',()=>openTreeAuthor(this.api,p)));
    const actions=node('div',undefined,'activity-actions');
    actions.append(this.action(`${p.liked?'♥':'♡'} ${p.likes}`,async()=>{await this.api.request(treeApi.postTreePostsPostLike({"post":(p.id)}),{method:'POST',data:{liked:String(!p.liked)}});await this.refresh();}),
      this.action(p.favorited?'★ 已收藏':'☆ 收藏',async()=>{this.post=await this.api.request(activitiesApi.postActivitiesPostFavorite({"post":(p.id)}),{method:'POST',data:{enabled:String(!p.favorited)}});this.paint();this.onChanged();}),
      this.action('分享',async()=>{const data=await this.api.request(activitiesApi.getActivitiesPostShare({"post":(p.id)}));const url=new URL(data.path,location.origin).href;
        if(navigator.clipboard?.writeText){try{await navigator.clipboard.writeText(url);this.status('链接已复制；访问者仍须登录并有权限。');return;}catch{}}
        this.status(url+'（访问者仍须有权限）');}));
    if(p.capabilities?.delete)actions.append(this.action('删除动态',async()=>{
      let reason='';if(p.capabilities.deleteReasonRequired){reason=prompt('站点管理员删除原因（必填，最多 200 字）');if(reason===null||!reason.trim())return;}
      if(!confirm('删除这条动态？正文和媒体将从所有展示入口消失。'))return;
      await this.api.request(treeApi.deleteTreePostsPost({"post":(p.id)}),{method:'DELETE',data:{reason}});this.dialog.close();await this.onChanged();
    }));article.append(actions);
  }
  async refresh(){this.post=await this.api.request(activitiesApi.getActivitiesPost({"post":(this.post.id)}));this.paint();await this.onChanged();}
  async comments(older=false) {
    const id=this.post.id;const rows=await this.api.request(treeApi.getTreePostsPostReplies({"post":(id)}, "before=" + String(older?this.before:'0')));
    if(id!==this.post.id)return;if(!older)this.rows=[];
    const seen=new Set(this.rows.map(r=>r.id));this.rows.push(...rows.filter(r=>!seen.has(r.id)));this.before=rows.at(-1)?.id||this.before;
    const box=this.dialog.querySelector('[data-comments]');box.replaceChildren();
    for(const r of this.rows){const row=node('div',undefined,'activity-comment');row.append(node('strong',r.authorName),node('p',r.parentReplyId&&r.parentReplyId!=='0'?`回复 @${r.replyToName}：${r.body}`:r.body),
      this.action('回复',()=>{this.parent=r.id;this.dialog.querySelector('[data-reply-target]').textContent='回复 @'+r.authorName;this.dialog.querySelector('textarea').focus();}));box.append(row);}
    if(!this.rows.length)box.append(node('p','还没有回应，来说点什么吧。','activity-empty'));
    this.dialog.querySelector('[data-older]').hidden=rows.length<30;
  }
  async reply(form) {
    const body=form.elements.body.value.trim();if(!body||this.sending)return;const button=form.querySelector('button[type=submit]');this.sending=true;button.disabled=true;
    const payload={body,parentReplyId:this.parent||'0'};const signature=JSON.stringify(payload);
    if(this.replySignature!==signature){this.replySignature=signature;this.replyId=key();}
    try{await this.api.request(treeApi.postTreePostsPostReplies({"post":(this.post.id)}),{method:'POST',data:{...payload,requestId:this.replyId}});
      form.reset();this.parent='0';this.dialog.querySelector('[data-reply-target]').textContent='';this.replySignature='';await this.comments();await this.refresh();this.status('回应已发布');
    }catch(e){this.status(e.message);}finally{this.sending=false;button.disabled=false;}
  }
}
