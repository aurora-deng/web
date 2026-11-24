import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=1b40f8c19ed29802';
import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=1b40f8c19ed29802';
import { accountEntry } from '../../shared/AccountSession/AccountSession.js?v=1b40f8c19ed29802';

const api = new BusinessApi();
mountNavigation('study', { api });
const element = id => document.getElementById(id);
const status = element('study-sync-state');
const surface = element('study-surface');
const entry = element('study-entry');
const download = element('study-download-settings');
const switchAccount = element('study-switch-account');
const scopePath = /^\/api\/study\/[a-f0-9]{32}\/$/;
const accountId = /^[1-9][0-9]{0,19}$/;
const current = () => location.pathname + location.search + location.hash;
element('study-login').onclick = event => { event.currentTarget.href = accountEntry(current()); };
switchAccount.onclick = event => { event.currentTarget.href = accountEntry(current(), { change: true }); };
let scope;
let frame;
let checking;
let sessionRequest;

function detach() {
  frame?.remove();
  frame = undefined;
  scope = undefined;
  download.hidden = true;
}

function requireEntry(title, detail, needsLogin) {
  detach();
  switchAccount.hidden = needsLogin || !api.user;
  status.textContent = needsLogin ? '未登录' : '账号同步暂不可用';
  element('study-entry-title').textContent = title;
  element('study-entry-detail').textContent = detail;
  element('study-login').hidden = !needsLogin;
  element('study-retry').hidden = needsLogin;
  entry.hidden = false;
}

function mountWorkspace(session) {
  if (!scopePath.test(session.url) || session.url !== `/api/study/${session.scope}/`) throw new Error('无效的研途入口');
  if (!accountId.test(session.accountId) || String(api.user?.id) !== session.accountId) throw new Error('研途账号已改变');
  status.textContent = `已绑定 ${api.user.displayName || api.user.username || 'NEBULA 账号'} · 仅本人可见`;
  switchAccount.hidden = false;
  if (scope === session.scope && frame) return;
  detach();
  scope = session.scope;
  frame = document.createElement('iframe');
  frame.title = '研途学习工作区';
  frame.src = session.url + (location.hash === '#download-settings' ? location.hash : '');
  frame.setAttribute('allow', 'clipboard-read; clipboard-write');
  surface.replaceChildren(frame);
  download.hidden = false;
  entry.hidden = true;
}

async function syncSession() {
  if (checking) return checking;
  const request = new AbortController();
  sessionRequest = request;
  const pending = (async () => {
    try {
      const response = await fetch('/api/study/session', { credentials: 'same-origin', cache: 'no-store',
        signal: AbortSignal.any([request.signal, AbortSignal.timeout(5000)]) });
      if (request.signal.aborted) return;
      if (response.status === 401) {
        api.user = null;
        requireEntry('登录后打开研途', '使用同一个 NEBULA 账号，就能在客户端和网页继续学习。', true);
        return;
      }
      if (response.status === 404) {
        requireEntry('当前入口暂不提供研途', '请使用已启用研途的 NEBULA 网关。Windows 客户端应连接该网关的 HTTPS 地址。', false);
        return;
      }
      if (!response.ok) throw new Error('研途账号服务不可用');
      const session = await response.json();
      if (request.signal.aborted) return;
      if (String(api.user?.id) !== session.accountId) {
        detach();
        await api.restore();
      }
      if (!request.signal.aborted) mountWorkspace(session);
    } catch {
      if (request.signal.aborted) return;
      if (frame) status.textContent = '同步暂不可用 · 本机待同步改动已保留';
      else requireEntry('暂时无法打开研途', '请确认当前 NEBULA 地址已启用研途账号服务。', false);
    }
  })().finally(() => { if (checking === pending) checking = undefined; });
  checking = pending;
  return checking;
}

function refreshIdentity() {
  sessionRequest?.abort();
  checking = undefined;
  api.user = null;
  detach();
  entry.hidden = true;
  switchAccount.hidden = true;
  status.textContent = '正在检查账号';
  syncSession();
}

download.onclick = () => frame?.contentWindow.postMessage({ type: 'nebula-study-download-settings' }, location.origin);
element('study-retry').onclick = syncSession;
window.addEventListener('message', event => {
  if (event.origin === location.origin && event.source === frame?.contentWindow &&
      event.data?.type === 'nebula-study-session-changed') {
    refreshIdentity();
  }
});
window.addEventListener('nebula:companion-identity', refreshIdentity);
window.addEventListener('focus', syncSession);
window.addEventListener('online', syncSession);
const watcher = setInterval(() => { if (!document.hidden) syncSession(); }, 15000);
window.addEventListener('pagehide', () => { clearInterval(watcher); sessionRequest?.abort(); }, { once: true });
syncSession();
