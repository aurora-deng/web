const $ = selector => document.querySelector(selector);

const elements = {
  authPanel: $('#auth-panel'),
  chatPanel: $('#chat-panel'),
  authStatus: $('#auth-status'),
  chatStatus: $('#chat-status'),
  conversationList: $('#conversation-list'),
  sidebarFriendList: $('#sidebar-friend-list'),
  conversationEmpty: $('#conversation-empty'),
  conversationLoading: $('#conversation-loading'),
  messageList: $('#message-list'),
  messageForm: $('#message-form'),
  emptyConversation: $('#empty-conversation'),
  registerDialog: $('#register-dialog'),
  friendDialog: $('#friend-dialog'),
  conversationDialog: $('#conversation-dialog'),
  profileDialog: $('#profile-dialog'),
  searchDialog: $('#search-dialog'),
  memberDialog: $('#member-dialog'),
  modelDialog: $('#model-dialog'),
  detailPane: $('#detail-pane'),
};

const state = {
  me: null,
  csrf: '',
  conversation: null,
  conversations: [],
  friends: [],
  socket: null,
  events: null,
  reconnectTimer: null,
  activeGenerations: new Map(),
  unread: new Map(),
  members: [],
  aiConversations: new Set(),
  conversationFilter: 'all',
  conversationQuery: '',
  realtime: { ws: 'offline', sse: 'offline' },
  leaving: false,
};

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

/**
 * 为聊天消息生成客户端幂等 ID。
 *
 * crypto.randomUUID() 只保证出现在安全上下文（HTTPS/localhost）里；开发环境从
 * http://<虚拟机IP> 打开时部分浏览器没有这个函数。getRandomValues 在这种页面仍可用，
 * 因此手工组装 RFC 4122 v4 UUID，并保留极旧浏览器的时间戳兜底。
 */
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

function conversationTitle(conversation) {
  return conversation.title || (conversation.kind === 'group'
    ? `群聊 #${conversation.id}`
    : `私聊 #${conversation.id}`);
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
  const headers = new Headers(options.headers || {});
  const method = String(options.method || 'GET').toUpperCase();
  if (options.body && !headers.has('Content-Type')) headers.set('Content-Type', 'application/json');
  if (state.csrf && !['GET', 'HEAD'].includes(method)) headers.set('X-CSRF-Token', state.csrf);

  let response;
  try {
    response = await fetch(path, { credentials: 'same-origin', ...options, method, headers });
  } catch {
    throw new Error('无法连接服务器，请检查网络或服务状态');
  }

  const text = await response.text();
  let data = null;
  if (text) {
    try { data = JSON.parse(text); }
    catch { data = { message: text }; }
  }
  if (!response.ok) {
    const error = new Error(data?.message || data?.error || `请求失败（HTTP ${response.status}）`);
    error.status = response.status;
    throw error;
  }
  return data;
}

function updateProfileView() {
  if (!state.me) return;
  const name = state.me.displayName || state.me.username;
  $('#profile-name').textContent = name;
  $('#profile-handle').textContent = `@${state.me.username} · ID ${state.me.id}`;
  $('#profile-avatar').textContent = initials(name);
  $('#profile-dialog-avatar').textContent = initials(name);
}

function setRealtimeState(kind, status) {
  const isWebSocket = kind === 'ws';
  state.realtime[kind] = status;
  const label = status === 'online' ? (isWebSocket ? '已连接' : '运行中')
    : status === 'error' ? '重连中' : '离线';
  const dots = isWebSocket
    ? [$('#header-ws-dot'), $('#detail-ws-dot')]
    : [$('#detail-sse-dot')];
  const labels = isWebSocket
    ? [$('#header-ws-state'), $('#detail-ws-state'), $('#model-ws-status')]
    : [$('#detail-sse-state'), $('#model-sse-status')];
  dots.forEach(dot => {
    if (!dot) return;
    dot.classList.toggle('online', status === 'online');
    dot.classList.toggle('ok', status === 'online');
    dot.classList.toggle('error', status === 'error');
  });
  labels.forEach(text => {
    if (!text) return;
    text.textContent = isWebSocket && text.id === 'header-ws-state'
      ? `WebSocket ${label}` : label;
    text.classList.toggle('ok', status === 'online');
  });
  if (isWebSocket) $('#reconnect-banner').classList.toggle(
    'hidden', status !== 'error' || !state.me,
  );
}

async function showChat(me) {
  state.me = me;
  state.leaving = false;
  updateProfileView();
  elements.authPanel.classList.add('hidden');
  elements.chatPanel.classList.remove('hidden');
  connectRealtime();
  await Promise.allSettled([loadConversations(), loadFriendsPreview(), refreshPendingBadge()]);
}

function showLoggedOut() {
  state.me = null;
  state.csrf = '';
  state.conversation = null;
  state.conversations = [];
  state.members = [];
  clearTimeout(state.reconnectTimer);
  state.socket?.close();
  state.events?.close();
  elements.chatPanel.classList.add('hidden');
  elements.authPanel.classList.remove('hidden');
}

function renderConversations() {
  $('#conversation-count').textContent = String(state.conversations.length);
  const query = state.conversationQuery.toLocaleLowerCase();
  const visible = state.conversations.filter(conversation => {
    if (state.conversationFilter !== 'all' && state.conversationFilter !== 'ai'
        && conversation.kind !== state.conversationFilter) return false;
    if (state.conversationFilter === 'ai'
        && !state.aiConversations.has(Number(conversation.id))) return false;
    return !query || conversationTitle(conversation).toLocaleLowerCase().includes(query);
  });
  const totalUnread = [...state.unread.values()].reduce((sum, value) => sum + value, 0);
  $('#total-unread').textContent = totalUnread > 99 ? '99+' : String(totalUnread);
  $('#total-unread').classList.toggle('hidden', totalUnread === 0);
  elements.conversationEmpty.classList.toggle('hidden', visible.length !== 0);
  elements.conversationList.replaceChildren(...visible.map(conversation => {
    const item = document.createElement('li');
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'conversation-item';
    button.classList.toggle('active', Number(conversation.id) === Number(state.conversation?.id));

    const avatar = document.createElement('span');
    avatar.className = 'avatar';
    avatar.textContent = conversation.kind === 'group' ? '群' : initials(conversationTitle(conversation), '聊');
    const copy = document.createElement('span');
    copy.className = 'conversation-copy';
    const title = document.createElement('strong');
    title.textContent = conversationTitle(conversation);
    const detail = document.createElement('small');
    detail.textContent = conversation.kind === 'group'
      ? `群组 · 消息序号 ${Math.max(0, Number(conversation.nextSequence) - 1)}`
      : `私聊 · 消息序号 ${Math.max(0, Number(conversation.nextSequence) - 1)}`;
    copy.append(title, detail);
    button.append(avatar, copy);

    const unread = state.unread.get(Number(conversation.id)) || 0;
    if (unread) {
      const badge = document.createElement('span');
      badge.className = 'unread-badge';
      badge.textContent = unread > 99 ? '99+' : String(unread);
      button.append(badge);
    }
    button.addEventListener('click', () => openConversation(conversation));
    item.append(button);
    return item;
  }));
}

async function loadConversations() {
  elements.conversationLoading.classList.remove('hidden');
  try {
    state.conversations = await api('/api/conversations');
    if (state.conversation) {
      state.conversation = state.conversations.find(
        item => Number(item.id) === Number(state.conversation.id),
      ) || state.conversation;
    }
    renderConversations();
    setStatus(elements.chatStatus);
  } catch (error) {
    setStatus(elements.chatStatus, error.message, 'error');
  } finally {
    elements.conversationLoading.classList.add('hidden');
  }
}

async function openConversation(conversation) {
  state.conversation = conversation;
  state.unread.delete(Number(conversation.id));
  renderConversations();

  const title = conversationTitle(conversation);
  $('#conversation-title').textContent = title;
  $('#conversation-subtitle').textContent = conversation.kind === 'group'
    ? `群组会话 · ID ${conversation.id}`
    : `点对点会话 · ID ${conversation.id}`;
  $('#conversation-avatar').textContent = conversation.kind === 'group' ? '群' : initials(title, '聊');
  $('#composer-destination').textContent = title;
  $('#detail-title').textContent = title;
  $('#detail-kind').textContent = conversation.kind === 'group'
    ? `群聊 · 会话 ID ${conversation.id}` : `私聊 · 会话 ID ${conversation.id}`;
  $('#detail-avatar').textContent = conversation.kind === 'group' ? '群' : initials(title, '聊');
  $('#search-messages').disabled = false;
  $('#add-member').classList.add('hidden');
  elements.emptyConversation.classList.add('hidden');
  elements.messageList.classList.remove('hidden');
  elements.messageForm.classList.remove('hidden');
  elements.messageList.replaceChildren();
  refreshCancelButton();
  elements.chatPanel.classList.add('conversation-open');

  try {
    const [messages, members] = await Promise.all([
      api(`/api/conversations/${encodeURIComponent(conversation.id)}/messages?after=0&limit=100`),
      api(`/api/conversations/${encodeURIComponent(conversation.id)}/members`),
    ]);
    state.members = members;
    const aiMember = members.find(member => member.aiAccount &&
      Number(member.userId) !== Number(state.me?.id));
    const aiInput = elements.messageForm.elements.aiUserId;
    aiInput.value = aiMember ? String(aiMember.userId) : '';
    aiInput.placeholder = aiMember
      ? `${aiMember.displayName || aiMember.username}（已自动识别）`
      : '群聊中可填写 AI 成员 ID';
    if (aiMember || messages.some(message => message.clientMessageId?.startsWith('ai:'))) {
      state.aiConversations.add(Number(conversation.id));
    }
    renderMessages(messages);
    renderConversations();
    renderConversationMembers();
    const me = members.find(member => Number(member.userId) === Number(state.me?.id));
    $('#add-member').classList.toggle(
      'hidden', conversation.kind !== 'group' || me?.role !== 'owner',
    );
    if (messages.length) acknowledge(messages.at(-1));
    setStatus(elements.chatStatus);
  } catch (error) {
    setStatus(elements.chatStatus, error.message, 'error');
  }
}

function renderConversationMembers() {
  $('#detail-member-count').textContent = String(state.members.length);
  $('#detail-member-list').replaceChildren(...(state.members.length ? state.members.map(member => {
    const item = document.createElement('li');
    const avatar = document.createElement('span');
    avatar.className = 'avatar';
    avatar.textContent = Number(member.userId) === Number(state.me?.id)
      ? '我' : (member.aiAccount ? 'AI' : initials(member.displayName || member.username, `#${member.userId}`));
    const copy = document.createElement('span');
    copy.className = 'member-copy';
    const name = document.createElement('strong');
    name.textContent = Number(member.userId) === Number(state.me?.id)
      ? (state.me.displayName || state.me.username)
      : `${member.displayName || member.username || `用户 ${member.userId}`}${member.aiAccount ? ' · AI' : ''}`;
    const progress = document.createElement('small');
    progress.textContent = `已读至 #${member.lastReadSequence || 0}`;
    copy.append(name, progress);
    const role = document.createElement('span');
    role.className = 'member-role';
    role.textContent = member.role === 'owner' ? '创建者' : '成员';
    item.append(avatar, copy, role);
    return item;
  }) : [actionListEmpty('暂无成员信息')]));
}

function formatMessageTime(value) {
  const date = Number(value) > 0 ? new Date(Number(value)) : null;
  if (!date || Number.isNaN(date.getTime())) return '';
  return date.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
}

function createMessageElement(message, options = {}) {
  const mine = Number(message.senderId) === Number(state.me?.id);
  const aiMessage = Boolean(options.ai || message.clientMessageId?.startsWith('ai:'));
  const item = document.createElement('li');
  item.className = 'message';
  item.classList.toggle('mine', mine);
  item.classList.toggle('ai-streaming', Boolean(options.streaming));
  if (message.id !== undefined) item.dataset.messageId = String(message.id);
  if (options.generationId) item.dataset.generationId = options.generationId;
  if (message.sequence !== undefined) item.dataset.sequence = String(message.sequence);

  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  avatar.textContent = mine ? initials(state.me?.displayName || state.me?.username) : (aiMessage ? 'AI' : `#${message.senderId || '?'}`);
  const content = document.createElement('div');
  content.className = 'message-content';
  const meta = document.createElement('p');
  meta.className = 'message-author';
  const time = formatMessageTime(message.createdAt);
  meta.textContent = options.streaming ? 'AI 正在生成'
    : `${aiMessage ? 'AI 账号' : mine ? '我' : `用户 ${message.senderId}`} · #${message.sequence || '…'}${time ? ` · ${time}` : ''}`;
  const bubble = document.createElement('div');
  bubble.className = 'message-bubble';
  bubble.classList.toggle('typing-caret', Boolean(options.streaming));
  bubble.textContent = message.body || '';
  content.append(meta, bubble);
  item.append(avatar, content);
  return item;
}

function renderMessages(messages) {
  elements.messageList.replaceChildren(...messages.map(message => createMessageElement(message)));
  if (!messages.length) {
    const empty = document.createElement('li');
    empty.className = 'list-empty';
    empty.textContent = '这里还没有消息，发送第一句话吧。';
    elements.messageList.append(empty);
  }
  scrollMessages();
}

function scrollMessages() {
  requestAnimationFrame(() => { elements.messageList.scrollTop = elements.messageList.scrollHeight; });
}

function existingMessage(messageId) {
  return [...elements.messageList.children].find(item => item.dataset.messageId === String(messageId));
}

function generationElement(generationId) {
  return [...elements.messageList.children].find(item => item.dataset.generationId === String(generationId));
}

function appendMessage(message) {
  if (!message || existingMessage(message.id)) return;
  if (message.clientMessageId?.startsWith('ai:')) {
    generationElement(message.clientMessageId.slice(3))?.remove();
  }
  elements.messageList.querySelector('.list-empty')?.remove();
  elements.messageList.append(createMessageElement(message));
  scrollMessages();
}

function sendSocket(payload) {
  if (state.socket?.readyState !== WebSocket.OPEN) return false;
  try {
    state.socket.send(JSON.stringify(payload));
    return true;
  } catch (error) {
    console.error('WebSocket send failed', error);
    return false;
  }
}

function acknowledge(message) {
  if (!state.conversation || !message?.sequence) return;
  const base = {
    v: 1,
    id: createClientId(),
    conversationId: Number(state.conversation.id),
    sequence: Number(message.sequence),
  };
  sendSocket({ ...base, type: 'chat.ack' });
  sendSocket({ ...base, id: createClientId(), type: 'chat.read' });
}

function refreshCancelButton() {
  const active = [...state.activeGenerations.values()]
    .find(item => Number(item.conversationId) === Number(state.conversation?.id));
  $('#cancel-generation').classList.toggle('hidden', !active);
  $('#cancel-generation').dataset.generationId = active?.generationId || '';
}

function onRealtimeMessage(message) {
  if (message.type === 'chat.message') {
    const isCurrent = Number(message.conversationId) === Number(state.conversation?.id);
    if (isCurrent) {
      appendMessage(message);
      acknowledge(message);
    } else {
      const id = Number(message.conversationId);
      state.unread.set(id, (state.unread.get(id) || 0) + 1);
      renderConversations();
    }
    return;
  }
  if (message.type === 'chat.accepted') {
    const text = message.status === 'duplicate'
      ? '重复消息已由服务器安全去重。'
      : '消息已由服务器持久化；接收方离线也可以稍后恢复。';
    setStatus(elements.chatStatus, text, 'success');
  } else if (message.type === 'chat.error') {
    setStatus(elements.chatStatus, message.status || '消息发送失败', 'error');
  } else if (message.type === 'ai.error') {
    state.activeGenerations.delete(message.replyTo);
    refreshCancelButton();
    setStatus(elements.chatStatus, message.status || 'AI 请求失败', 'error');
  } else if (message.type === 'ai.cancel.accepted') {
    setStatus(elements.chatStatus, '正在停止生成…');
  }
}

function connectRealtime() {
  clearTimeout(state.reconnectTimer);
  state.socket?.close();
  setRealtimeState('ws', 'error');
  const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
  const socket = new WebSocket(`${protocol}://${location.host}/ws`);
  state.socket = socket;
  socket.addEventListener('open', () => {
    if (state.socket !== socket) return;
    setRealtimeState('ws', 'online');
    if (state.conversation) openConversation(state.conversation);
  });
  socket.addEventListener('close', () => {
    if (state.socket !== socket) return;
    setRealtimeState('ws', state.me && !state.leaving ? 'error' : 'offline');
    if (state.me && !state.leaving) state.reconnectTimer = setTimeout(connectRealtime, 1600);
  });
  socket.addEventListener('error', () => setRealtimeState('ws', 'error'));
  socket.addEventListener('message', event => {
    try { onRealtimeMessage(JSON.parse(event.data)); }
    catch { setStatus(elements.chatStatus, '收到无法解析的实时消息', 'error'); }
  });

  state.events?.close();
  setRealtimeState('sse', 'error');
  const events = new EventSource('/events');
  state.events = events;
  events.addEventListener('open', () => setRealtimeState('sse', 'online'));
  events.addEventListener('error', () => setRealtimeState('sse', state.me ? 'error' : 'offline'));
  events.addEventListener('message.notification', event => {
    try { onRealtimeMessage(JSON.parse(event.data)); } catch { /* EventSource will keep running. */ }
  });
  events.addEventListener('conversation.invited', async event => {
    try {
      const notice = JSON.parse(event.data);
      toast(`你已加入会话 #${notice.conversationId}`);
      await loadConversations();
    } catch { /* A later refresh can recover the conversation list. */ }
  });
  events.addEventListener('friend.request', async () => {
    toast('收到新的好友申请');
    await refreshPendingBadge();
    if (elements.friendDialog.open) await loadContacts();
  });
  events.addEventListener('ai.token', event => {
    const token = JSON.parse(event.data);
    if (Number(token.conversationId) !== Number(state.conversation?.id)) return;
    let item = generationElement(token.generationId);
    if (!item) {
      item = createMessageElement({ senderId: 0, body: '' }, {
        ai: true, streaming: true, generationId: token.generationId,
      });
      elements.messageList.querySelector('.list-empty')?.remove();
      elements.messageList.append(item);
    }
    item.querySelector('.message-bubble').textContent += token.text;
    scrollMessages();
  });
  events.addEventListener('ai.completed', async event => {
    const completed = JSON.parse(event.data);
    state.activeGenerations.delete(completed.generationId);
    refreshCancelButton();
    if (Number(completed.conversationId) !== Number(state.conversation?.id)) return;
    const item = generationElement(completed.generationId);
    item?.querySelector('.message-bubble')?.classList.remove('typing-caret');
    item?.classList.remove('ai-streaming');
    if (completed.success) {
      await openConversation(state.conversation);
      setStatus(elements.chatStatus);
    } else {
      const reason = completed.cancelled ? '生成已取消' : (completed.error || 'AI 生成失败');
      if (item && !item.querySelector('.message-bubble').textContent) item.querySelector('.message-bubble').textContent = reason;
      setStatus(elements.chatStatus, reason, completed.cancelled ? '' : 'error');
    }
  });
}

async function refreshPendingBadge() {
  if (!state.me) return;
  try {
    const requests = await api('/api/friend-requests');
    const badge = $('#friend-badge');
    badge.textContent = String(requests.length);
    badge.classList.toggle('hidden', requests.length === 0);
    $('#show-requests').classList.toggle('has-notice', requests.length !== 0);
  } catch { /* The dialog will show the detailed error when opened. */ }
}

function actionListEmpty(text) {
  const item = document.createElement('li');
  item.className = 'list-empty';
  item.textContent = text;
  return item;
}

function contactRow(user) {
  const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  avatar.textContent = initials(user.displayName || user.username);
  const copy = document.createElement('span');
  copy.className = 'item-copy';
  const name = document.createElement('strong');
  name.textContent = user.displayName || user.username;
  const detail = document.createElement('small');
  detail.textContent = `@${user.username} · ID ${user.id}`;
  const biography = document.createElement('small');
  biography.className = 'contact-biography';
  biography.textContent = user.biography || '可接收离线消息';
  copy.append(name, detail, biography);
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'mini-button';
  button.textContent = '发消息';
  button.addEventListener('click', () => openFriendConversation(user, button));
  item.append(avatar, copy, button);
  return item;
}

function sidebarFriendRow(user) {
  const item = document.createElement('li');
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'sidebar-friend';
  button.title = `给 ${user.displayName || user.username} 发送消息`;

  const avatar = document.createElement('span');
  avatar.className = 'avatar avatar-friend';
  avatar.textContent = initials(user.displayName || user.username);
  const copy = document.createElement('span');
  copy.className = 'friend-copy';
  const name = document.createElement('strong');
  name.textContent = user.displayName || user.username;
  const detail = document.createElement('small');
  detail.textContent = `@${user.username} · ID ${user.id}`;
  const biography = document.createElement('small');
  biography.className = 'friend-biography';
  biography.textContent = user.biography || '可接收离线消息';
  const action = document.createElement('span');
  action.className = 'friend-chat-action';
  action.textContent = '私聊';
  copy.append(name, detail, biography);
  button.append(avatar, copy, action);
  button.addEventListener('click', () => openFriendConversation(user, button));
  item.append(button);
  return item;
}

function renderFriendPreview() {
  $('#sidebar-friend-count').textContent = String(state.friends.length);
  const rows = state.friends.length
    ? state.friends.map(sidebarFriendRow)
    : [actionListEmpty('还没有好友，点击“管理”添加')];
  elements.sidebarFriendList.replaceChildren(...rows);
}

async function loadFriendsPreview() {
  if (!state.me) return;
  try {
    state.friends = await api('/api/friends');
    renderFriendPreview();
  } catch (error) {
    elements.sidebarFriendList.replaceChildren(actionListEmpty(error.message));
  }
}

async function openFriendConversation(user, button) {
  setButtonBusy(button, true, '打开中…');
  try {
    await createDirectConversation(user.id);
  } catch (error) {
    setStatus(elements.chatStatus, error.message, 'error');
    toast(error.message, 'error');
  } finally {
    setButtonBusy(button, false);
  }
}

function friendRequestRow(request) {
  const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  avatar.textContent = `#${request.senderId}`;
  const copy = document.createElement('span');
  copy.className = 'item-copy';
  const name = document.createElement('strong');
  name.textContent = `用户 ${request.senderId}`;
  const detail = document.createElement('small');
  detail.textContent = `申请编号 ${request.id}`;
  copy.append(name, detail);
  const actions = document.createElement('span');
  actions.className = 'mini-actions';
  for (const [label, accept] of [['接受', 'true'], ['拒绝', 'false']]) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `mini-button ${accept === 'false' ? 'danger' : ''}`;
    button.textContent = label;
    button.addEventListener('click', async () => {
      try {
        await api(`/api/friend-requests/${encodeURIComponent(request.id)}`, {
          method: 'POST', body: JSON.stringify({ accept }),
        });
        toast(accept === 'true' ? '已接受好友申请' : '已拒绝好友申请');
        await loadContacts();
      } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
    });
    actions.append(button);
  }
  item.append(avatar, copy, actions);
  return item;
}

async function loadContacts() {
  const status = $('#friend-status');
  setStatus(status, '正在加载…');
  try {
    const [friends, requests] = await Promise.all([api('/api/friends'), api('/api/friend-requests')]);
    state.friends = friends;
    renderFriendPreview();
    $('#friend-count').textContent = String(friends.length);
    $('#request-count').textContent = String(requests.length);
    $('#friend-list').replaceChildren(...(friends.length ? friends.map(contactRow) : [actionListEmpty('还没有好友')]));
    $('#friend-request-list').replaceChildren(...(requests.length ? requests.map(friendRequestRow) : [actionListEmpty('没有待处理申请')]));
    $('#friend-badge').textContent = String(requests.length);
    $('#friend-badge').classList.toggle('hidden', requests.length === 0);
    setStatus(status);
  } catch (error) { setStatus(status, error.message, 'error'); }
}

async function createDirectConversation(peerId) {
  const conversation = await api('/api/conversations', {
    method: 'POST', body: JSON.stringify({ kind: 'direct', peerId: String(peerId) }),
  });
  elements.friendDialog.close();
  elements.conversationDialog.close();
  await loadConversations();
  await openConversation(conversation);
  toast('私聊会话已准备好');
}

function openConversationDialog() {
  setStatus($('#conversation-status'));
  elements.conversationDialog.showModal();
}

$('#login-form').addEventListener('submit', async event => {
  event.preventDefault();
  // Event.currentTarget 只在浏览器同步派发事件期间可靠。异步处理会跨过
  // await，因此先保存稳定的表单引用，后续不要再从 event 读取表单。
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  setButtonBusy(button, true, '正在登录…');
  setStatus(elements.authStatus, '正在验证账号…');
  try {
    const result = await api('/api/auth/login', {
      method: 'POST', body: JSON.stringify(Object.fromEntries(new FormData(form))),
    });
    state.csrf = result.csrfToken;
    setStatus(elements.authStatus);
    await showChat(result.user);
  } catch (error) {
    setStatus(elements.authStatus, error.message, 'error');
  } finally { setButtonBusy(button, false); }
});

$('#show-register').addEventListener('click', () => {
  setStatus($('#register-status'));
  elements.registerDialog.showModal();
});

$('#register-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const fields = Object.fromEntries(new FormData(form));
  setButtonBusy(button, true, '注册中…');
  try {
    await api('/api/auth/register', { method: 'POST', body: JSON.stringify(fields) });
    setStatus($('#register-status'), '注册成功，现在可以登录。', 'success');
    $('#login-form').elements.username.value = fields.username;
    form.reset();
    setTimeout(() => elements.registerDialog.close(), 800);
  } catch (error) { setStatus($('#register-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

elements.messageForm.addEventListener('submit', event => {
  event.preventDefault();
  if (!state.conversation) return;
  if (state.socket?.readyState !== WebSocket.OPEN) {
    setStatus(elements.chatStatus, 'WebSocket 正在重连，暂时无法发送。', 'error');
    return;
  }
  const input = event.currentTarget.elements.message;
  const content = input.value.trim();
  if (!content) return;
  const aiMode = $('#toggle-ai').getAttribute('aria-pressed') === 'true';
  const aiUserId = event.currentTarget.elements.aiUserId.value.trim();
  if (aiMode && !aiUserId) {
    setStatus(elements.chatStatus, 'AI 模式需要填写目标 AI 账号 ID。', 'error');
    event.currentTarget.elements.aiUserId.focus();
    return;
  }
  const messageId = createClientId();
  const sent = sendSocket({
    v: 1,
    type: aiMode ? 'ai.generate' : 'chat.send',
    conversationId: Number(state.conversation.id),
    id: messageId,
    ...(aiMode ? { to: Number(aiUserId) } : {}),
    content,
  });
  if (!sent) {
    setStatus(elements.chatStatus, '消息未发送：WebSocket 当前不可写，请等待重连后重试。', 'error');
    return;
  }
  if (aiMode) {
    state.activeGenerations.set(messageId, { generationId: messageId, conversationId: state.conversation.id });
    refreshCancelButton();
    setStatus(elements.chatStatus, 'AI 请求已进入后台队列。', 'success');
  } else {
    setStatus(elements.chatStatus);
  }
  input.value = '';
  input.style.height = '';
});

elements.messageForm.elements.message.addEventListener('keydown', event => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    elements.messageForm.requestSubmit();
  }
});
elements.messageForm.elements.message.addEventListener('input', event => {
  event.target.style.height = 'auto';
  event.target.style.height = `${Math.min(event.target.scrollHeight, 150)}px`;
});

$('#toggle-ai').addEventListener('click', event => {
  const active = event.currentTarget.getAttribute('aria-pressed') !== 'true';
  event.currentTarget.setAttribute('aria-pressed', String(active));
  $('#ai-target-wrap').classList.toggle('hidden', !active);
  elements.messageForm.elements.message.placeholder = active
    ? '输入给 AI 的提示词，Enter 发送'
    : '输入消息，Enter 发送，Shift + Enter 换行';
});

$('#cancel-generation').addEventListener('click', event => {
  const generationId = event.currentTarget.dataset.generationId;
  if (!generationId) return;
  sendSocket({ v: 1, type: 'ai.cancel', id: createClientId(), replyTo: generationId });
});

$('#show-conversations').addEventListener('click', loadConversations);
$('#show-friends').addEventListener('click', async () => {
  elements.friendDialog.showModal();
  await loadContacts();
});
$('#manage-friends').addEventListener('click', async () => {
  elements.friendDialog.showModal();
  await loadContacts();
});
$('#show-requests').addEventListener('click', async () => {
  elements.friendDialog.showModal();
  await loadContacts();
  $('#friend-request-list').scrollIntoView({ block: 'nearest' });
});
$('#show-model-status').addEventListener('click', () => {
  setRealtimeState('ws', state.realtime.ws);
  setRealtimeState('sse', state.realtime.sse);
  elements.modelDialog.showModal();
});
$('#new-conversation').addEventListener('click', openConversationDialog);
$('#new-direct-conversation').addEventListener('click', () => {
  openConversationDialog();
  $('#direct-conversation-form').elements.peerId.focus();
});
$('#new-group-conversation').addEventListener('click', () => {
  openConversationDialog();
  $('#group-conversation-form').elements.title.focus();
});
$('#empty-create-conversation').addEventListener('click', openConversationDialog);
$('#welcome-create-conversation').addEventListener('click', openConversationDialog);

$('#conversation-filter-input').addEventListener('input', event => {
  state.conversationQuery = event.currentTarget.value.trim();
  renderConversations();
});
document.querySelectorAll('[data-conversation-filter]').forEach(button => {
  button.addEventListener('click', () => {
    state.conversationFilter = button.dataset.conversationFilter;
    document.querySelectorAll('[data-conversation-filter]').forEach(item => {
      item.classList.toggle('active', item === button);
    });
    renderConversations();
  });
});

$('#open-global-search').addEventListener('click', () => {
  if (!state.conversation) {
    toast('请先选择一个会话；当前业务接口按会话权限执行搜索。');
    return;
  }
  $('#search-messages').click();
});

$('#toggle-details').addEventListener('click', () => elements.detailPane.classList.toggle('open'));
$('#close-details').addEventListener('click', () => elements.detailPane.classList.remove('open'));
$('#mobile-back').addEventListener('click', () => elements.chatPanel.classList.remove('conversation-open'));

document.addEventListener('keydown', event => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k' && state.me) {
    event.preventDefault();
    $('#conversation-filter-input').focus();
  }
  if (event.key === 'Escape') elements.detailPane.classList.remove('open');
});

$('#friend-request-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const receiverId = new FormData(form).get('receiverId');
  setButtonBusy(button, true, '发送中…');
  try {
    await api('/api/friend-requests', { method: 'POST', body: JSON.stringify({ receiverId }) });
    form.reset();
    setStatus($('#friend-status'), '好友申请已发送。', 'success');
  } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

$('#direct-conversation-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const peerId = new FormData(form).get('peerId');
  setButtonBusy(button, true, '创建中…');
  try { await createDirectConversation(peerId); form.reset(); }
  catch (error) { setStatus($('#conversation-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

$('#group-conversation-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const fields = Object.fromEntries(new FormData(form));
  setButtonBusy(button, true, '创建中…');
  try {
    const conversation = await api('/api/conversations', {
      method: 'POST', body: JSON.stringify({ kind: 'group', ...fields }),
    });
    elements.conversationDialog.close();
    form.reset();
    await loadConversations();
    await openConversation(conversation);
    toast('群聊已创建');
  } catch (error) { setStatus($('#conversation-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

$('#show-profile').addEventListener('click', () => {
  $('#profile-form').elements.displayName.value = state.me?.displayName || '';
  $('#profile-form').elements.biography.value = state.me?.biography || '';
  $('#profile-id-hint').textContent = `@${state.me?.username} · 用户 ID ${state.me?.id}`;
  setStatus($('#profile-status'));
  elements.profileDialog.showModal();
});

$('#profile-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  setButtonBusy(button, true, '保存中…');
  try {
    const result = await api('/api/me', {
      method: 'PATCH', body: JSON.stringify(Object.fromEntries(new FormData(form))),
    });
    state.me = result.user;
    updateProfileView();
    setStatus($('#profile-status'), '资料已保存。', 'success');
    toast('个人资料已更新');
    setTimeout(() => elements.profileDialog.close(), 600);
  } catch (error) { setStatus($('#profile-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

$('#search-messages').addEventListener('click', () => {
  $('#search-results').replaceChildren();
  setStatus($('#search-status'));
  elements.searchDialog.showModal();
  $('#search-form').elements.query.focus();
});

$('#search-form').addEventListener('submit', async event => {
  event.preventDefault();
  if (!state.conversation) return;
  const form = event.currentTarget;
  const query = new FormData(form).get('query').trim();
  const button = form.querySelector('[type="submit"]');
  setButtonBusy(button, true, '搜索中…');
  try {
    const messages = await api(`/api/search/messages?conversationId=${encodeURIComponent(state.conversation.id)}&query=${encodeURIComponent(query)}&limit=50`);
    $('#search-results').replaceChildren(...messages.map(message => {
      const item = document.createElement('li');
      const body = document.createElement('p');
      body.textContent = message.body;
      const meta = document.createElement('small');
      meta.textContent = `用户 ${message.senderId} · 消息序号 ${message.sequence}`;
      item.append(body, meta);
      return item;
    }));
    setStatus($('#search-status'), messages.length ? `找到 ${messages.length} 条结果` : '没有匹配消息。', messages.length ? 'success' : '');
  } catch (error) { setStatus($('#search-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

$('#add-member').addEventListener('click', () => {
  setStatus($('#member-status'));
  elements.memberDialog.showModal();
});

$('#member-form').addEventListener('submit', async event => {
  event.preventDefault();
  if (!state.conversation) return;
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const memberId = new FormData(form).get('memberId');
  setButtonBusy(button, true, '添加中…');
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/members`, {
      method: 'POST', body: JSON.stringify({ memberId }),
    });
    setStatus($('#member-status'), '成员已添加。', 'success');
    form.reset();
    toast('群成员已添加');
    await openConversation(state.conversation);
    setTimeout(() => elements.memberDialog.close(), 600);
  } catch (error) { setStatus($('#member-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

$('#logout').addEventListener('click', async () => {
  state.leaving = true;
  try { await api('/api/auth/logout', { method: 'POST' }); }
  catch (error) { toast(error.message, 'error'); }
  showLoggedOut();
  setStatus(elements.authStatus, '已安全退出。', 'success');
});

document.querySelectorAll('[data-close-dialog]').forEach(button => {
  button.addEventListener('click', () => button.closest('dialog').close());
});
document.querySelectorAll('dialog').forEach(dialog => {
  dialog.addEventListener('click', event => {
    if (event.target === dialog) dialog.close();
  });
});

// HttpOnly Cookie 由浏览器保管，页面刷新后通过 /api/me 恢复登录和 CSRF Token。
(async function restoreSession() {
  try {
    const result = await api('/api/me');
    state.csrf = result.csrfToken;
    await showChat(result.user);
  } catch (error) {
    if (error.status && error.status !== 401) setStatus(elements.authStatus, error.message, 'error');
  }
}());
