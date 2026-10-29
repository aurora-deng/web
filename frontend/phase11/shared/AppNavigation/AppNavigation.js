const icons={chat:'<path d="M5 4h14a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H9l-5 3v-3H3V6a2 2 0 0 1 2-2Z"/><path d="M7 9h10M7 13h7"/>',tree:'<path d="m12 2 6 7h-3l5 7h-6v6h-4v-6H4l5-7H6Z"/>',admin:'<path d="m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6Z"/><path d="m8 12 3 3 5-6"/>'};
const svg=name=>`<svg viewBox="0 0 24 24" aria-hidden="true">${icons[name]}</svg>`;
import { LocalAssistantApi } from '../LocalAssistantApi/LocalAssistantApi.js?v=4ec58ab3c1e38adb';
import { openSpeechSettings } from '../SpeechSettings/SpeechSettings.js?v=4ec58ab3c1e38adb';
import { mountTheme } from '../Theme/Theme.js?v=4ec58ab3c1e38adb';
import { companionSession } from '../ClientBootstrap/ClientBootstrap.js?v=4ec58ab3c1e38adb';
import { mountGlobalAssistant } from '../AssistantPanel/AssistantPanel.js?v=4ec58ab3c1e38adb';
// module 的顶层等待保证 Cookie 引导完成后，页面才启动 /api/me、WS 和 SSE。
await companionSession();
/** 三个业务区共用同一入口；导航可见性不代替服务端权限检查。 */
export function mountNavigation(active){
  if(document.getElementById('nebula-navigation'))return;
  const css=document.createElement('link');css.rel='stylesheet';css.href='/phase11/shared/AppNavigation/AppNavigation.css?v=4ec58ab3c1e38adb';document.head.append(css);
  document.body.dataset.workspace=active;
  const header=document.createElement('header');header.id='nebula-navigation';
  header.innerHTML=`<a class="workspace-brand" href="/phase11/"><span class="workspace-mark">N<span>✦</span></span><span>NEBULA<small>人与故事，在这里相遇</small></span></a><nav aria-label="功能区切换">${[['chat','消息','/phase11/'],['tree','苦情树','/phase11/tree/'],['admin','管理中心','/phase11/admin/']].map(([id,label,url])=>`<a href="${url}" ${id===active?'aria-current="page"':''}>${svg(id)}<span>${label}</span></a>`).join('')}</nav><div class="workspace-tools"><details class="sound-menu"><summary title="公告声音设置">♫<span>声音与通知</span></summary><div id="global-audio-panel"><p class="audio-placeholder">登录后可设置公告声音</p></div></details><a class="workspace-account" href="/phase11/" title="前往聊天管理个人资料"><span class="account-dot"></span><span data-nav-user>账号</span></a></div>`;
  document.body.prepend(header);
  const themeButton=document.createElement('button');themeButton.type='button';themeButton.className='workspace-theme';
  const tools=header.querySelector('.workspace-tools');tools.prepend(themeButton);mountGlobalAssistant(tools);tools.insertBefore(tools.querySelector('.workspace-assistant'),tools.querySelector('.sound-menu'));mountTheme(themeButton);
  const menu=header.querySelector('.sound-menu');const links=document.createElement('div');links.className='audio-settings-links';links.innerHTML='<button type=button data-speech-settings>语音 API 与音色设置 ↗</button><a href="/phase11/admin/#speech">站点公告语音 · 管理员</a><p role=status></p>';menu.append(links);new ResizeObserver(entries=>menu.style.setProperty('--audio-height',entries[0].borderBoxSize?.[0]?.blockSize+'px')).observe(header.querySelector('#global-audio-panel'));
  links.querySelector('button').onclick=async()=>{const status=links.querySelector('[role=status]');try{const api=new LocalAssistantApi();const result=await api.start();if(!result.available){status.textContent='个人语音 Key 在 Windows 本机入口配置；站点公告请由管理员进入公告音色设置。';return;}await openSpeechSettings(api);}catch(e){status.textContent=e.message;}};
  const user=me=>{const value=me?.user||me;header.querySelector('[data-nav-user]').textContent=value?.displayName||'登录';header.querySelector('.workspace-account').href=value?.id?'/phase11/profile/?user='+encodeURIComponent(value.id):'/phase11/';};
  window.addEventListener('nebula:user',event=>user(event.detail));
  fetch('/api/me',{credentials:'same-origin',cache:'no-store'}).then(r=>r.ok?r.json():null).then(user).catch(()=>user(null));
}
