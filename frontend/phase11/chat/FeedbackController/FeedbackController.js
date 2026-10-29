/** FeedbackController: explicit use cases; shared state/other controllers arrive through context. */
export function createFeedbackController(context) {
  const {businessApi,userPickers,unreadWatermarks,pageBudget,wsBackoff,sseBackoff,$,elements,state,messageView,history,sender,trainingStateLabels,runtime,actions,types}=context;
  const {mountNavigation,UserPicker,groupMessages,BusinessApi,AnnouncementPlayer,UnreadWatermarks,ChatHistory,canRecallMessage,ReliableSender,VirtualMessageList,RecoveryBudget,ReconnectBackoff,formatChatTime,formatDayKey,formatDateDivider}=types;

function setStatus(element, message = '', kind = '') {
  element.textContent = message;
  element.classList.toggle('success', kind === 'success');
  element.classList.toggle('error', kind === 'error');
}

function toast(message, kind = 'info', lifetime = 3600) {
  const item = document.createElement('div');
  item.className = `toast ${kind}`;
  const copy = document.createElement('span');
  copy.textContent = message;
  item.append(copy);
  $('#toast-region').append(item);
  window.setTimeout(() => item.remove(), lifetime);
}

function initials(value, fallback = 'U') {
  const text = String(value || '').trim();
  if (!text) return fallback;
  return [...text].slice(0, 2).join('').toUpperCase();
}

function createClientId() {
  if (globalThis.crypto?.randomUUID) return globalThis.crypto.randomUUID();
  if (globalThis.crypto?.getRandomValues) {
    const bytes = new Uint8Array(16);
    globalThis.crypto.getRandomValues(bytes);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const hex = [...bytes].map(value => value.toString(16).padStart(2, '0')).join('');
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  }
  return `msg-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}-${Math.random().toString(36).slice(2)}`;
}

function setButtonBusy(button, busy, busyText = '处理中…') {
  if (!button) return;
  if (busy) {
    button.dataset.originalMarkup = button.innerHTML;
    button.textContent = busyText;
    button.disabled = true;
  } else {
    if (button.dataset.originalMarkup) button.innerHTML = button.dataset.originalMarkup;
    button.disabled = false;
  }
}

async function api(path, options = {}) {
  businessApi.csrf = state.csrf;
  const account = runtime.accountAbort;
  const signal = options.signal ? AbortSignal.any([options.signal, account.signal]) : account.signal;
  const result = await businessApi.request(path, {...options, signal});
  if(account!==runtime.accountAbort)throw new DOMException('账号已切换','AbortError');
  return result;
}

function formatBytes(value) {
  const bytes = Number(value || 0);
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function paintAvatar(element, user, fallback) {
  if (!element) return;
  const url = user?.avatarUrl;
  element.style.backgroundImage = url ? `url(${JSON.stringify(url).slice(1, -1)})` : '';
  element.classList.toggle('has-image', Boolean(url));
  element.textContent = url ? '' : initials(user?.displayName || user?.username, fallback);
}

function actionListEmpty(text) {
  const item = document.createElement('li');
  item.className = 'list-empty';
  item.textContent = text;
  return item;
}

  return {setStatus,toast,initials,createClientId,setButtonBusy,api,formatBytes,paintAvatar,actionListEmpty};
}
