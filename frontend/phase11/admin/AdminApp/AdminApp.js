import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=4ec58ab3c1e38adb';
mountNavigation('admin');
import {BusinessApi} from '../../shared/BusinessApi/BusinessApi.js?v=4ec58ab3c1e38adb';
import {AnnouncementPlayer} from '../../shared/AnnouncementPlayer/AnnouncementPlayer.js?v=4ec58ab3c1e38adb';
import {openSpeechSettings} from '../../shared/SpeechSettings/SpeechSettings.js?v=4ec58ab3c1e38adb';
const api=new BusinessApi(),$=s=>document.querySelector(s);let view=['users','reports','audits','messaging','categories','models','runtime','speech'].includes(location.hash.slice(1))?location.hash.slice(1):'users',cursor='',rows=[],role,pageToken='',pageTrail=[];
const el=(tag,text,cls)=>{const n=document.createElement(tag);if(text!==undefined)n.textContent=text;if(cls)n.className=cls;return n;};
const names={users:'用户与权限',reports:'内容举报',audits:'操作审计',messaging:'消息队列',categories:'内容分类',models:'模型治理',runtime:'运行状态',speech:'公告音色'};
function failure(error){$('#status').textContent=error.status===401?'请先返回聊天页登录。':error.message;}
function action(text,run){const b=el('button',text);b.onclick=async()=>{b.disabled=true;try{await run();}catch(e){failure(e);}finally{b.disabled=false;}};return b;}
function reason(){const value=prompt('请填写操作原因（最多200字）');if(!value?.trim())throw new Error('已取消：操作需要填写原因。');return value;}
async function mutate(path,data){await api.request(path,{method:'POST',data});await reloadPages();$('#status').textContent='已由服务器确认保存。';}
function table(headers,items,render){const t=el('table'),head=el('tr');headers.forEach(h=>head.append(el('th',h)));t.append(head);const body=el('tbody');items.forEach(item=>{const tr=el('tr');render(item).forEach(value=>{const td=el('td');if(value instanceof Node)td.append(value);else td.textContent=value;tr.append(td);});body.append(tr);});t.append(body);const node=items.length?t:Object.assign(el('div','当前没有记录。'),{className:'empty'});$('#content').replaceChildren(node);return node;}
/* 滚动位置必须自己存：重放期间 #content 先从 N 行缩到 50 行再长回来，
   浏览器会在缩小的那一瞬间把滚动位置夹回顶部，之后不会自动恢复。
   注意真实滚动容器不是固定的：`.content{overflow:auto}` 没有高度约束时它自己不滚
   （scrollHeight===clientHeight），实际滚的是 document.scrollingElement。
   所以按「overflow 允许滚动 且 当前确实滚得动」筛，并始终带上文档根。 */
function scrollHosts(){
  const hosts=[];
  for(let node=$('#content');node&&node!==document.body;node=node.parentElement){
    const style=getComputedStyle(node);
    if(/(auto|scroll)/.test(style.overflowY)&&node.scrollHeight>node.clientHeight)hosts.push(node);
  }
  const root=document.scrollingElement||document.documentElement;
  if(root&&!hosts.includes(root))hosts.push(root);
  return hosts;
}
/* 分页语义：pageToken = 取「当前页」用的 token；cursor = 服务端给的「下一页」token。
   两者必须分开，否则「原地重载」和「翻到下一页」会互相踩。 */
async function reloadPages(){
  const tokens=[...pageTrail,pageToken];
  const kept=scrollHosts().map(node=>({node,top:node.scrollTop}));
  pageToken='';pageTrail=[];
  await load();
  // 逐页重放，并把 pageTrail 恢复成「除最后一页之外的全部 token」。
  // 不恢复的话第二次处置只剩第 1 页 + 第 N 页，中间页丢失、窗口不连续。
  for(const token of tokens.slice(1)){pageTrail.push(pageToken);pageToken=token;await load(true);}
  // 越界时浏览器会自己夹取，不抛异常；行数减少导致的比例偏移可接受，但不许回到顶部。
  kept.forEach(({node,top})=>{if(top&&node.scrollTop!==top)node.scrollTop=top;});
}
async function load(append=false){
  $('#content').replaceChildren(el('div','正在加载…','loading'));
  try{return await renderView(append);}
  catch(error){
    // 错误态隔离：先把旧表清掉，再报错。绝不能让上一页的数据留在可点击的位置。
    $('#content').replaceChildren(el('div','加载失败，本页没有可显示的数据。'+(error.status===401?'请先返回聊天页登录。':''),'empty'));
    throw error;
  }
}
async function renderView(append=false){$('#status').textContent='';$('#title').textContent=names[view];$('#description').textContent='当前身份：'+({user:'普通用户',moderator:'内容管理员',administrator:'站点管理员'}[role]||'未知');$('#next-page').hidden=true;
  if(view==='runtime'){
    const [live,ready,metrics]=await Promise.all([api.request('/health/live'),api.request('/health/ready'),fetch('/metrics',{credentials:'same-origin',cache:'no-store'}).then(async r=>{if(!r.ok)throw new Error('运行指标暂不可用');return r.text();})]);
    const values=metrics.split('\n').map(line=>line.trim().split(/\s+/)).filter(parts=>parts.length===2&&parts[0].startsWith('webserver_')).slice(0,200);
    const cache=await api.request('/api/admin/cache').catch(()=>null);const rows=[['进程存活',JSON.stringify(live)],['就绪检查',JSON.stringify(ready)],['Redis 读缓存',cache?JSON.stringify(cache):'需站点管理员权限'],...values];table(['指标','服务器当前值'],rows,r=>r);return;
  }
  if(view==='speech'){
    const box=el('div');box.append(el('h2','本地公告音色'),el('p','试听采用固定文案，不上传语音。首次生成需要加载本地模型；文字公告独立于语音服务。'));
    if(role==='administrator')box.append(action('设置云语音、表达与回退',()=>openSpeechSettings(api,{site:true})));
    for(const [voice,label]of[['zf_001','成熟女声'],['zm_009','青年沉稳男声']]){
      const row=el('section');row.append(el('h3',label+' · '+voice),action('试听预置音色',async()=>{let result=await api.request('/api/admin/speech/preview',{method:'POST',data:{voice}});const deadline=Date.now()+110000;while(result.state==='pending'&&Date.now()<deadline){$('#status').textContent='正在后台准备试听音频…';await new Promise(resolve=>setTimeout(resolve,1000));result=await api.request('/api/admin/speech/preview/'+voice);}if(result.state!=='ready')throw new Error('试听暂未就绪，请稍后重试。');$('#status').textContent='试听已就绪。';let player=row.querySelector('audio');if(!player){player=el('audio');player.controls=true;row.append(player);}player.src=result.url;try{await player.play();}catch{$('#status').textContent='音频已就绪，可点击播放器试听。';}}),action('作为我的公告音色',async()=>{const settings=await api.request('/api/me/notification-settings');await api.request('/api/me/notification-settings',{method:'PATCH',data:{sound:String(settings.sound),voice}});$('#status').textContent='已替换当前账号的公告音色，重新进入页面后生效。';}));box.append(row);
    }$('#content').replaceChildren(box);return;
  }
  if(view==='models'){
    const models=await api.request('/api/admin/models');
    table(['节点','名称','当前版本','操作'],models,m=>[m.id,m.name,m.activeVersion,action('查看版本与评测',()=>versions(m))]);return;
  }
  const query=view==='users'?'after': 'before';const path=view==='messaging'?'/api/admin/messaging':view==='categories'?'/api/admin/tree/categories':`/api/admin/${view}?${query}=${pageToken||'0'}`;
  const data=await api.request(path);rows=append?rows.concat(data).slice(-200):data;
  if(view==='users')table(['用户','账号','站点身份','状态','操作'],rows,u=>{const tools=el('div');if(role==='administrator'){const select=el('select');for(const [value,text]of[['user','普通用户'],['moderator','内容管理员'],['administrator','站点管理员']])select.add(new Option(text,value));select.value=u.role;tools.append(select,action('设置角色',()=>mutate('/api/admin/users/'+u.id,{action:'role',role:select.value,reason:reason()})),action(u.active?'停用':'启用',()=>mutate('/api/admin/users/'+u.id,{action:'active',active:String(!u.active),reason:reason()})),action('吊销会话',()=>mutate('/api/admin/users/'+u.id,{action:'revoke',reason:reason()})));}return[u.name+' #'+u.id,u.username,u.role,u.active?'正常':'已停用',tools];});
  if(view==='reports')table(['举报编号','果实','举报人','原因','处理'],rows,r=>{const tools=el('div');tools.append(action('查看',()=>window.open('/phase11/tree/?post='+r.postId,'_blank','noopener')),action('隐藏果实',()=>mutate('/api/admin/tree/posts/'+r.postId+'/hide',{reason:reason()})),action('完成处置',()=>mutate(`/api/admin/reports/${r.id}/resolve`,{dismiss:'false',reason:reason()})),action('驳回',()=>mutate(`/api/admin/reports/${r.id}/resolve`,{dismiss:'true',reason:reason()})));return[r.id,r.postId,r.reporterId,r.reason,tools];});
  if(view==='audits')table(['时间','操作者','操作','资源','原因'],rows,r=>[r.at,r.actorId,r.action,r.resource,r.reason]);
  if(view==='categories'){
    table(['编号','分类','状态','操作'],rows,r=>{const tools=el('div');tools.append(action('改名',()=>{const name=prompt('分类名称（最多20字）',r.name);if(name?.trim())return mutate('/api/admin/tree/categories',{id:r.id,name,active:String(r.active)});}),action(r.active?'停用':'启用',()=>mutate('/api/admin/tree/categories',{id:r.id,name:r.name,active:String(!r.active)})));return[r.id,r.name,r.active?'已启用':'已停用',tools];});
    $('#content').append(action('＋ 新建分类',()=>{const name=prompt('新分类名称');if(name?.trim())return mutate('/api/admin/tree/categories',{id:'0',name,active:'true'});}));
  }
  if(view==='messaging'){
    const summary=table(['状态','积压数量','最早事件'],rows,r=>[r.state,r.count,r.firstId]);
    const [dead,failed]=await Promise.all([api.request('/api/admin/messaging/dead'),api.request('/api/admin/messaging/consumer-failures')]);
    const deadTable=table(['失败事件','类型','次数','原因','操作'],dead,r=>[r.id,r.type,r.attempts,r.error,action('审计后重放',()=>mutate(`/api/admin/messaging/${r.id}/replay`,{reason:reason()}))]);
    const failures=table(['消费失败','来源事件','Topic','次数','原因','操作'],failed,r=>[r.id,r.eventId,r.topic,r.attempts,r.error,r.eventId!=='0'?action('重新投递',()=>mutate(`/api/admin/messaging/${r.eventId}/replay`,{reason:reason()})):'检查 Broker 中的原消息']);
    $('#content').replaceChildren(el('h3','持久发布队列'),summary,el('h3','发布死信与人工重放'),deadTable,el('h3','消费者失败（已完成消费不会重复执行）'),failures);
  }
  if(['users','reports','audits'].includes(view)){cursor=data.at(-1)?.id||pageToken;$('#next-page').hidden=data.length<50;}
}
async function versions(model,after='0',previous=[]){
  const data=await api.request(`/api/admin/models/${model.id}/versions?after=${after}`),items=previous.concat(data).slice(-200);
  table(['版本','运行方式','状态','制品摘要','操作'],items,v=>{
    const tools=el('div');tools.append(action('评测门禁',async()=>{const q=await api.request(`/api/admin/models/${model.id}/versions/${v.id}/quality`);$('#status').textContent=`${q.allowed?'门禁通过':'门禁拒绝'} · 运行 ${q.runId} · ${q.reason}`;}));
    if(v.state==='candidate'||v.state==='retired')tools.append(action(v.state==='retired'?'回滚到此版本':'启用候选版本',async()=>{const why=reason();if(!confirm('将由服务器进行质量门禁与制品试装，成功后才切换当前版本。继续？'))return;$('#status').textContent='正在校验模型，请稍候…';const accepted=await api.request(`/api/admin/models/${model.id}/versions/${v.id}`,{method:'POST',data:{action:v.state==='retired'?'rollback':'activate',reason:why}});let operation={state:'pending'};const deadline=Date.now()+600000;while(operation.state==='pending'&&Date.now()<deadline){await new Promise(resolve=>setTimeout(resolve,1000));operation=await api.request('/api/admin/model-operations/'+accepted.operationId);}if(operation.state!=='completed')throw new Error('模型操作未成功完成，请检查评测门禁和操作审计；不要直接重复发布。');await versions(model);$('#status').textContent='模型切换已由服务器确认，结果已写入模型操作审计。';}));
    return[v.id,v.runtime,v.state,v.checksum,tools];
  });
  $('#title').textContent=model.name+' · 模型版本';
  if(data.length===50)$('#content').append(action('加载更多版本',()=>versions(model,data.at(-1).id,items)));
}
document.querySelectorAll('[data-view]').forEach(b=>b.onclick=()=>{view=b.dataset.view;cursor='';pageToken='';pageTrail=[];rows=[];document.querySelectorAll('[data-view]').forEach(n=>n.classList.toggle('selected',n===b));load().catch(failure);});
$('#refresh').onclick=()=>{cursor='';pageToken='';pageTrail=[];load().catch(failure);};$('#next-page').onclick=()=>{pageTrail.push(pageToken);pageToken=cursor;load(true).catch(failure);};
(async()=>{try{const user=await api.restore();const identity=await api.request('/api/admin/me');role=identity.role;$('#identity').textContent=user.displayName+' · '+role;if(role==='user')throw new Error('此账号没有站点管理权限。群主身份不会自动成为站点管理员。');if(role==='moderator'){view='reports';for(const name of ['users','models','messaging'])document.querySelector('[data-view='+name+']').hidden=true;document.querySelectorAll('[data-view]').forEach(n=>n.classList.toggle('selected',n.dataset.view==='reports'));}await load();new AnnouncementPlayer(api,$('#announcements'),{connectEvents:true}).start();}catch(e){failure(e);}})();
