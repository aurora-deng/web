import { treeApi } from "../../tree/TreeApi/TreeApi.js?v=1b40f8c19ed29802";
import { profilesApi } from "../ProfilesApi/ProfilesApi.js?v=1b40f8c19ed29802";
import { activitiesApi } from "../../activities/ActivitiesApi/ActivitiesApi.js?v=1b40f8c19ed29802";
import { mountProfileSettings } from '../ProfileSettings/ProfileSettings.js?v=1b40f8c19ed29802';
import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=1b40f8c19ed29802';
import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=1b40f8c19ed29802';
import { ActivityPublisher } from '../../shared/ActivityPublisher/ActivityPublisher.js?v=1b40f8c19ed29802';
import { ActivityDetail } from '../../shared/ActivityDetail/ActivityDetail.js?v=1b40f8c19ed29802';
import { AnnouncementPlayer } from '../../shared/AnnouncementPlayer/AnnouncementPlayer.js?v=1b40f8c19ed29802';
import { formatShortDateTime } from '../../shared/TimeFormat/TimeFormat.js?v=1b40f8c19ed29802';
mountNavigation('profile');
const $=s=>document.querySelector(s),api=new BusinessApi();
const state={owner:'',self:false,channel:'all',section:'timeline',before:'0',rows:[],audience:''};
const titles={timeline:'日常，有迹可循',information:'关于我',favorites:'珍藏的故事',settings:'个人设置'};
const element=(tag,text,cls)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;};
const error=e=>{$('#profile-status').textContent=e.status===401?'请先前往消息页面登录。':e.message;};
const detail=new ActivityDetail(api,()=>load());
const settings = mountProfileSettings($('#settings-section'), api, profile);
let player;
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
  if (!state.owner || (state.section !== 'timeline' && state.section !== 'favorites')) return;
  const current=++version;$('#profile-status').textContent='';
  const before=older?state.before:'0';let rows,next;
  if(state.section==='favorites'){const result=await api.request(activitiesApi.getActivitiesFavorites({}, "before=" + String(before)));rows=result.items;next=result.nextCursor;}
  else{rows=await api.request(activitiesApi.getActivities({}, "author=" + String(state.owner) + "&before=" + String(before) + "&channel=" + String(state.channel)));next=rows.length===24?rows.at(-1).id:'0';}
  if(version!==current)return;
  state.rows=older?[...state.rows,...rows]:rows;state.before=String(next);$('#older-activities').hidden=!next||String(next)==='0';render();
}
document.querySelectorAll('[data-channel]').forEach(b=>b.onclick=()=>{state.channel=b.dataset.channel;document.querySelectorAll('[data-channel]').forEach(x=>x.classList.toggle('active',x===b));load().catch(error);});
function showSection(section) {
  if (!Object.hasOwn(titles, section) || !state.owner) return;
  if (!state.self && (section === 'settings' || section === 'favorites')) return;
  state.section = section;
  ++version;
  for (const button of document.querySelectorAll('[data-section]')) {
    const selected = button.dataset.section === section;
    button.classList.toggle('active', selected);
    button.setAttribute('aria-pressed', String(selected));
  }
  $('#information-section').hidden = section !== 'information';
  $('#timeline-section').hidden = section !== 'timeline' && section !== 'favorites';
  $('#settings-section').hidden = section !== 'settings';
  $('.timeline-filters').hidden = section === 'favorites' || state.audience === 'tree-only';
  $('#section-title').textContent = section === 'timeline' && state.audience === 'tree-only'
    ? '公开的树上故事' : titles[section];
  player?.setControlsHost(section === 'settings' ? settings.notificationPanel : null);
}
document.querySelectorAll('[data-section]').forEach(button => button.onclick = () => {
  showSection(button.dataset.section);
  const url = new URL(location.href);
  const next = state.section === 'settings' ? '#settings' : '';
  if (url.hash !== next) { url.hash = next; history.replaceState(null, '', url); }
  load().catch(error);
});
window.addEventListener('hashchange', () => {
  if (location.hash === '#settings') showSection('settings');
  else if (state.section === 'settings') { showSection('timeline'); load().catch(error); }
});
$('#older-activities').onclick=()=>load(true).catch(error);
$('#publish-activity').onclick=()=>$('#profile-publish-dialog').showModal();
document.querySelectorAll('[data-close-publisher]').forEach(b=>b.onclick=()=>$('#profile-publish-dialog').close());
new ActivityPublisher($('#profile-publish-form'),api,{files:()=>[...$('#profile-media-files').files],status:$('#profile-publish-status'),onPublished:async()=>{$('#profile-publish-dialog').close();await load();}});
async function profile() {
  const p=await api.request(profilesApi.getProfilesUser({"user":(state.owner)}));$('#public-name').textContent=p.displayName;$('#public-handle').textContent='@'+p.username;$('#public-biography').textContent=p.biography||'这个人留了一点神秘感。';
  const avatar=$('#public-avatar');avatar.replaceChildren();if(p.avatarUrl){const img=element('img');img.src=p.avatarUrl;img.alt=p.displayName;avatar.append(img);}else avatar.textContent=[...p.displayName][0]||'N';
  state.self = p.self === true && String(p.id) === String(api.user?.id);
  state.audience = p.audience;
  if (p.audience === 'tree-only') state.channel='tree';
  $('#favorites-tab').hidden = !state.self;
  $('#publish-activity').hidden = !state.self;
  $('#settings-tab').hidden = !state.self;
  settings.setProfile(state.self ? p : null);
  const list=$('#public-information');list.replaceChildren();for(const [label,value] of [['显示名称',p.displayName],['用户名','@'+p.username],['简介',p.biography||'未公开']])list.append(element('dt',label),element('dd',value));
}
async function start() {
  try {
    const me = await api.restore();
    const url = new URL(location.href);
    const owner = url.searchParams.get('user') || String(me.id);
    if (!/^\d+$/.test(owner)) throw new Error('无效用户');
    state.owner = owner;
    await profile();
    player = new AnnouncementPlayer(api,$('#profile-announcements'));
    showSection(state.self && location.hash === '#settings' ? 'settings' : 'timeline');
    player.start();
    if (state.self) {
      const categories = await api.request(treeApi.getTreeCategories({}));
      for (const category of categories) {
        const option = element('option', category.name);
        option.value = category.id;
        $('#profile-publish-category').append(option);
      }
    }
    await load();
    const post = url.searchParams.get('post');
    if (post && state.section === 'timeline') await detail.open(post);
  } catch (e) { error(e); }
}
start();
