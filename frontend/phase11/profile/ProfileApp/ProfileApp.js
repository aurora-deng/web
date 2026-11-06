import { treeApi } from "../../tree/TreeApi/TreeApi.js?v=144f1e6009a3d461";
import { profilesApi } from "../ProfilesApi/ProfilesApi.js?v=144f1e6009a3d461";
import { activitiesApi } from "../../activities/ActivitiesApi/ActivitiesApi.js?v=144f1e6009a3d461";
import { accountsApi } from "../../accounts/AccountsApi/AccountsApi.js?v=144f1e6009a3d461";
import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=144f1e6009a3d461';
import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=144f1e6009a3d461';
import { ActivityPublisher } from '../../shared/ActivityPublisher/ActivityPublisher.js?v=144f1e6009a3d461';
import { ActivityDetail } from '../../shared/ActivityDetail/ActivityDetail.js?v=144f1e6009a3d461';
import { AnnouncementPlayer } from '../../shared/AnnouncementPlayer/AnnouncementPlayer.js?v=144f1e6009a3d461';
import { formatShortDateTime } from '../../shared/TimeFormat/TimeFormat.js?v=144f1e6009a3d461';
mountNavigation('profile');
const $=s=>document.querySelector(s),api=new BusinessApi();
const state={owner:'',channel:'all',section:'timeline',before:'0',rows:[],busy:false};
const element=(tag,text,cls)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;};
const error=e=>{$('#profile-status').textContent=e.status===401?'请先前往消息页面登录。':e.message;};
const detail=new ActivityDetail(api,()=>load());
function render() {
  const list=$('#activity-list');list.replaceChildren();
  if(!state.rows.length)list.append(element('div',state.section==='favorites'?'还没有收藏，去树下发现值得珍藏的故事。':'暂时没有你可见的动态。','activity-empty'));
  for(const p of state.rows){const card=element('article',undefined,'timeline-card');
    const header=element('header'),author=element('a',p.authorName);author.href='/phase11/profile/?user='+encodeURIComponent(p.authorId);
    // dateTime 只放合法 ISO 时间；无效输入不再抛 RangeError（toISOString 对 Invalid Date 会抛）。
    const stamp=element('time',formatShortDateTime(p.createdAt));
    if(Number(p.createdAt)>0&&!Number.isNaN(new Date(Number(p.createdAt)).getTime()))stamp.dateTime=new Date(Number(p.createdAt)).toISOString();
    header.append(author,stamp);
    const body=element('button',p.body||'分享了一段生活记录','timeline-body');body.onclick=()=>detail.open(p.id).catch(error);
    const media=element('div',undefined,'timeline-media');for(const m of p.media||[]){const item=element(m.kind==='image'?'img':'audio');item.src=m.url;if(m.kind==='image'){item.loading='lazy';item.alt='动态图片';item.onclick=()=>detail.open(p.id).catch(error);}else{item.controls=true;item.preload='none';}media.append(item);}
    const footer=element('footer');footer.append(element('span',p.treeChannel?'✿ 树上故事':p.visibility==='friends'?'♧ 好友可见':p.visibility==='private'?'♢ 仅自己':'◎ 登录用户可见'));
    const open=element('button',`♡ ${p.likes}　回应 ${p.repliers}`);open.onclick=()=>detail.open(p.id).catch(error);footer.append(open);card.append(header,body,media,footer);list.append(card);
  }
}
let version=0;
async function load(older=false) {
  const current=++version;$('#profile-status').textContent='';
  const before=older?state.before:'0';let rows,next;
  if(state.section==='favorites'){const result=await api.request(activitiesApi.getActivitiesFavorites({}, "before=" + String(before)));rows=result.items;next=result.nextCursor;}
  else{rows=await api.request(activitiesApi.getActivities({}, "author=" + String(state.owner) + "&before=" + String(before) + "&channel=" + String(state.channel)));next=rows.length===24?rows.at(-1).id:'0';}
  if(version!==current)return;
  state.rows=older?[...state.rows,...rows]:rows;state.before=String(next);$('#older-activities').hidden=!next||String(next)==='0';render();
}
document.querySelectorAll('[data-channel]').forEach(b=>b.onclick=()=>{state.channel=b.dataset.channel;document.querySelectorAll('[data-channel]').forEach(x=>x.classList.toggle('active',x===b));load().catch(error);});
document.querySelectorAll('[data-section]').forEach(b=>b.onclick=()=>{state.section=b.dataset.section;document.querySelectorAll('[data-section]').forEach(x=>x.classList.toggle('active',x===b));$('#information-section').hidden=state.section!=='information';$('#timeline-section').hidden=state.section==='information';$('.timeline-filters').hidden=state.section==='favorites';$('#section-title').textContent=state.section==='information'?'关于我':state.section==='favorites'?'珍藏的故事':'日常，有迹可循';if(state.section!=='information')load().catch(error);});
$('#older-activities').onclick=()=>load(true).catch(error);
$('#publish-activity').onclick=()=>$('#profile-publish-dialog').showModal();
document.querySelectorAll('[data-close-publisher]').forEach(b=>b.onclick=()=>$('#profile-publish-dialog').close());
new ActivityPublisher($('#profile-publish-form'),api,{files:()=>[...$('#profile-media-files').files],status:$('#profile-publish-status'),onPublished:async()=>{$('#profile-publish-dialog').close();await load();}});
$('#profile-edit-form').onsubmit=async event=>{
  event.preventDefault();const form=event.currentTarget,button=form.querySelector('[type=submit]');button.disabled=true;
  try{await api.request(accountsApi.patchMe({}),{method:'PATCH',data:{displayName:form.elements.displayName.value.trim(),biography:form.elements.biography.value.trim()}});
    await api.request(profilesApi.patchMeProfileDisplay({}),{method:'PATCH',data:{showBiography:String(form.elements.showBiography.checked),showAvatar:String(form.elements.showAvatar.checked),allowTreeFriendRequests:String(form.elements.allowTreeFriendRequests.checked)}});await profile();$('#profile-status').textContent='资料已更新';
  }catch(e){error(e);}finally{button.disabled=false;}
};
async function profile() {
  const p=await api.request(profilesApi.getProfilesUser({"user":(state.owner)}));$('#public-name').textContent=p.displayName;$('#public-handle').textContent='@'+p.username;$('#public-biography').textContent=p.biography||'这个人留了一点神秘感。';
  const avatar=$('#public-avatar');avatar.replaceChildren();if(p.avatarUrl){const img=element('img');img.src=p.avatarUrl;img.alt=p.displayName;avatar.append(img);}else avatar.textContent=[...p.displayName][0]||'N';
  $('.timeline-filters').hidden=p.audience==='tree-only';if(p.audience==='tree-only'){state.channel='tree';$('#section-title').textContent='公开的树上故事';$('#profile-status').textContent='你们尚未成为好友，仅展示公开到神树的故事。';}$('#favorites-tab').hidden=!p.self;$('#publish-activity').hidden=!p.self;$('#profile-edit-form').hidden=!p.self;
  const list=$('#public-information');list.replaceChildren();for(const [label,value] of [['显示名称',p.displayName],['用户名','@'+p.username],['简介',p.biography||'未公开']])list.append(element('dt',label),element('dd',value));
  if(p.self){const form=$('#profile-edit-form');for(const key of ['username','displayName','biography'])form.elements[key].value=p[key]||'';form.elements.showBiography.checked=p.showBiography;form.elements.showAvatar.checked=p.showAvatar;form.elements.allowTreeFriendRequests.checked=p.allowTreeFriendRequests;}
}
async function start(){try{const me=await api.restore();state.owner=new URL(location.href).searchParams.get('user')||String(me.id);if(!/^\d+$/.test(state.owner))throw new Error('无效用户');await profile();const categories=await api.request(treeApi.getTreeCategories({}));for(const c of categories){const option=element('option',c.name);option.value=c.id;$('#profile-publish-category').append(option);}await load();const post=new URL(location.href).searchParams.get('post');if(post)await detail.open(post);new AnnouncementPlayer(api,$('#profile-announcements')).start();}catch(e){error(e);}}
start();
