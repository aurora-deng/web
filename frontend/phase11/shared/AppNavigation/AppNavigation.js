import { readAdminRole } from '../../admin/AdminAccess/AdminAccess.js?v=1b40f8c19ed29802';
const icons={chat:'<path d="M5 4h14a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H9l-5 3v-3H3V6a2 2 0 0 1 2-2Z"/><path d="M7 9h10M7 13h7"/>',tree:'<path d="m12 2 6 7h-3l5 7h-6v6h-4v-6H4l5-7H6Z"/>',study:'<path d="M3 5h5a4 4 0 0 1 4 4v12a4 4 0 0 0-4-4H3V5Zm18 0h-5a4 4 0 0 0-4 4v12a4 4 0 0 1 4-4h5V5Z"/>',admin:'<path d="m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6Z"/><path d="m8 12 3 3 5-6"/>'};
const svg=name=>`<svg viewBox="0 0 24 24" aria-hidden="true">${icons[name]}</svg>`;
import { BusinessApi } from '../BusinessApi/BusinessApi.js?v=1b40f8c19ed29802';
import { accountEntry, protectRestoredWorkspace } from '../AccountSession/AccountSession.js?v=1b40f8c19ed29802';
import { mountTheme } from '../Theme/Theme.js?v=1b40f8c19ed29802';
import { companionSession } from '../ClientBootstrap/ClientBootstrap.js?v=1b40f8c19ed29802';
import { mountGlobalAssistant } from '../AssistantPanel/AssistantPanel.js?v=1b40f8c19ed29802';
import { mountChatBackdrop } from '../../chat/ChatBackdrop/ChatBackdrop.js?v=1b40f8c19ed29802';
// module 的顶层等待保证 Cookie 引导完成后，页面才启动 /api/me、WS 和 SSE。
await companionSession();
/** 权限由当前服务端会话确认，切换账号后丢弃旧会话的查询。 */
export function mountNavigation(active, { api = new BusinessApi() } = {}){
  if(document.getElementById('nebula-navigation'))return;
  protectRestoredWorkspace();
  const css=document.createElement('link');css.rel='stylesheet';css.href='/phase11/shared/AppNavigation/AppNavigation.css?v=1b40f8c19ed29802';document.head.append(css);
  document.body.dataset.workspace=active;
  mountChatBackdrop(document.body);
  const header=document.createElement('header');header.id='nebula-navigation';
  header.innerHTML=`<a class="workspace-brand" href="/phase11/"><span class="workspace-mark">N<span>✦</span></span><span>NEBULA<small>人与故事，在这里相遇</small></span></a><nav aria-label="功能区切换">${[['chat','缘续-聊天','/phase11/'],['tree','缘生-广场','/phase11/tree/'],['study','研途','/phase11/study/']].map(([id,label,url])=>`<a href="${url}" ${id===active?'aria-current="page"':''}>${svg(id)}<span>${label}</span></a>`).join('')}</nav><div class="workspace-tools"><details class="sound-menu"><summary title="公告声音设置">♫<span>声音与通知</span></summary><div id="global-audio-panel"><p class="audio-placeholder">登录后可设置公告声音</p><a data-personal-settings hidden href="/phase11/profile/#settings">个人声音与通知设置 ↗</a></div></details><a class="workspace-account" aria-busy="true" title="正在恢复账号"><span class="account-dot"></span><span data-nav-user>…</span></a></div>`;
  document.body.prepend(header);
  const themeButton=document.createElement('button');themeButton.type='button';themeButton.className='workspace-theme';
  const tools=header.querySelector('.workspace-tools');tools.prepend(themeButton);mountGlobalAssistant(tools,{fullPage:active==='assistant'});tools.insertBefore(tools.querySelector('.workspace-assistant'),tools.querySelector('.sound-menu'));mountTheme(themeButton);
  const nav = header.querySelector('nav');
  const account = header.querySelector('.workspace-account');
  const accountName = header.querySelector('[data-nav-user]');
  const settings = header.querySelector('[data-personal-settings]');
  const currentWorkspace = location.pathname + location.search + location.hash;
  let generation = 0;
  let userId;
  let roleRequest;
  let adminLink;
  async function showUser(user) {
    const current = ++generation;
    roleRequest?.abort();
    adminLink?.remove();
    userId = user?.id;
    account.removeAttribute('aria-busy');
    accountName.textContent = user?.displayName || '登录';
    account.title = user?.id ? '个人主页' : '登录 NEBULA';
    account.href = user?.id ? '/phase11/profile/' : active === 'chat' ? '/phase11/' : accountEntry(currentWorkspace);
    settings.hidden = !user?.id;
    if (!user?.id) return { user: null, role: null };

    roleRequest = new AbortController();
    try {
      const role = await readAdminRole(api, user.id, roleRequest.signal);
      if (current !== generation) return null;
      if (role) {
        adminLink = document.createElement('a');
        adminLink.href = '/phase11/admin/';
        adminLink.dataset.adminLink = '';
        adminLink.innerHTML = `${svg('admin')}<span>管理中心</span>`;
        if (active === 'admin') adminLink.setAttribute('aria-current', 'page');
        nav.append(adminLink);
      }
      return { user, role };
    } catch (error) {
      return current === generation ? { user, role: null, error } : null;
    }
  }
  window.addEventListener('nebula:user', event => showUser(event.detail));
  window.addEventListener('nebula:profile-updated', event => {
    const profile = event.detail;
    if (userId && String(profile?.id) === String(userId)) accountName.textContent = profile.displayName;
  });
  const initial = generation;
  return api.restore().then(
    user => initial === generation ? showUser(user) : null,
    error => {
      if (initial !== generation) return null;
      if (error.status === 401) return showUser(null);
      account.removeAttribute('aria-busy');
      account.title = '账号状态暂不可用';
      return { user: null, role: null, error };
    },
  );
}
