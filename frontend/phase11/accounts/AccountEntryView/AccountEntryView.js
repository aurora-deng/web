import { workspaceReturn } from '../../shared/AccountSession/AccountSession.js?v=4364119c78f60e44';

export function accountEntryView(root = document, url = new URL(location.href)) {
  const auth = root.getElementById('auth-panel');
  const chat = root.getElementById('chat-panel');
  const checking = root.getElementById('account-check');
  const status = root.getElementById('account-check-status');
  const retry = root.getElementById('account-check-retry');
  const back = root.getElementById('cancel-account-change');
  const returnTo = workspaceReturn(url.searchParams.get('returnTo'), url.origin);
  const requestedChange = url.searchParams.get('account') === 'change';

  function show(view) {
    auth.hidden = view !== 'form';
    auth.classList.toggle('hidden', auth.hidden);
    chat.hidden = view !== 'chat';
    chat.classList.toggle('hidden', chat.hidden);
    checking.hidden = view === 'form' || view === 'chat';
    status.textContent = view === 'error' ? '暂时无法确认登录状态，请重试。' : '正在恢复账号会话…';
    retry.hidden = view !== 'error';
  }

  return { returnTo, requestedChange, show, retry,
    showChange() {
      root.getElementById('login-title').textContent = '切换 NEBULA 账号';
      back.href = returnTo || '/phase11/';
      back.hidden = false;
      show('form');
    },
  };
}
