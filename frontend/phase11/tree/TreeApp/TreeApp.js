import { TreePageNavigation } from '../TreePageNavigation/TreePageNavigation.js?v=1f08ad808b467825';
import { TreePageTransition } from '../TreePageTransition/TreePageTransition.js?v=1f08ad808b467825';
import { treeApi } from "../TreeApi/TreeApi.js?v=1f08ad808b467825";
import { activitiesApi } from "../../activities/ActivitiesApi/ActivitiesApi.js?v=1f08ad808b467825";
import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=1f08ad808b467825';
import { openTreeAuthor } from '../../shared/TreeAuthorCard/TreeAuthorCard.js?v=1f08ad808b467825';
import { ActivityPublisher } from '../../shared/ActivityPublisher/ActivityPublisher.js?v=1f08ad808b467825';
import { CategoryRanking } from '../CategoryRanking/CategoryRanking.js?v=1f08ad808b467825';
import { trainingConsentControl } from '../TrainingConsent/TrainingConsent.js?v=1f08ad808b467825';
import { paintDiscussion } from '../../shared/DiscussionThreads/DiscussionThreads.js?v=1f08ad808b467825';
import { storyText } from '../StoryPreview/StoryPreview.js?v=1f08ad808b467825';
import { recordPublicCategoryView } from '../../shared/PersonalAgent/PersonalAgent.js?v=1f08ad808b467825';
import { preferenceSite } from '../../shared/PersonalAgent/PreferenceStore.js?v=1f08ad808b467825';
import { companionSession } from '../../shared/ClientBootstrap/ClientBootstrap.js?v=1f08ad808b467825';
mountNavigation('tree');
import { VoiceRecorder } from '../VoiceRecorder/VoiceRecorder.js?v=1f08ad808b467825';
import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=1f08ad808b467825';
import { AnnouncementPlayer } from '../../shared/AnnouncementPlayer/AnnouncementPlayer.js?v=1f08ad808b467825';
import { formatChatTime } from '../../shared/TimeFormat/TimeFormat.js?v=1f08ad808b467825';

const randomId = () => globalThis.crypto?.randomUUID?.() || Array.from(crypto.getRandomValues(new Uint8Array(16)), n => n.toString(16).padStart(2,'0')).join('');
const $ = selector => document.querySelector(selector);
const api = new BusinessApi();
const discussionStyle = document.createElement('link');
discussionStyle.rel = 'stylesheet';
discussionStyle.href = '/phase11/shared/DiscussionThreads/DiscussionThreads.css?v=1f08ad808b467825';
document.head.append(discussionStyle);
const state = { posts: [], ranks: [], rankedPosts: [], total: '0', before: '', previous: [], detail: null,
  replyBefore: '', categories: [], recorded: null, since: '', keyword: '', replies: [], authorOnly: false, replyTarget: null, reportPost: null };
let scene, sceneLoading, generation = 0, detailGeneration = 0, replyGeneration = 0, pendingReply, replyRequest;
const el = (tag, text, cls) => { const n = document.createElement(tag); if (text !== undefined) n.textContent = text; if (cls) n.className = cls; return n; };
const rankingView = new CategoryRanking(api, {
  select: $('#rank-category'), list: $('#ranking'), updated: $('#rank-time'),
  onSnapshot(ranks, posts) { state.ranks = ranks; state.rankedPosts = posts; updateScene(); },
  onOpen: openPost,
});
const showError = (error, target = '#status') => { $(target).textContent = error.status === 401 ? '请先登录，再来树下分享你的故事。' : error.message; $(target).classList.add('error'); };
document.querySelectorAll('[data-close]').forEach(button => button.onclick = () => document.getElementById(button.dataset.close).close());
function author(post) {
  const head = el('header'); head.append(el('span', [...post.authorName][0] || '✿', 'avatar'));
  const copy = el('div', undefined, 'author-copy');const link=el('a',post.authorName);link.href='/phase11/profile/?user='+encodeURIComponent(post.authorId);copy.append(link, el('small', formatChatTime(post.createdAt))); head.append(copy); return head;
}
function tags(post, selectable = false) {
  const box = el('div', undefined, 'tags');
  const category = state.categories.find(c => c.id === post.categoryId);
  if (category) box.append(el('span', category.name));
  if (post.mood) box.append(el('span', post.mood));
  for (const tag of (post.tags || [])) {
    if (selectable && post.bundledExample && tag === '站内示例') continue;
    if (selectable) box.append(button('# '+tag, () => { $('#tag-filter').value=tag; return firstPage(); }));
    else box.append(el('span', '# '+tag));
  }
  return box;
}
function button(text, action) { const b = el('button', text); b.type = 'button'; b.onclick = () => Promise.resolve().then(action).catch(showError); return b; }
function updateScene(){
  const posts = rankingView.category ? state.rankedPosts : [...state.rankedPosts, ...state.posts];
  const visible = new Map(posts.map(post=>[post.id,post]));
  scene?.update([...visible.values()],Number(state.total),state.ranks);
}
function ensureScene(){
  sceneLoading??=import('../TreeScene/TreeScene.js?v=1f08ad808b467825').then(({TreeScene})=>{
    scene=new TreeScene($('#scene'),openPost);
    scene.setVisible(document.body.dataset.treeView==='tree');updateScene();
  }).catch(()=>{
    $('#view-toggle').disabled=true;
    if(document.body.dataset.treeView==='tree')document.body.classList.add('list-mode');
  });
  return sceneLoading.then(()=>{
    if(!scene&&document.body.dataset.treeView==='tree')document.body.classList.add('list-mode');
  });
}
function render() {
  const list = $('#post-list'); list.replaceChildren();
  const filters = [state.keyword && `“${state.keyword}”`, $('#tag-filter').value && `# ${$('#tag-filter').value}`].filter(Boolean);
  $('#story-filter-summary').textContent = filters.join(' · ');
  if (!state.posts.length) list.append(el('div', filters.length ? '没有符合条件的故事，试试其他关键词或筛选。' : '这里还没有故事。', 'empty-state'));
  state.posts.forEach(post => {
    const card = el('article', undefined, 'post-card');
    const story = storyText(post);
    card.dataset.storyId=post.id;
    card.classList.toggle('story-text-card',!story.image);
    if (story.image) {
      const cover = button('', () => openPost(post.id));
      cover.className = 'story-cover';
      cover.setAttribute('aria-label', `查看 ${story.title}`);
      const img = el('img'); img.src = story.image.url; img.alt = ''; img.loading = 'lazy'; img.decoding='async';
      cover.append(img); card.append(cover);
    }
    const content=el('div',undefined,'story-card-content');
    const markers=el('div',undefined,'story-markers');
    if(story.example){
      const sampleFilter=button('站内示例',()=>{$('#tag-filter').value='站内示例';return firstPage();});
      sampleFilter.className='story-example';markers.append(sampleFilter);
    }
    if(story.voice)markers.append(el('span','语音','story-voice'));
    if(markers.childElementCount)content.append(markers);
    const body = button(story.title, () => openPost(post.id));
    body.className = 'story-title-button';
    const heading=el('h2');heading.append(body);content.append(heading);
    if(story.excerpt)content.append(el('p',story.excerpt,'story-excerpt'));
    content.append(tags(post,true),author(post));
    const footer=el('footer');
    const heart=button(`${post.liked?'♥':'♡'} ${post.likes}`,()=>like(post));
    heart.setAttribute('aria-label','点赞');heart.setAttribute('aria-pressed',String(post.liked));
    const saved=button(post.favorited?'已收藏':'收藏',()=>favorite(post));saved.setAttribute('aria-pressed',String(Boolean(post.favorited)));
    footer.append(heart,button(`${post.repliers} 人讨论`,()=>openPost(post.id)),saved);
    content.append(footer);card.append(content);list.append(card);
  });
  $('#tree-count').textContent=`${Number(state.total).toLocaleString()} 枚果实正在生长`;
  $('#tree-height').textContent=state.total==='0'?'新生树苗':`树高 ${(Number(state.total)*.08).toLocaleString(undefined,{maximumFractionDigits:2})} m`;
  $('#previous-page').disabled=!state.previous.length;$('#next-page').disabled=state.posts.length<24;
  $('#page-info').textContent=`第 ${state.previous.length+1} 分区 · ${state.posts.length} 枚果实`;
  updateScene();
}
async function load() {
  const version=++generation;$('#status').textContent='正在树下寻找新的故事…';$('#status').classList.remove('error');
  $('#post-list').setAttribute('aria-busy','true');$('#previous-page').disabled=true;$('#next-page').disabled=true;
  const q=new URLSearchParams({limit:'24'});if(state.before)q.set('before',state.before);
  for(const [field,id] of [['category','category-filter'],['media','media-filter'],['mood','mood-filter'],['tag','tag-filter'],['author','author-filter']]){const value=$('#'+id)?.value.trim();if(value)q.set(field,value);}
  if(state.since)q.set('since',state.since);
  if(state.keyword)q.set('q',state.keyword);
  try { const data=await api.request(treeApi.getTreePosts({}, String(q)));if(version!==generation)return;state.posts=data.posts;state.total=data.total;render();$('#status').textContent=''; }
  catch(error){if(version===generation)showError(error);}
  finally{if(version===generation)$('#post-list').setAttribute('aria-busy','false');}
}
function ranking() { return rankingView.refresh().catch(showError); }
async function like(post) {
  const value=await api.request(treeApi.postTreePostsPostLike({post:post.id}),{method:'POST',data:{liked:String(!post.liked)}});
  const updated={liked:value.liked,likes:value.likes,version:value.version};
  state.posts=state.posts.map(item=>item.id===post.id?{...item,...updated}:item);
  state.rankedPosts=state.rankedPosts.map(item=>item.id===post.id?{...item,...updated}:item);
  if(state.detail?.id===post.id){state.detail={...state.detail,...updated};renderDetail();}
  render();
}
async function favorite(post) {
  const value=await api.request(activitiesApi.postActivitiesPostFavorite({post:post.id}),{method:'POST',data:{enabled:String(!post.favorited)}});
  state.posts=state.posts.map(item=>item.id===post.id?{...item,favorited:value.favorited}:item);
  if(state.detail?.id===post.id){state.detail={...state.detail,favorited:value.favorited};renderDetail();}
  render();
}
async function openPost(id) {
  const version=++detailGeneration;
  try {
    const post=await api.request(activitiesApi.getActivitiesPost({post:id}));
    if(version!==detailGeneration)return;
    state.detail=post;state.authorOnly=false;setReplyTarget(null);
    if (post.treeChannel && post.visibility === 'members') {
      const category = state.categories.find(item => item.id === post.categoryId);
      const account = api.user?.id;
      if (category && account) companionSession().then(session =>
        recordPublicCategoryView(account, category.id, category.name, post.id, preferenceSite(session))).catch(() => {});
    }
    renderDetail();$('#detail-status').textContent='';
    if(!$('#post-dialog').open)$('#post-dialog').showModal();
    await resetReplies();
  } catch(error){if(version===detailGeneration)showError(error);}
}
$('#post-dialog').addEventListener('close',()=>{++detailGeneration;++replyGeneration;state.detail=null;});
function renderDetail() {
  const p=state.detail,detail=$('#post-detail');detail.replaceChildren(author(p),tags(p),el('p',p.body,'post-body'));const cardButton=button('查看作者 · 好友申请',()=>openTreeAuthor(api,p));cardButton.className='author-card-entry';detail.querySelector('header').append(cardButton);
  const media=el('div',undefined,'detail-media');for(const m of p.media){const node=el(m.kind==='image'?'img':'audio');node.src=m.url;if(m.kind==='image'){node.alt='果实图片';node.loading='lazy';node.onclick=()=>{const dialog=$('#image-dialog');dialog.querySelector('img').src=m.url;dialog.showModal();};}else{node.controls=true;node.preload='none';}media.append(node);}detail.append(media);
  const actions=el('div',undefined,'detail-actions');actions.append(button(`${p.liked?'♥':'♡'} ${p.likes}`,()=>like(p)),button('举报',()=>{state.reportPost=p.id;$('#report-form').reset();$('#report-status').textContent='';$('#report-dialog').showModal();}));
  actions.append(button(p.favorited?'★ 已收藏':'☆ 收藏',()=>favorite(p)),button('分享',async()=>{const result=await api.request(activitiesApi.getActivitiesPostShare({"post":(p.id)}));const url=new URL(result.path,location.origin).href;try{await navigator.clipboard.writeText(url);$('#detail-status').textContent='链接已复制';}catch{$('#detail-status').textContent=url;}}));
  const training = trainingConsentControl(api, p, text => { $('#detail-status').textContent = text; });
  if (training) actions.append(training);
  if(p.capabilities?.delete)actions.append(button('删除果实',async()=>{const reason=await confirmDelete(p.capabilities.deleteReasonRequired);if(reason===null)return;await api.request(treeApi.deleteTreePostsPost({"post":(p.id)}),{method:'DELETE',data:{reason}});$('#post-dialog').close();await load();await ranking();}));detail.append(actions);
}
function setReplyTarget(reply){
  state.replyTarget=reply;pendingReply=null;$('#reply-target').hidden=!reply;
  $('#reply-target span').textContent=reply?`回复 @${reply.authorName}`:'';
  $('#reply-body').placeholder=reply?`回复 @${reply.authorName}…`:'友善交流，让故事多一份温度…';
  if(reply)$('#reply-body').focus();
}
$('#cancel-reply').onclick=()=>setReplyTarget(null);
function paintReplies(){
  const list=$('#replies');$('#reply-count').textContent=String(state.detail?.repliers||state.replies.length);
  if(!state.replies.length){list.replaceChildren(el('p',state.authorOnly?'楼主还没有回复。':'还没有讨论，欢迎留下第一条评论。','comments-empty'));return;}
  paintDiscussion(list, state.replies, r => {
    const row=el('article',undefined,'reply'+(r.parentReplyId&&r.parentReplyId!=='0'?' nested':''));row.dataset.replyId=r.id;
    const avatar=el('span',[...r.authorName][0]||'友','comment-avatar'),copy=el('div');
    const name=el('div',r.authorName,'reply-name');if(String(r.authorId)===String(state.detail.authorId))name.append(el('span','作者','author-badge'));copy.append(name);
    if(r.parentReplyId&&r.parentReplyId!=='0')copy.append(el('div',`回复 @${r.replyToName||'树下的朋友'}`,'reply-address'));
    copy.append(el('p',r.body));const tools=el('div',undefined,'reply-tools');tools.append(el('time',formatChatTime(r.createdAt)),button('回复',()=>setReplyTarget(r)));
    if(String(r.authorId)===String(api.user.id))tools.append(button('删除',async()=>{await api.request(treeApi.deleteTreeRepliesReply({"reply":(r.id)}),{method:'DELETE'});state.replies=state.replies.filter(x=>x.id!==r.id);paintReplies();}));
    copy.append(tools);row.append(avatar,copy);return row;
  });
}
function resetReplies() {
  ++replyGeneration;replyRequest=null;state.replyBefore='';state.replies=[];
  document.querySelectorAll('[data-reply-author]').forEach(control=>control.setAttribute('aria-pressed',String((control.dataset.replyAuthor==='author')===state.authorOnly)));
  $('#replies').replaceChildren();$('#older-replies').hidden=true;
  return replies();
}
function replies() {
  if(!state.detail)return;
  if(replyRequest)return replyRequest;
  const id=state.detail.id, version=replyGeneration;
  const query=new URLSearchParams({before:state.replyBefore||'0',authorOnly:String(state.authorOnly)});
  const request=api.request(treeApi.getTreePostsPostReplies({post:id},String(query))).then(rows=>{
    if(version!==replyGeneration)return;
    const seen=new Set(state.replies.map(reply=>reply.id));state.replies.push(...rows.filter(reply=>!seen.has(reply.id)));paintReplies();
    state.replyBefore=rows.at(-1)?.id||state.replyBefore;$('#older-replies').hidden=rows.length<30;
  }).finally(()=>{if(replyRequest===request)replyRequest=null;});
  replyRequest=request;
  return request;
}
document.querySelectorAll('[data-reply-author]').forEach(control=>control.onclick=()=>{
  if(!state.detail)return;
  state.authorOnly=control.dataset.replyAuthor==='author';
  resetReplies().catch(error=>showError(error,'#detail-status'));
});
async function confirmDelete(reasonRequired=false){
  const dialog=$('#confirm-dialog');let reason=dialog.querySelector('textarea');if(!reason){reason=el('textarea');reason.maxLength=200;reason.rows=3;reason.placeholder='管理员删除原因（必填，将记录审计）';dialog.querySelector('footer').before(reason);}reason.hidden=!reasonRequired;reason.value='';
  return new Promise(resolve=>{let accepted=null;$('#confirm-delete').onclick=()=>{if(reasonRequired&&!reason.value.trim()){reason.focus();reason.setCustomValidity('请填写删除原因');reason.reportValidity();return;}reason.setCustomValidity('');accepted=reason.value.trim();dialog.close();};reason.oninput=()=>reason.setCustomValidity('');dialog.addEventListener('close',()=>resolve(accepted),{once:true});dialog.showModal();});
}
$('#report-form').onsubmit=async event=>{event.preventDefault();const form=event.currentTarget,button=form.querySelector('button[type=submit]'),fields=new FormData(form),kind=String(fields.get('reason')||''),detail=String(fields.get('detail')||'').trim();if(!kind)return;
  button.disabled=true;try{await api.request(treeApi.postTreePostsPostReports({"post":(state.reportPost)}),{method:'POST',data:{reason:kind+(detail?'：'+detail:'')}});$('#report-dialog').close();$('#detail-status').textContent='举报已提交，管理员会核实处理。';}catch(e){showError(e,'#report-status');}finally{button.disabled=false;}};
$('#older-replies').onclick=()=>replies()?.catch(e=>showError(e,'#detail-status'));
$('#reply-form').onsubmit=async event=>{event.preventDefault();const form=event.currentTarget,body=$('#reply-body').value.trim();if(!body)return;
  const id=state.detail.id;const parentReplyId=state.replyTarget?.id||'0';pendingReply=pendingReply?.body===body&&pendingReply.parentReplyId===parentReplyId?pendingReply:{requestId:randomId(),body,parentReplyId};const submit=form.querySelector('button[type=submit]');submit.disabled=true;
  try{await api.request(treeApi.postTreePostsPostReplies({"post":(id)}),{method:'POST',data:pendingReply});
    if(state.detail?.id===id){pendingReply=null;form.reset();setReplyTarget(null);state.authorOnly=false;await resetReplies();}
    await load();}
  catch(error){showError(error,'#detail-status');}finally{submit.disabled=false;}
};
$('#show-publish').onclick=()=>{
  if (!api.user) return location.assign('/phase11/');
  if (rankingView.category) $('#publish-category').value = rankingView.category;
  $('#publish-dialog').showModal();
};
new ActivityPublisher($('#publish-form'),api,{initialDestination:'tree',files:()=>[...$('#media-files').files,...(state.recorded?[state.recorded]:[])],status:$('#publish-status'),onPublished:async()=>{state.recorded=null;$('#record-state').textContent='';$('#publish-dialog').close();state.before='';state.previous=[];await load();await ranking();}});
const recorder = new VoiceRecorder($('#record-voice'),$('#record-state'),file=>{state.recorded=file;});
$('#publish-dialog').addEventListener('close',()=>recorder.stop());
function applyView(view){
  if(!['tree','feed','rank'].includes(view))view='tree';document.body.dataset.treeView=view;document.body.classList.remove('list-mode');
  const copy={tree:['神树全景','旋转神树，点亮果实阅读故事。'],feed:['故事广场','发现不同领域的故事与讨论。'],rank:['七日心动榜','查看各领域过去七天的热门故事。']}[view];
  $('#view-title').textContent=copy[0];$('#view-description').textContent=copy[1];
  document.querySelectorAll('[data-tree-tab]').forEach(button=>{const active=button.dataset.treeTab===view;button.classList.toggle('active',active);button.setAttribute('aria-pressed',String(active));});
  scene?.setVisible(view==='tree');
  if(view==='tree')ensureScene().then(()=>requestAnimationFrame(()=>scene?.resize()));history.replaceState(null,'','#'+view);
}
const pageTransition=new TreePageTransition({surface:$('.community-main'),current:()=>document.body.dataset.treeView,apply:applyView});
$('#view-title').tabIndex=-1;
function switchView(view,options){return pageTransition.navigate(view,options);}
document.querySelectorAll('[data-tree-tab]').forEach(button=>button.onclick=()=>switchView(button.dataset.treeTab));
function firstPage(){state.before='';state.previous=[];const hours=Number($('#time-filter')?.value||0);state.since=hours?String(Date.now()-hours*3600000):'';
  document.querySelectorAll('[data-story-media]').forEach(control=>control.setAttribute('aria-pressed',String(control.dataset.storyMedia===$('#media-filter').value)));
  $('#clear-keyword').hidden=!state.keyword;return load();}
function chooseCategory(id) {
  const category = state.categories.find(item => item.id === id);
  const selection = category?.id || '';
  $('#category-filter').value = selection;
  document.querySelectorAll('[data-category]').forEach(button => {
    const selected=button.dataset.category===selection;button.classList.toggle('active',selected);button.setAttribute('aria-pressed',String(selected));
  });
  const url = new URL(location.href);
  if (category) url.searchParams.set('topic', category.name);
  else url.searchParams.delete('topic');
  history.replaceState(null, '', url);
  $('#rank-scope').textContent=category ? `${category.name}榜` : '全站榜';
  return Promise.all([rankingView.setCategory(selection).catch(showError), firstPage()]);
}
$('#rank-category').onchange = event => chooseCategory(event.target.value);
$('#apply-filter').onclick=()=>{switchView('feed');$('#filter-dialog').close();chooseCategory($('#category-filter').value);};$('#open-filter').onclick=()=>$('#filter-dialog').showModal();$('#refresh').onclick=()=>load();
function searchStories(value){
  state.keyword=[...value.trim()].slice(0,64).join('');$('#story-keyword').value=state.keyword;
  const url=new URL(location.href);if(state.keyword)url.searchParams.set('q',state.keyword);else url.searchParams.delete('q');
  history.replaceState(null,'',url);return firstPage();
}
$('#story-search').onsubmit=event=>{event.preventDefault();searchStories($('#story-keyword').value);};
$('#clear-keyword').onclick=()=>searchStories('');
document.querySelectorAll('[data-story-media]').forEach(control=>control.onclick=()=>{$('#media-filter').value=control.dataset.storyMedia;firstPage();});
$('#next-page').onclick=()=>{state.previous.push(state.before);state.before=state.posts.at(-1)?.id||'';load();};
$('#previous-page').onclick=()=>{state.before=state.previous.pop()||'';load();};
$('#view-toggle').onclick=()=>{const list=document.body.classList.toggle('list-mode');$('#view-toggle').textContent=list?'✧ 三维模式':'☷ 列表模式';$('#view-toggle').setAttribute('aria-pressed',String(list));scene?.setVisible(!list);if(!list)scene?.resize();};
$('#scene-reset').onclick=()=>scene?.reset();
$('#scene-zoom-in').onclick=()=>scene?.zoom(.84);$('#scene-zoom-out').onclick=()=>scene?.zoom(1.18);
const pageNavigation=new TreePageNavigation({current:()=>document.body.dataset.treeView,change:switchView,progress:value=>pageTransition.progress(value)});
addEventListener('pagehide',()=>{pageNavigation.dispose();pageTransition.dispose();},{once:true});
async function start(){
  try{const user=await api.restore();$('#identity').textContent=user.displayName;
    const categories = await api.request(treeApi.getTreeCategories({}));
    state.categories=categories;
    for(const category of state.categories){const b=button(category.name,()=>chooseCategory(category.id));b.dataset.category=category.id;$('#category-tabs').append(b);}
    $('#category-tabs [data-category=""]').onclick=()=>chooseCategory('');
    for(const category of state.categories){for(const selector of ['#category-filter','#publish-category','#rank-category']){const option=el('option',category.name);option.value=category.id;$(selector).append(option);}}
    $('#rank-category').disabled = false;
    applyView(location.hash.slice(1)||'tree');
    // 故事广场与榜单不加载三维模块，进入神树时再初始化。
    const sceneReady=document.body.dataset.treeView==='tree'?ensureScene():Promise.resolve();
    const query = new URL(location.href).searchParams;
    state.keyword=[...(query.get('q')||'')].slice(0,64).join('');$('#story-keyword').value=state.keyword;
    const topic = query.get('topic');
    const selectedCategory = categories.find(category => category.name === topic)?.id || '';
    await Promise.all([chooseCategory(selectedCategory),sceneReady]);
    const selectedPost=new URL(location.href).searchParams.get('post');if(selectedPost)openPost(selectedPost);
    new AnnouncementPlayer(api,$('#announcements'),{connectEvents:true}).start();
    const timer=setInterval(()=>{if(!document.hidden)ranking().catch(()=>{});},30000);
    addEventListener('pagehide',()=>{clearInterval(timer);scene?.dispose();},{once:true});
  }catch(error){$('#identity').textContent='暂未登录';showError(error);$('#ranking').replaceChildren(el('li','登录后，和大家在树下相遇。'));}
}
start();
