const workspacePaths = new Set(['/phase11/', '/phase11/tree/', '/phase11/study/',
  '/phase11/profile/', '/phase11/assistant/', '/phase11/admin/',
  '/phase11/admin/permissions/', '/phase11/agent-control/']);

let identityRead;
let channel;
let watching = false;

export function readAccountSession(request) {
  if (identityRead) return identityRead;
  const pending = Promise.resolve().then(request);
  identityRead = pending;
  const finish = () => { if (identityRead === pending) identityRead = undefined; };
  pending.then(finish, finish);
  return pending;
}

export function workspaceReturn(value, origin) {
  if (typeof value !== 'string' || !value.startsWith('/phase11/') || value.length > 2048 || /[\\\u0000-\u0020]/.test(value)) return '';
  const url = new URL(value, origin);
  if (url.origin !== origin || !workspacePaths.has(url.pathname)) return '';
  url.searchParams.delete('account');
  url.searchParams.delete('returnTo');
  return url.pathname + url.search + url.hash;
}

export function accountEntry(destination, { change = false, origin = location.origin } = {}) {
  const query = new URLSearchParams();
  const safe = workspaceReturn(destination, origin);
  if (safe) query.set('returnTo', safe);
  if (change) query.set('account', 'change');
  return '/phase11/' + (query.size ? '?' + query : '');
}

export function invalidateAccountSession() {
  identityRead = undefined;
  channel?.postMessage('changed');
}

export function protectRestoredWorkspace() {
  if (watching) return;
  watching = true;
  const suspend = () => { document.documentElement.dataset.accountSuspended = ''; };
  if (typeof BroadcastChannel === 'function') {
    channel = new BroadcastChannel('nebula:verified-account-session');
    channel.onmessage = event => {
      if (event.data !== 'changed') return;
      suspend();
      location.reload();
    };
  }
  window.addEventListener('pagehide', event => {
    if (event.persisted) suspend();
    channel?.close();
    channel = undefined;
  }, { once: true });
  window.addEventListener('pageshow', event => {
    if (event.persisted) location.reload();
  });
}
