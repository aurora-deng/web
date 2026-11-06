import { TreePageNavigation } from '../TreePageNavigation/TreePageNavigation.js?v=144f1e6009a3d461';
import { TreePageTransition } from '../TreePageTransition/TreePageTransition.js?v=144f1e6009a3d461';
import { treeApi } from "../TreeApi/TreeApi.js?v=144f1e6009a3d461";
import { adminApi } from "../../admin/AdminApi/AdminApi.js?v=144f1e6009a3d461";
import { activitiesApi } from "../../activities/ActivitiesApi/ActivitiesApi.js?v=144f1e6009a3d461";
import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=144f1e6009a3d461';
import { openTreeAuthor } from '../../shared/TreeAuthorCard/TreeAuthorCard.js?v=144f1e6009a3d461';
import { ActivityPublisher } from '../../shared/ActivityPublisher/ActivityPublisher.js?v=144f1e6009a3d461';
mountNavigation('tree');
import { VoiceRecorder } from '../VoiceRecorder/VoiceRecorder.js?v=144f1e6009a3d461';
import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=144f1e6009a3d461';
import { AnnouncementPlayer } from '../../shared/AnnouncementPlayer/AnnouncementPlayer.js?v=144f1e6009a3d461';
import { formatShortDateTime, formatChatTime } from '../../shared/TimeFormat/TimeFormat.js?v=144f1e6009a3d461';

const randomId = () => globalThis.crypto?.randomUUID?.() || Array.from(crypto.getRandomValues(new Uint8Array(16)), n => n.toString(16).padStart(2,'0')).join('');
const $ = selector => document.querySelector(selector);
const api = new BusinessApi();
const state = { posts: [], ranks: [], rankedPosts: [], total: '0', before: '', previous: [], detail: null, replyBefore: '', categories: [], busy: false, recorded: null, since: '', replies: [], replyTarget: null, reportPost: null };
let scene, generation = 0, pendingReply, rankingPending;
const el = (tag, text, cls) => { const n = document.createElement(tag); if (text !== undefined) n.textContent = text; if (cls) n.className = cls; return n; };
const shortTime = value => formatShortDateTime(value);
const showError = (error, target = '#status') => { $(target).textContent = error.status === 401 ? '请先登录，再来树下分享你的故事。' : error.message; $(target).classList.add('error'); };
document.querySelectorAll('[data-close]').forEach(button => button.onclick = () => document.getElementById(button.dataset.close).close());
function author(post) {
  const head = el('header'); head.append(el('span', [...post.authorName][0] || '✿', 'avatar'));
  const copy = el('div', undefined, 'author-copy');const link=el('a',post.authorName);link.href='/phase11/profile/?user='+encodeURIComponent(post.authorId);copy.append(link, el('small', formatChatTime(post.createdAt))); head.append(copy); return head;
}
function tags(post) { const box = el('div', undefined, 'tags');
  const category = state.categories.find(c => c.id === post.categoryId); [category?.name, post.mood, ...post.tags].filter(Boolean).forEach(t => box.append(el('span', '# '+t))); return box;
}
function button(text, action) { const b = el('button', text); b.type = 'button'; b.onclick = () => Promise.resolve().then(action).catch(showError); return b; }
function updateScene(){const visible=new Map([...state.rankedPosts,...state.posts].map(p=>[p.id,p]));scene?.update([...visible.values()],Number(state.total),state.ranks);}
function render() {
  const list = $('#post-list'); list.replaceChildren();
  if (!state.posts.length) list.append(el('div', '这里还没有故事。来挂下第一枚果实吧。', 'empty-state'));
  state.posts.forEach(post => {
    const card = el('article', undefined, 'post-card');card.append(author(post),tags(post));
    const body = el('div',post.body || '有一段想与你分享的声音', 'post-body');body.tabIndex=0;body.setAttribute('role','button');body.onclick=()=>openPost(post.id);body.onkeydown=e=>{if(e.key==='Enter')openPost(post.id);};card.append(body);
    const image = post.media.find(m=>m.kind==='image');if(image){const img=el('img');img.src=image.url;img.alt='动态图片';img.loading='lazy';img.onclick=()=>openPost(post.id);card.append(img);}
    const footer=el('footer');footer.append(button(`${post.liked?'♥':'♡'} ${post.likes}`,()=>like(post)),button(`评论 ${post.repliers}`,()=>openPost(post.id)));card.append(footer);list.append(card);
  });
  $('#tree-count').textContent=`${Number(state.total).toLocaleString()} 枚果实正在生长`;
  $('#tree-height').textContent=state.total==='0'?'新生树苗':`树高 ${(Number(state.total)*.08).toLocaleString(undefined,{maximumFractionDigits:2})} m`;
  $('#previous-page').disabled=!state.previous.length;$('#next-page').disabled=state.posts.length<24;
  $('#page-info').textContent=`第 ${state.previous.length+1} 分区 · ${state.posts.length} 枚果实`;
  updateScene();
}
async function load() {
  const version=++generation;$('#status').textContent='正在树下寻找新的故事…';$('#status').classList.remove('error');
  const q=new URLSearchParams({limit:'24'});if(state.before)q.set('before',state.before);
  for(const [field,id] of [['category','category-filter'],['media','media-filter'],['mood','mood-filter'],['tag','tag-filter'],['author','author-filter']]){const value=$('#'+id)?.value.trim();if(value)q.set(field,value);}
  if(state.since)q.set('since',state.since);
  try { const data=await api.request(treeApi.getTreePosts({}, String(q)));if(version!==generation)return;state.posts=data.posts;state.total=data.total;render();$('#status').textContent=''; }
  catch(error){if(version===generation)showError(error);}
}
function ranking() {
  // 定时刷新、发布完成和重连可能同时触发；共享一次读取，避免重复扇出。
  if(rankingPending)return rankingPending;
  rankingPending=readRanking().finally(()=>{rankingPending=null;});return rankingPending;
}
async function readRanking() {
  const data=await api.request(treeApi.getTreeRanking({}));state.ranks=data.entries;
  // 即使榜单版本没变，也重新执行可见性过滤，删除/隐藏不会留在缓存中。
  const ids=data.entries.map(r=>r.postId);
  const ranked=ids.length?(await api.request(treeApi.getTreePosts({}, "limit=10&ids=" + String(ids.join(','))))).posts:[];
  state.rankedPosts=ranked;
  const list=$('#ranking');list.replaceChildren();
  if(!state.ranks.length)list.append(el('li','新的故事，等待第一束微光。'));
  for(const [index,rank] of state.ranks.entries()){
    const row=el('li'),b=button('',()=>openPost(rank.postId));b.append(el('strong',String(index+1).padStart(2,'0')),el('span',`第 ${rank.postId} 枚果实`));row.append(b);list.append(row);
  }
  $('#rank-time').textContent=data.settledAt?`${shortTime(data.settledAt)} 更新 · 每30秒结算`:'每30秒结算一次';updateScene();
}
async function like(post) { const value=await api.request(treeApi.postTreePostsPostLike({"post":(post.id)}),{method:'POST',data:{liked:String(!post.liked)}});const result={...value,favorited:post.favorited};state.posts=state.posts.map(p=>p.id===post.id?result:p);state.rankedPosts=state.rankedPosts.map(p=>p.id===post.id?result:p);if(state.detail?.id===post.id){state.detail={...state.detail,...result};renderDetail();}render(); }
async function openPost(id) {
  try { const p=await api.request(activitiesApi.getActivitiesPost({"post":(id)}));state.detail=p;state.replyBefore='';state.replies=[];state.replyTarget=null;pendingReply=null;setReplyTarget(null);renderDetail();$('#replies').replaceChildren();$('#detail-status').textContent='';$('#post-dialog').showModal();await replies(); }
  catch(error){showError(error);}
}
function renderDetail() {
  const p=state.detail,detail=$('#post-detail');detail.replaceChildren(author(p),tags(p),el('p',p.body,'post-body'));const cardButton=button('查看作者 · 好友申请',()=>openTreeAuthor(api,p));cardButton.className='author-card-entry';detail.querySelector('header').append(cardButton);
  const media=el('div',undefined,'detail-media');for(const m of p.media){const node=el(m.kind==='image'?'img':'audio');node.src=m.url;if(m.kind==='image'){node.alt='果实图片';node.loading='lazy';node.onclick=()=>{const dialog=$('#image-dialog');dialog.querySelector('img').src=m.url;dialog.showModal();};}else{node.controls=true;node.preload='none';}media.append(node);}detail.append(media);
  const actions=el('div',undefined,'detail-actions');actions.append(button(`${p.liked?'♥':'♡'} ${p.likes}`,()=>like(p)),button('举报',()=>{state.reportPost=p.id;$('#report-form').reset();$('#report-status').textContent='';$('#report-dialog').showModal();}));
  actions.append(button(p.favorited?'★ 已收藏':'☆ 收藏',async()=>{state.detail=await api.request(activitiesApi.postActivitiesPostFavorite({"post":(p.id)}),{method:'POST',data:{enabled:String(!p.favorited)}});renderDetail();}),button('分享',async()=>{const result=await api.request(activitiesApi.getActivitiesPostShare({"post":(p.id)}));const url=new URL(result.path,location.origin).href;try{await navigator.clipboard.writeText(url);$('#detail-status').textContent='链接已复制，访问者仍须有权限。';}catch{$('#detail-status').textContent=url;}}));
  if(p.capabilities?.delete)actions.append(button('删除果实',async()=>{const reason=await confirmDelete(p.capabilities.deleteReasonRequired);if(reason===null)return;await api.request(treeApi.deleteTreePostsPost({"post":(p.id)}),{method:'DELETE',data:{reason}});$('#post-dialog').close();await load();}));detail.append(actions);
}
function setReplyTarget(reply){
  state.replyTarget=reply;pendingReply=null;$('#reply-target').hidden=!reply;
  $('#reply-target span').textContent=reply?`回复 @${reply.authorName}`:'';
  $('#reply-body').placeholder=reply?`回复 @${reply.authorName}…`:'友善交流，让故事多一份温度…';
  if(reply)$('#reply-body').focus();
}
$('#cancel-reply').onclick=()=>setReplyTarget(null);
function paintReplies(){
  const list=$('#replies');list.replaceChildren();$('#reply-count').textContent=String(state.detail?.repliers||state.replies.length);
  if(!state.replies.length){list.append(el('p','还没有评论。来留下第一份温柔的回应吧。','comments-empty'));return;}
  for(const r of state.replies){
    const row=el('article',undefined,'reply'+(r.parentReplyId&&r.parentReplyId!=='0'?' nested':''));row.dataset.replyId=r.id;
    const avatar=el('span',[...r.authorName][0]||'友','comment-avatar'),copy=el('div');
    const name=el('div',r.authorName,'reply-name');if(String(r.authorId)===String(state.detail.authorId))name.append(el('span','作者','author-badge'));copy.append(name);
    if(r.parentReplyId&&r.parentReplyId!=='0')copy.append(el('div',`回复 @${r.replyToName||'树下的朋友'}`,'reply-address'));
    copy.append(el('p',r.body));const tools=el('div',undefined,'reply-tools');tools.append(el('time',formatChatTime(r.createdAt)),button('回复',()=>setReplyTarget(r)));
    if(String(r.authorId)===String(api.user.id))tools.append(button('删除',async()=>{await api.request(treeApi.deleteTreeRepliesReply({"reply":(r.id)}),{method:'DELETE'});state.replies=state.replies.filter(x=>x.id!==r.id);paintReplies();}));
    copy.append(tools);row.append(avatar,copy);list.append(row);
  }
}
async function replies() {
  const id=state.detail.id,rows=await api.request(treeApi.getTreePostsPostReplies({"post":(id)}, "before=" + String(state.replyBefore||'0')));
  if(state.detail.id!==id)return;
  const seen=new Set(state.replies.map(r=>r.id));state.replies.push(...rows.filter(r=>!seen.has(r.id)));paintReplies();
  state.replyBefore=rows.at(-1)?.id||state.replyBefore;$('#older-replies').hidden=rows.length<30;
}
async function confirmDelete(reasonRequired=false){
  const dialog=$('#confirm-dialog');let reason=dialog.querySelector('textarea');if(!reason){reason=el('textarea');reason.maxLength=200;reason.rows=3;reason.placeholder='管理员删除原因（必填，将记录审计）';dialog.querySelector('footer').before(reason);}reason.hidden=!reasonRequired;reason.value='';
  return new Promise(resolve=>{let accepted=null;$('#confirm-delete').onclick=()=>{if(reasonRequired&&!reason.value.trim()){reason.focus();reason.setCustomValidity('请填写删除原因');reason.reportValidity();return;}reason.setCustomValidity('');accepted=reason.value.trim();dialog.close();};reason.oninput=()=>reason.setCustomValidity('');dialog.addEventListener('close',()=>resolve(accepted),{once:true});dialog.showModal();});
}
$('#report-form').onsubmit=async event=>{event.preventDefault();const form=event.currentTarget,button=form.querySelector('button[type=submit]'),fields=new FormData(form),kind=String(fields.get('reason')||''),detail=String(fields.get('detail')||'').trim();if(!kind)return;
  button.disabled=true;try{await api.request(treeApi.postTreePostsPostReports({"post":(state.reportPost)}),{method:'POST',data:{reason:kind+(detail?'：'+detail:'')}});$('#report-dialog').close();$('#detail-status').textContent='举报已提交，管理员会核实处理。';}catch(e){showError(e,'#report-status');}finally{button.disabled=false;}};
$('#older-replies').onclick=()=>replies().catch(e=>showError(e,'#detail-status'));
$('#reply-form').onsubmit=async event=>{event.preventDefault();const form=event.currentTarget,body=$('#reply-body').value.trim();if(!body)return;
  const id=state.detail.id;const parentReplyId=state.replyTarget?.id||'0';pendingReply=pendingReply?.body===body&&pendingReply.parentReplyId===parentReplyId?pendingReply:{requestId:randomId(),body,parentReplyId};const submit=form.querySelector('button[type=submit]');submit.disabled=true;
  try{await api.request(treeApi.postTreePostsPostReplies({"post":(id)}),{method:'POST',data:pendingReply});pendingReply=null;form.reset();setReplyTarget(null);state.replyBefore='';state.replies=[];$('#replies').replaceChildren();await replies();await load();}
  catch(error){showError(error,'#detail-status');}finally{submit.disabled=false;}
};
$('#show-publish').onclick=()=>{if(!api.user)return location.assign('/phase11/');$('#publish-dialog').showModal();};
new ActivityPublisher($('#publish-form'),api,{initialDestination:'tree',files:()=>[...$('#media-files').files,...(state.recorded?[state.recorded]:[])],status:$('#publish-status'),onPublished:async()=>{state.recorded=null;$('#record-state').textContent='';$('#publish-dialog').close();state.before='';state.previous=[];await load();await ranking();}});
const recorder = new VoiceRecorder($('#record-voice'),$('#record-state'),file=>{state.recorded=file;});
$('#publish-dialog').addEventListener('close',()=>recorder.stop());
function applyView(view){
  if(!['tree','feed','rank'].includes(view))view='tree';document.body.dataset.treeView=view;document.body.classList.remove('list-mode');
  const copy={tree:['一树花开，万种相逢','转动视角，轻触一枚果实，听见藏在花间的故事。'],feed:['树下的故事','分享生活的片刻，也认真回应每一次相遇。'],rank:['七日心动榜','过去七天里，那些被大家温柔点亮的故事。']}[view];
  $('#view-title').textContent=copy[0];$('#view-description').textContent=copy[1];
  document.querySelectorAll('[data-tree-tab]').forEach(button=>button.classList.toggle('active',button.dataset.treeTab===view));
  scene?.setVisible(view==='tree');
  if(view==='tree')requestAnimationFrame(()=>scene?.resize());history.replaceState(null,'','#'+view);
}
const pageTransition=new TreePageTransition({surface:$('.community-main'),current:()=>document.body.dataset.treeView,apply:applyView});
$('#view-title').tabIndex=-1;
function switchView(view,options){return pageTransition.navigate(view,options);}
document.querySelectorAll('[data-tree-tab]').forEach(button=>button.onclick=()=>switchView(button.dataset.treeTab));
function firstPage(){state.before='';state.previous=[];const hours=Number($('#time-filter')?.value||0);state.since=hours?String(Date.now()-hours*3600000):'';return load();}
$('#apply-filter').onclick=()=>{switchView('feed');$('#filter-dialog').close();firstPage();};$('#open-filter').onclick=()=>$('#filter-dialog').showModal();$('#refresh').onclick=()=>load();
$('#next-page').onclick=()=>{state.previous.push(state.before);state.before=state.posts.at(-1)?.id||'';load();};
$('#previous-page').onclick=()=>{state.before=state.previous.pop()||'';load();};
$('#view-toggle').onclick=()=>{const list=document.body.classList.toggle('list-mode');$('#view-toggle').textContent=list?'✧ 三维模式':'☷ 列表模式';$('#view-toggle').setAttribute('aria-pressed',String(list));scene?.setVisible(!list);if(!list)scene?.resize();};
$('#scene-reset').onclick=()=>scene?.reset();
$('#scene-zoom-in').onclick=()=>scene?.zoom(.84);$('#scene-zoom-out').onclick=()=>scene?.zoom(1.18);
const pageNavigation=new TreePageNavigation({current:()=>document.body.dataset.treeView,change:switchView,progress:value=>pageTransition.progress(value)});
addEventListener('pagehide',()=>{pageNavigation.dispose();pageTransition.dispose();},{once:true});
async function start(){
  try{const user=await api.restore();$('#identity').textContent=user.displayName;
    const [role,categories]=await Promise.all([api.request(adminApi.getAdminMe({})),api.request(treeApi.getTreeCategories({}))]);$('#admin-link').hidden=role.role==='user';
    state.categories=categories;
    for(const category of state.categories){const b=button(category.name,()=>{document.querySelector('#category-filter').value=category.id;document.querySelectorAll('[data-category]').forEach(n=>n.classList.toggle('active',n.dataset.category===category.id));return firstPage();});b.dataset.category=category.id;$('#category-tabs').append(b);}
    $('#category-tabs [data-category=""]').onclick=()=>{document.querySelector('#category-filter').value='';document.querySelectorAll('[data-category]').forEach(n=>n.classList.toggle('active',n.dataset.category===''));firstPage();};
    for(const category of state.categories){for(const selector of ['#category-filter','#publish-category']){const option=el('option',category.name);option.value=category.id;$(selector).append(option);}}
    applyView(location.hash.slice(1)||'tree');
    // 网络读取与3D模块加载并行，树形无需等待榜单的全部数据才能出现。
    const sceneReady=import('../TreeScene/TreeScene.js?v=144f1e6009a3d461').then(({TreeScene})=>{scene=new TreeScene($('#scene'),openPost);scene.setVisible(document.body.dataset.treeView==='tree');updateScene();}).catch(()=>{document.body.classList.add('list-mode');$('#view-toggle').disabled=true;$('#status').textContent='此设备无法使用三维场景，已切换完整列表模式。';});
    await Promise.all([load(),ranking(),sceneReady]);
    const selectedPost=new URL(location.href).searchParams.get('post');if(selectedPost)openPost(selectedPost);
    new AnnouncementPlayer(api,$('#announcements'),{connectEvents:true}).start();
    const timer=setInterval(()=>{if(!document.hidden)ranking().catch(()=>{});},30000);
    addEventListener('pagehide',()=>{clearInterval(timer);scene?.dispose();},{once:true});
  }catch(error){$('#identity').textContent='暂未登录';showError(error);$('#ranking').replaceChildren(el('li','登录后，和大家在树下相遇。'));}
}
start();
