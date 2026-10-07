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
  groupProfileDialog: $('#group-profile-dialog'),
  knowledgeDialog: $('#knowledge-dialog'),
  citationDialog: $('#citation-dialog'),
  modelDialog: $('#model-dialog'),
  detailPane: $('#detail-pane'),
};

const state = {
  me: null,
  csrf: '',
  conversation: null,
  conversations: [],
  friends: [],
  blocked: [],
  socket: null,
  events: null,
  reconnectTimer: null,
  activeGenerations: new Map(),
  unread: new Map(),
  seenMessageIds: new Set(),
  pendingRecalls: new Map(),
  members: [],
  aiConversations: new Set(),
  conversationFilter: 'all',
  conversationQuery: '',
  selectedFile: null,
  uploadingFile: false,
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

/**
 * 记录已经处理过的持久化消息，阻止 WebSocket 与 SSE 的同一事件重复渲染。
 * Set 像一个临时“验票册”，只保留最近 4096 张票，避免页面长期打开后无限占用内存。
 * 返回 true 表示该消息此前已经处理过。
 */
function rememberMessageId(messageId) {
  if (messageId === undefined || messageId === null) return false;
  const key = String(messageId);
  if (state.seenMessageIds.has(key)) return true;
  state.seenMessageIds.add(key);
  if (state.seenMessageIds.size > 4096) {
    state.seenMessageIds.delete(state.seenMessageIds.values().next().value);
  }
  return false;
}

function advanceConversationSequence(conversationId, sequence) {
  const conversation = state.conversations.find(
    item => Number(item.id) === Number(conversationId),
  );
  if (!conversation) return;
  conversation.nextSequence = Math.max(
    Number(conversation.nextSequence) || 1,
    Number(sequence || 0) + 1,
  );
}

function conversationTitle(conversation) {
  return conversation.title || (conversation.kind === 'group'
    ? `群聊 #${conversation.id}`
    : `私聊 #${conversation.id}`);
}

function myMembership() {
  return state.members.find(member => Number(member.userId) === Number(state.me?.id));
}

function clearConversationView() {
  state.conversation = null;
  state.members = [];
  $('#conversation-title').textContent = '请选择会话';
  $('#conversation-subtitle').textContent = '选择左侧会话，开始交流';
  $('#conversation-avatar').textContent = '✦';
  $('#detail-title').textContent = '尚未选择会话';
  $('#detail-kind').textContent = '选择会话后显示详情';
  $('#detail-avatar').textContent = '✦';
  $('#search-messages').disabled = true;
  $('#add-member').classList.add('hidden');
  $('#group-governance').classList.add('hidden');
  elements.messageList.classList.add('hidden');
  elements.messageForm.classList.add('hidden');
  elements.emptyConversation.classList.remove('hidden');
  renderConversationMembers();
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

function declaredMediaType(file) {
  if (file.type) return file.type;
  const extension = file.name.toLowerCase().split('.').pop();
  return ({
    md: 'text/markdown', txt: 'text/plain', json: 'application/json',
    zip: 'application/zip', bin: 'application/octet-stream',
    docx: 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
    xlsx: 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
    pptx: 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
  })[extension] || 'application/octet-stream';
}

async function uploadFile(file, onProgress = () => {}) {
  const created = await api('/api/files/uploads', {
    method: 'POST',
    body: JSON.stringify({
      name: file.name,
      mediaType: declaredMediaType(file),
      size: String(file.size),
    }),
  });
  const chunkSize = 256 * 1024;
  let offset = Number(created.uploadedSize || 0);
  while (offset < file.size) {
    const chunk = file.slice(offset, Math.min(offset + chunkSize, file.size));
    let response;
    try {
      response = await fetch(`/api/files/${created.id}/content`, {
        method: 'PATCH',
        credentials: 'same-origin',
        headers: {
          'Content-Type': 'application/octet-stream',
          'X-CSRF-Token': state.csrf,
          'Upload-Offset': String(offset),
        },
        body: chunk,
      });
    } catch {
      throw new Error('文件分片上传失败，请检查网络后重试');
    }
    const text = await response.text();
    let result = {};
    try { result = text ? JSON.parse(text) : {}; } catch { result = { message: text }; }
    if (!response.ok) throw new Error(result.message || `上传失败（HTTP ${response.status}）`);
    const next = Number(result.uploadedSize);
    if (!Number.isSafeInteger(next) || next <= offset)
      throw new Error('服务器返回了无效的上传游标');
    offset = next;
    onProgress(offset / file.size);
  }
  return api(`/api/files/${created.id}/complete`, { method: 'POST' });
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

function updateProfileView() {
  if (!state.me) return;
  const name = state.me.displayName || state.me.username;
  $('#profile-name').textContent = name;
  $('#profile-handle').textContent = `@${state.me.username} · ID ${state.me.id}`;
  paintAvatar($('#profile-avatar'), state.me, initials(name));
  paintAvatar($('#profile-dialog-avatar'), state.me, initials(name));
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
  state.friends = [];
  state.blocked = [];
  state.members = [];
  state.unread.clear();
  state.seenMessageIds.clear();
  state.pendingRecalls.clear();
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
    state.unread.clear();
    state.conversations.forEach(conversation => {
      const unread = Math.max(0, Number(conversation.unreadCount) || 0);
      if (unread && Number(conversation.id) !== Number(state.conversation?.id)) {
        state.unread.set(Number(conversation.id), unread);
      }
    });
    if (state.conversation) {
      const current = state.conversations.find(
        item => Number(item.id) === Number(state.conversation.id),
      );
      if (current) state.conversation = current;
      else clearConversationView();
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
  $('#group-governance').classList.toggle('hidden', conversation.kind !== 'group');
  $('#edit-group-profile').classList.add('hidden');
  $('#leave-group').classList.add('hidden');
  $('#dissolve-group').classList.add('hidden');
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
      'hidden', conversation.kind !== 'group' || !['owner', 'administrator'].includes(me?.role),
    );
    renderGroupGovernance();
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
  paintAvatar(avatar, Number(member.userId) === Number(state.me?.id) ? state.me : member,
    Number(member.userId) === Number(state.me?.id)
      ? '我' : (member.aiAccount ? 'AI' : initials(member.displayName || member.username, `#${member.userId}`)));
    const copy = document.createElement('span');
    copy.className = 'member-copy';
    const name = document.createElement('strong');
    name.textContent = Number(member.userId) === Number(state.me?.id)
      ? (state.me.displayName || state.me.username)
      : `${member.displayName || member.username || `用户 ${member.userId}`}${member.aiAccount ? ' · AI' : ''}`;
    const progress = document.createElement('small');
    const muted = Number(member.mutedUntil) > Date.now();
    progress.textContent = muted
      ? `禁言至 ${new Date(Number(member.mutedUntil)).toLocaleString()}`
      : `已读至 #${member.lastReadSequence || 0}`;
    copy.append(name, progress);
    const role = document.createElement('span');
    role.className = `member-role ${member.role === 'owner' ? 'owner' : member.role === 'administrator' ? 'admin' : ''}`;
    role.textContent = member.role === 'owner' ? '群主'
      : member.role === 'administrator' ? '管理员' : '成员';
    item.append(avatar, copy, role);

    const mine = myMembership();
    const isSelf = Number(member.userId) === Number(state.me?.id);
    const ownerCanAct = mine?.role === 'owner' && member.role !== 'owner' && !isSelf;
    const adminCanAct = mine?.role === 'administrator' && member.role === 'member' && !isSelf;
    if (ownerCanAct || adminCanAct) {
      const actions = document.createElement('span');
      actions.className = 'member-actions';
      if (ownerCanAct) {
        const roleButton = document.createElement('button');
        roleButton.type = 'button';
        roleButton.className = 'mini-button';
        roleButton.textContent = member.role === 'administrator' ? '取消管理员' : '设为管理员';
        roleButton.addEventListener('click', () => updateMemberRole(
          member.userId,
          member.role === 'administrator' ? 'member' : 'administrator',
        ));
        actions.append(roleButton);
      }
      const muteButton = document.createElement('button');
      muteButton.type = 'button';
      muteButton.className = 'mini-button';
      muteButton.textContent = muted ? '解除禁言' : '禁言 10 分钟';
      muteButton.addEventListener('click', () => updateMemberMute(
        member.userId, muted ? 0 : 600,
      ));
      const removeButton = document.createElement('button');
      removeButton.type = 'button';
      removeButton.className = 'mini-button danger';
      removeButton.textContent = '移出群聊';
      removeButton.addEventListener('click', () => removeMember(member));
      actions.append(muteButton, removeButton);
      item.append(actions);
    }
    return item;
  }) : [actionListEmpty('暂无成员信息')]));
}

function renderGroupGovernance() {
  const group = state.conversation?.kind === 'group';
  $('#group-governance').classList.toggle('hidden', !group);
  if (!group) return;
  const mine = myMembership();
  const privileged = ['owner', 'administrator'].includes(mine?.role);
  $('#group-announcement').textContent = state.conversation.announcement || '暂无群公告';
  $('#edit-group-profile').classList.toggle('hidden', !privileged);
  $('#leave-group').classList.toggle('hidden', !mine || mine.role === 'owner');
  $('#dissolve-group').classList.toggle('hidden', mine?.role !== 'owner');
}

async function refreshCurrentGroup(message) {
  toast(message, 'success');
  const currentId = Number(state.conversation?.id);
  await loadConversations();
  const current = state.conversations.find(item => Number(item.id) === currentId);
  if (current) await openConversation(current);
}

async function updateMemberRole(userId, role) {
  if (!state.conversation) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/members/${encodeURIComponent(userId)}/role`, {
      method: 'POST', body: JSON.stringify({ role }),
    });
    await refreshCurrentGroup(role === 'administrator' ? '已设置管理员' : '已取消管理员');
  } catch (error) { toast(error.message, 'error'); }
}

async function updateMemberMute(userId, seconds) {
  if (!state.conversation) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/members/${encodeURIComponent(userId)}/mute`, {
      method: 'POST', body: JSON.stringify({ seconds }),
    });
    await refreshCurrentGroup(seconds ? '成员已禁言 10 分钟' : '成员禁言已解除');
  } catch (error) { toast(error.message, 'error'); }
}

async function removeMember(member) {
  if (!state.conversation || !confirm(`确定将“${member.displayName || member.username}”移出群聊吗？`)) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/members/${encodeURIComponent(member.userId)}/remove`, {
      method: 'POST', body: '{}',
    });
    await refreshCurrentGroup('成员已移出群聊');
  } catch (error) { toast(error.message, 'error'); }
}

function formatMessageTime(value) {
  const date = Number(value) > 0 ? new Date(Number(value)) : null;
  if (!date || Number.isNaN(date.getTime())) return '';
  return date.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
}

function deliveryLabel(message) {
  if (message.state === 'recalled') return '已撤回';
  if (message.deliveryState === 'sending') return '发送中…';
  if (message.deliveryState === 'failed') return '发送失败';
  if (Number(message.senderId) !== Number(state.me?.id) || !message.sequence) return '';
  const peers = state.members.filter(member => Number(member.userId) !== Number(state.me?.id));
  if (!peers.length) return '已保存';
  const read = peers.filter(member => Number(member.lastReadSequence) >= Number(message.sequence)).length;
  const delivered = peers.filter(member => Number(member.lastDeliveredSequence) >= Number(message.sequence)).length;
  if (read === peers.length) return peers.length === 1 ? '已读' : `全部已读 ${read}/${peers.length}`;
  if (read) return `已读 ${read}/${peers.length}`;
  if (delivered === peers.length) return peers.length === 1 ? '已送达' : `全部送达 ${delivered}/${peers.length}`;
  if (delivered) return `已送达 ${delivered}/${peers.length}`;
  return '已保存';
}

function canRecall(message) {
  if (message.state === 'recalled' || !message.id ||
      Number(message.senderId) !== Number(state.me?.id)) return false;
  const createdAt = Number(message.createdAt);
  return createdAt > 0 && Date.now() - createdAt <= 2 * 60 * 1000;
}

function requestRecall(message, button) {
  const commandId = createClientId();
  const sent = sendSocket({
    v: 1,
    type: 'chat.recall',
    id: commandId,
    conversationId: Number(message.conversationId),
    replyTo: String(message.id),
  });
  if (!sent) {
    setStatus(elements.chatStatus, '消息撤回失败：实时连接当前不可写。', 'error');
    return;
  }
  state.pendingRecalls.set(commandId, String(message.id));
  button.disabled = true;
  button.textContent = '撤回中…';
}

function createMessageElement(message, options = {}) {
  const mine = Number(message.senderId) === Number(state.me?.id);
  const aiMessage = Boolean(options.ai || message.clientMessageId?.startsWith('ai:'));
  const generationId = options.generationId ||
    (message.clientMessageId?.startsWith('ai:') ? message.clientMessageId.slice(3) : '');
  const item = document.createElement('li');
  item.className = 'message';
  item.classList.toggle('mine', mine);
  item.classList.toggle('ai-streaming', Boolean(options.streaming));
  item.classList.toggle('message-recalled', message.state === 'recalled');
  if (message.id !== undefined) item.dataset.messageId = String(message.id);
  if (message.clientMessageId) item.dataset.clientMessageId = message.clientMessageId;
  if (generationId) item.dataset.generationId = generationId;
  if (message.sequence !== undefined) item.dataset.sequence = String(message.sequence);

  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  const sender = mine ? state.me : state.members.find(
    member => Number(member.userId) === Number(message.senderId));
  paintAvatar(avatar, sender, mine ? initials(state.me?.displayName || state.me?.username)
    : (aiMessage ? 'AI' : `#${message.senderId || '?'}`));
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
  bubble.classList.toggle('recalled', message.state === 'recalled');
  bubble.textContent = message.state === 'recalled' ? '这条消息已被撤回' : (message.body || '');
  if (!bubble.textContent && message.attachments?.length) bubble.classList.add('hidden');
  const attachments = document.createElement('div');
  attachments.className = 'message-attachments';
  (message.state === 'recalled' ? [] : (message.attachments || [])).forEach(file => {
    const card = file.url && !file.pending ? document.createElement('a') : document.createElement('div');
    card.className = 'attachment-card';
    if (file.url && !file.pending) {
      card.href = file.url;
      card.target = '_blank';
      card.rel = 'noopener';
    }
    if (String(file.mediaType || '').startsWith('image/') && file.url && !file.pending) {
      const preview = document.createElement('img');
      preview.src = file.url;
      preview.alt = file.name || '聊天图片';
      preview.loading = 'lazy';
      card.append(preview);
    }
    const icon = document.createElement('span');
    icon.className = 'attachment-icon';
    icon.textContent = file.pending ? 'UP' : String(file.mediaType || '').startsWith('image/') ? 'IMG' : 'FILE';
    const copy = document.createElement('span');
    const title = document.createElement('strong');
    title.textContent = file.name || '附件';
    const size = document.createElement('small');
    size.textContent = file.pending ? '正在分片上传…' : `${formatBytes(file.size)} · 点击下载`;
    copy.append(title, size);
    card.append(icon, copy);
    attachments.append(card);
  });
  const footer = document.createElement('div');
  footer.className = 'message-footer';
  const status = document.createElement('span');
  status.className = `message-delivery ${message.deliveryState || ''}`.trim();
  status.textContent = deliveryLabel(message);
  footer.append(status);
  if (aiMessage && generationId && !options.streaming && message.state !== 'recalled') {
    const sources = document.createElement('button');
    sources.type = 'button';
    sources.className = 'message-citations';
    sources.textContent = '查看引用';
    sources.addEventListener('click', () => openCitationDetails(generationId));
    footer.append(sources);
  }
  if (canRecall(message)) {
    const recall = document.createElement('button');
    recall.type = 'button';
    recall.className = 'message-recall';
    recall.textContent = '撤回';
    recall.addEventListener('click', () => requestRecall(message, recall));
    footer.append(recall);
  }
  content.append(meta, bubble);
  if (attachments.childElementCount) content.append(attachments);
  content.append(footer);
  item.append(avatar, content);
  return item;
}

function renderMessages(messages) {
  messages.forEach(message => {
    rememberMessageId(message.id);
  });
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

async function openCitationDetails(generationId) {
  $('#citation-list').replaceChildren(actionListEmpty('正在读取引用…'));
  setStatus($('#citation-status'));
  elements.citationDialog.showModal();
  try {
    const citations = await api(
      `/api/ai/generations/${encodeURIComponent(generationId)}/citations`,
    );
    const rows = citations.map(citation => {
      const item = document.createElement('li');
      const title = document.createElement('strong');
      title.textContent = `[资料${citation.rank}] ${citation.documentTitle}`;
      const source = document.createElement('small');
      source.textContent = `${citation.sourceName} · 片段 ${Number(citation.chunkOrdinal) + 1} · 相关度 ${Number(citation.score).toFixed(3)}`;
      const content = document.createElement('p');
      content.textContent = citation.content;
      item.append(title, source, content);
      return item;
    });
    $('#citation-list').replaceChildren(...(rows.length
      ? rows : [actionListEmpty('本次回答没有检索到可引用资料。')]));
    setStatus($('#citation-status'), `共 ${rows.length} 条可追溯引用`, 'success');
  } catch (error) {
    $('#citation-list').replaceChildren(actionListEmpty('引用读取失败'));
    setStatus($('#citation-status'), error.message, 'error');
  }
}

function existingMessage(messageId) {
  return [...elements.messageList.children].find(item => item.dataset.messageId === String(messageId));
}

function messageByClientId(clientMessageId) {
  return [...elements.messageList.children]
    .find(item => item.dataset.clientMessageId === String(clientMessageId));
}

function refreshMessageReceipts() {
  elements.messageList.querySelectorAll('.message.mine').forEach(item => {
    const status = item.querySelector('.message-delivery');
    if (!status || !item.dataset.sequence) return;
    status.textContent = deliveryLabel({
      senderId: state.me?.id,
      sequence: Number(item.dataset.sequence),
      state: item.classList.contains('message-recalled') ? 'recalled' : 'active',
    });
  });
}

function generationElement(generationId) {
  return [...elements.messageList.children].find(item => item.dataset.generationId === String(generationId));
}

function appendMessage(message) {
  if (!message) return;
  const pending = message.clientMessageId && messageByClientId(message.clientMessageId);
  if (pending) {
    pending.replaceWith(createMessageElement(message));
    rememberMessageId(message.id);
    scrollMessages();
    return;
  }
  if (existingMessage(message.id)) return;
  if (message.clientMessageId?.startsWith('ai:')) {
    generationElement(message.clientMessageId.slice(3))?.remove();
  }
  elements.messageList.querySelector('.list-empty')?.remove();
  elements.messageList.append(createMessageElement(message));
  rememberMessageId(message.id);
  scrollMessages();
}

function acceptPendingMessage(message) {
  const pending = messageByClientId(message.replyTo);
  if (!pending) return;
  // Outbox 广播可能比命令回执先到。若 DOM 已经是同一条持久消息，就保留广播携带的
  // 准确 createdAt/state，不再用回执中的精简字段覆盖它。
  if (pending.dataset.messageId === String(message.id)) return;
  const body = pending.querySelector('.message-bubble')?.textContent || '';
  const accepted = {
    id: message.id,
    conversationId: message.conversationId,
    sequence: message.sequence,
    senderId: state.me?.id,
    clientMessageId: message.replyTo,
    body,
    // chat.accepted 只证明已经落库，不携带服务端时间。先不显示依赖时间窗的撤回按钮，
    // 随后的 chat.message / SSE 持久事件会用完整记录替换这个临时状态。
    createdAt: 0,
    state: 'active',
  };
  pending.replaceWith(createMessageElement(accepted));
  advanceConversationSequence(message.conversationId, message.sequence);
  renderConversations();
}

function applyRecall(message) {
  const existing = existingMessage(message.id);
  if (existing) existing.replaceWith(createMessageElement(message));
}

function failPendingMessage(clientMessageId) {
  const pending = messageByClientId(clientMessageId);
  if (!pending) return;
  pending.classList.add('send-failed');
  const status = pending.querySelector('.message-delivery');
  if (status) {
    status.textContent = '发送失败';
    status.classList.add('failed');
  }
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
    if (rememberMessageId(message.id)) return;
    advanceConversationSequence(message.conversationId, message.sequence);
    const isCurrent = Number(message.conversationId) === Number(state.conversation?.id);
    if (isCurrent) {
      appendMessage(message);
      acknowledge(message);
    } else {
      const id = Number(message.conversationId);
      // 同一账号的另一浏览器标签页可能收到自己刚发送的广播；发送者游标已经在
      // 服务端推进，因此这里只给真正来自其他成员的消息增加本地未读数。
      if (Number(message.senderId) !== Number(state.me?.id)) {
        state.unread.set(id, (state.unread.get(id) || 0) + 1);
      }
      renderConversations();
    }
    return;
  }
  if (message.type === 'chat.message-recalled') {
    if (Number(message.conversationId) === Number(state.conversation?.id)) {
      applyRecall(message);
      setStatus(elements.chatStatus, '消息已撤回。', 'success');
    }
    return;
  }
  if (message.type === 'chat.receipt') {
    if (Number(message.conversationId) === Number(state.conversation?.id)) {
      const member = state.members.find(item => Number(item.userId) === Number(message.userId));
      if (member) {
        if (message.state === 'read') {
          member.lastReadSequence = Math.max(Number(member.lastReadSequence) || 0, Number(message.sequence));
          member.lastDeliveredSequence = Math.max(Number(member.lastDeliveredSequence) || 0, Number(message.sequence));
        } else {
          member.lastDeliveredSequence = Math.max(Number(member.lastDeliveredSequence) || 0, Number(message.sequence));
        }
        renderConversationMembers();
        refreshMessageReceipts();
      }
    }
    if (message.state === 'read' && Number(message.userId) === Number(state.me?.id)) {
      const conversation = state.conversations.find(item => Number(item.id) === Number(message.conversationId));
      const latest = Math.max(0, Number(conversation?.nextSequence || 1) - 1);
      const remaining = Math.max(0, latest - Number(message.sequence));
      if (remaining) state.unread.set(Number(message.conversationId), remaining);
      else state.unread.delete(Number(message.conversationId));
      renderConversations();
    }
    return;
  }
  if (message.type === 'conversation.updated') {
    const currentId = Number(state.conversation?.id);
    const affected = Number(message.conversationId) === currentId;
    const removedSelf = Number(message.userId) === Number(state.me?.id)
      && ['member-removed', 'member-left'].includes(message.action);
    loadConversations().then(async () => {
      const current = state.conversations.find(item => Number(item.id) === currentId);
      if (affected && current && !removedSelf) await openConversation(current);
      if (affected && (!current || removedSelf || message.action === 'dissolved')) {
        clearConversationView();
      }
    }).catch(() => {});
    const labels = {
      updated: '群资料已更新',
      'member-added': '群成员已更新',
      'member-removed': '有成员被移出群聊',
      'member-left': '有成员退出群聊',
      'member-role-changed': '群成员角色已更新',
      'member-muted': '群成员禁言状态已更新',
      dissolved: '群聊已解散',
    };
    toast(labels[message.action] || '群聊信息已更新');
    return;
  }
  if (message.type === 'presence.update') {
    const contact = state.friends.find(
      item => Number(item.id) === Number(message.userId),
    );
    if (contact) {
      contact.online = Boolean(message.online);
      contact.connections = Number(message.connections) || 0;
      contact.lastActiveAt = Number(message.lastActiveAt) || contact.lastActiveAt;
      renderFriendPreview();
      if (elements.friendDialog.open) renderContactLists();
    }
    return;
  }
  if (message.type === 'friend.updated') {
    loadFriendsPreview().catch(() => {});
    refreshPendingBadge().catch(() => {});
    if (elements.friendDialog.open) loadContacts().catch(() => {});
    const labels = {
      accepted: '好友申请已通过', rejected: '好友申请已处理',
      removed: '好友关系已解除', updated: '联系人设置已更新',
      blocked: '用户已加入黑名单', unblocked: '用户已移出黑名单',
    };
    toast(labels[message.action] || '联系人信息已更新');
    return;
  }
  if (message.type === 'chat.accepted') {
    acceptPendingMessage(message);
    acknowledge(message);
    const text = message.status === 'duplicate'
      ? '重复消息已由服务器安全去重。'
      : '消息已由服务器持久化；接收方离线也可以稍后恢复。';
    setStatus(elements.chatStatus, text, 'success');
  } else if (message.type === 'chat.error') {
    failPendingMessage(message.replyTo);
    const recalledMessage = state.pendingRecalls.get(message.replyTo);
    if (recalledMessage) {
      state.pendingRecalls.delete(message.replyTo);
      const recall = existingMessage(recalledMessage)?.querySelector('.message-recall');
      if (recall) {
        recall.disabled = false;
        recall.textContent = '撤回';
      }
    }
    setStatus(elements.chatStatus, message.status || '消息发送失败', 'error');
  } else if (message.type === 'chat.recall.accepted') {
    state.pendingRecalls.delete(message.replyTo);
    setStatus(elements.chatStatus,
      message.status === 'already_recalled' ? '该消息已经撤回。' : '撤回请求已提交。',
      'success');
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
  events.addEventListener('message.recalled', event => {
    try { onRealtimeMessage(JSON.parse(event.data)); } catch { /* WebSocket or history can recover it. */ }
  });
  events.addEventListener('message.receipt', event => {
    try { onRealtimeMessage(JSON.parse(event.data)); } catch { /* Receipt cursors remain persisted. */ }
  });
  events.addEventListener('conversation.invited', async event => {
    try {
      const notice = JSON.parse(event.data);
      toast(`你已加入会话 #${notice.conversationId}`);
      await loadConversations();
    } catch { /* A later refresh can recover the conversation list. */ }
  });
  events.addEventListener('conversation.updated', event => {
    try { onRealtimeMessage(JSON.parse(event.data)); } catch { /* 刷新会话列表可恢复状态。 */ }
  });
  events.addEventListener('friend.request', async () => {
    toast('收到新的好友申请');
    await refreshPendingBadge();
    if (elements.friendDialog.open) await loadContacts();
  });
  events.addEventListener('friend.updated', event => {
    try { onRealtimeMessage(JSON.parse(event.data)); } catch { /* HTTP 刷新可恢复。 */ }
  });
  events.addEventListener('presence.update', event => {
    try { onRealtimeMessage(JSON.parse(event.data)); } catch { /* 下次联系人刷新可恢复。 */ }
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

function contactName(user) {
  return user.remark || user.displayName || user.username;
}

function contactPresence(user) {
  if (user.online) {
    const devices = Math.max(1, Number(user.connections) || 1);
    return `在线 · ${devices} 个连接`;
  }
  if (user.lastActiveAt) {
    return `最后活跃 ${new Date(Number(user.lastActiveAt)).toLocaleString()}`;
  }
  return '离线 · 暂无活跃记录';
}

async function saveContact(user, changes) {
  await api(`/api/friends/${encodeURIComponent(user.id)}`, {
    method: 'PATCH',
    body: JSON.stringify({
      remark: changes.remark ?? user.remark ?? '',
      notificationsMuted: String(
        changes.notificationsMuted ?? Boolean(user.notificationsMuted),
      ),
    }),
  });
  await loadContacts();
}

function contactRow(user) {
  const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  paintAvatar(avatar, user, initials(user.displayName || user.username));
  const copy = document.createElement('span');
  copy.className = 'item-copy';
  const name = document.createElement('strong');
  name.textContent = contactName(user);
  const detail = document.createElement('small');
  detail.className = `contact-presence ${user.online ? 'online' : ''}`;
  detail.textContent = contactPresence(user);
  const biography = document.createElement('small');
  biography.className = 'contact-biography';
  biography.textContent = `@${user.username} · ID ${user.id}${user.notificationsMuted ? ' · 已免打扰' : ''}`;
  copy.append(name, detail, biography);
  const actions = document.createElement('span');
  actions.className = 'mini-actions contact-actions';
  const chat = document.createElement('button');
  chat.type = 'button';
  chat.className = 'mini-button';
  chat.textContent = '私聊';
  chat.addEventListener('click', () => openFriendConversation(user, chat));
  const remark = document.createElement('button');
  remark.type = 'button';
  remark.className = 'mini-button';
  remark.textContent = '备注';
  remark.addEventListener('click', async () => {
    const value = window.prompt('设置仅自己可见的好友备注（留空可清除）', user.remark || '');
    if (value === null) return;
    try { await saveContact(user, { remark: value.trim() }); }
    catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  const mute = document.createElement('button');
  mute.type = 'button';
  mute.className = 'mini-button';
  mute.textContent = user.notificationsMuted ? '取消免打扰' : '免打扰';
  mute.addEventListener('click', async () => {
    try { await saveContact(user, { notificationsMuted: !user.notificationsMuted }); }
    catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  const remove = document.createElement('button');
  remove.type = 'button';
  remove.className = 'mini-button danger';
  remove.textContent = '删除';
  remove.addEventListener('click', async () => {
    if (!window.confirm(`确定删除联系人“${contactName(user)}”吗？历史消息会保留。`)) return;
    try {
      await api(`/api/friends/${encodeURIComponent(user.id)}`, { method: 'DELETE' });
      await loadContacts();
    } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  const block = document.createElement('button');
  block.type = 'button';
  block.className = 'mini-button danger';
  block.textContent = '拉黑';
  block.addEventListener('click', async () => {
    if (!window.confirm(`拉黑“${contactName(user)}”会同时解除好友关系，是否继续？`)) return;
    try {
      await api(`/api/blocks/${encodeURIComponent(user.id)}`, { method: 'POST' });
      await loadContacts();
    } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  actions.append(chat, remark, mute, remove, block);
  item.append(avatar, copy, actions);
  return item;
}

function sidebarFriendRow(user) {
  const item = document.createElement('li');
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'sidebar-friend';
  button.title = `给 ${contactName(user)} 发送消息`;

  const avatar = document.createElement('span');
  avatar.className = 'avatar avatar-friend';
  paintAvatar(avatar, user, initials(user.displayName || user.username));
  const copy = document.createElement('span');
  copy.className = 'friend-copy';
  const name = document.createElement('strong');
  name.textContent = contactName(user);
  const detail = document.createElement('small');
  detail.className = `contact-presence ${user.online ? 'online' : ''}`;
  detail.textContent = contactPresence(user);
  const biography = document.createElement('small');
  biography.className = 'friend-biography';
  biography.textContent = `@${user.username}${user.notificationsMuted ? ' · 免打扰' : ''}`;
  const action = document.createElement('span');
  action.className = 'friend-chat-action';
  action.textContent = '私聊';
  copy.append(name, detail, biography);
  button.append(avatar, copy, action);
  button.addEventListener('click', () => openFriendConversation(user, button));
  item.append(button);
  return item;
}

function blockedRow(user) {
  const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  paintAvatar(avatar, user, initials(user.displayName || user.username));
  const copy = document.createElement('span');
  copy.className = 'item-copy';
  const name = document.createElement('strong');
  name.textContent = user.displayName || user.username;
  const detail = document.createElement('small');
  detail.textContent = `@${user.username} · ID ${user.id}`;
  copy.append(name, detail);
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'mini-button';
  button.textContent = '解除';
  button.addEventListener('click', async () => {
    try {
      await api(`/api/blocks/${encodeURIComponent(user.id)}`, { method: 'DELETE' });
      await loadContacts();
    } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  item.append(avatar, copy, button);
  return item;
}

function renderContactLists() {
  $('#friend-count').textContent = String(state.friends.length);
  $('#blocked-count').textContent = String(state.blocked.length);
  $('#friend-list').replaceChildren(...(state.friends.length
    ? state.friends.map(contactRow) : [actionListEmpty('还没有好友')]));
  $('#blocked-list').replaceChildren(...(state.blocked.length
    ? state.blocked.map(blockedRow) : [actionListEmpty('黑名单为空')]));
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
    const [friends, requests, blocked] = await Promise.all([
      api('/api/friends'), api('/api/friend-requests'), api('/api/blocks'),
    ]);
    state.friends = friends;
    state.blocked = blocked;
    renderFriendPreview();
    $('#request-count').textContent = String(requests.length);
    renderContactLists();
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

function showSelectedFile(file, progress = null) {
  state.selectedFile = file || null;
  $('#selected-file').classList.toggle('hidden', !file);
  $('#selected-file-name').textContent = file ? `${file.name} · ${formatBytes(file.size)}` : '';
  $('#selected-file-progress').textContent = progress === null ? '' : `${Math.round(progress * 100)}%`;
}

$('#message-file').addEventListener('change', event => {
  const file = event.currentTarget.files?.[0] || null;
  if (file && file.size > 25 * 1024 * 1024) {
    toast('文件不能超过 25 MiB', 'error');
    event.currentTarget.value = '';
    showSelectedFile(null);
    return;
  }
  showSelectedFile(file);
});

$('#clear-selected-file').addEventListener('click', () => {
  $('#message-file').value = '';
  showSelectedFile(null);
});

elements.messageForm.addEventListener('submit', async event => {
  event.preventDefault();
  if (!state.conversation || state.uploadingFile) return;
  const form = event.currentTarget;
  const input = form.elements.message;
  const content = input.value.trim();
  const selectedFile = state.selectedFile;
  if (!content && !selectedFile) return;
  const aiMode = $('#toggle-ai').getAttribute('aria-pressed') === 'true';
  const aiUserId = form.elements.aiUserId.value.trim();
  if (selectedFile && aiMode) {
    setStatus(elements.chatStatus, 'AI 模式暂只接收文字提示词；请先关闭 AI 模式再发送文件。', 'error');
    return;
  }
  if (aiMode && !aiUserId) {
    setStatus(elements.chatStatus, 'AI 模式需要填写目标 AI 账号 ID。', 'error');
    form.elements.aiUserId.focus();
    return;
  }
  const messageId = createClientId();
  if (selectedFile) {
    const sendButton = form.querySelector('.send-button');
    state.uploadingFile = true;
    setButtonBusy(sendButton, true, '上传 0%');
    appendMessage({
      conversationId: Number(state.conversation.id),
      senderId: Number(state.me.id),
      clientMessageId: messageId,
      body: content,
      attachments: [{ name: selectedFile.name, size: selectedFile.size,
        mediaType: declaredMediaType(selectedFile), pending: true }],
      createdAt: Date.now(),
      state: 'active',
      deliveryState: 'sending',
    });
    try {
      const uploaded = await uploadFile(selectedFile, progress => {
        showSelectedFile(selectedFile, progress);
        sendButton.textContent = `上传 ${Math.round(progress * 100)}%`;
      });
      const result = await api(`/api/conversations/${state.conversation.id}/file-messages`, {
        method: 'POST',
        body: JSON.stringify({ fileId: String(uploaded.id),
          clientMessageId: messageId, caption: content }),
      });
      appendMessage(result.message);
      setStatus(elements.chatStatus, '文件已保存并作为消息发送。', 'success');
      input.value = '';
      input.style.height = '';
      $('#message-file').value = '';
      showSelectedFile(null);
    } catch (error) {
      failPendingMessage(messageId);
      setStatus(elements.chatStatus, error.message, 'error');
    } finally {
      state.uploadingFile = false;
      setButtonBusy(sendButton, false);
    }
    return;
  }
  if (state.socket?.readyState !== WebSocket.OPEN) {
    setStatus(elements.chatStatus, 'WebSocket 正在重连，暂时无法发送文字消息。', 'error');
    return;
  }
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
    appendMessage({
      conversationId: Number(state.conversation.id),
      senderId: Number(state.me.id),
      clientMessageId: messageId,
      body: content,
      createdAt: Date.now(),
      state: 'active',
      deliveryState: 'sending',
    });
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

function knowledgeScope() {
  const form = $('#knowledge-form');
  return form.elements.scope.value === 'conversation' && state.conversation
    ? state.conversation.id : null;
}

function renderKnowledgeDocuments(documents) {
  const rows = documents.map(document => {
    const item = document.createElement('li');
    const icon = document.createElement('span');
    icon.className = 'knowledge-file-icon';
    icon.textContent = document.mediaType === 'text/markdown' ? 'MD' : 'TXT';
    const copy = document.createElement('span');
    copy.className = 'item-copy';
    const title = document.createElement('strong');
    title.textContent = document.title;
    const meta = document.createElement('small');
    meta.textContent = `${document.sourceName} · ${document.scope === 'private' ? '私人' : '会话共享'} · ${document.state}`;
    copy.append(title, meta);
    const checksum = document.createElement('code');
    checksum.className = 'knowledge-checksum';
    checksum.textContent = document.sha256.slice(0, 10);
    checksum.title = document.sha256;
    item.append(icon, copy, checksum);
    return item;
  });
  $('#knowledge-list').replaceChildren(...(rows.length
    ? rows : [actionListEmpty('当前范围还没有可用资料。')]));
  $('#knowledge-count').textContent = String(rows.length);
}

async function loadKnowledgeDocuments() {
  const conversationId = knowledgeScope();
  const query = conversationId
    ? `?conversationId=${encodeURIComponent(conversationId)}` : '';
  $('#knowledge-list').replaceChildren(actionListEmpty('正在加载资料…'));
  try {
    renderKnowledgeDocuments(await api(`/api/knowledge/documents${query}`));
  } catch (error) {
    $('#knowledge-list').replaceChildren(actionListEmpty('资料加载失败'));
    setStatus($('#knowledge-status'), error.message, 'error');
  }
}

function updateKnowledgeScopeHint() {
  const form = $('#knowledge-form');
  const conversation = form.elements.scope.value === 'conversation';
  $('#knowledge-scope-hint').textContent = conversation && state.conversation
    ? `共享给“${conversationTitle(state.conversation)}”的当前成员。`
    : '私人资料只会进入本人和 AI 账号的私聊。';
}

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
$('#show-knowledge').addEventListener('click', async () => {
  const form = $('#knowledge-form');
  const conversationOption = form.elements.scope.querySelector('[value="conversation"]');
  conversationOption.disabled = !state.conversation;
  conversationOption.textContent = state.conversation
    ? `当前会话：${conversationTitle(state.conversation)}` : '当前会话（请先选择会话）';
  if (!state.conversation && form.elements.scope.value === 'conversation')
    form.elements.scope.value = 'private';
  updateKnowledgeScopeHint();
  setStatus($('#knowledge-status'));
  elements.knowledgeDialog.showModal();
  await loadKnowledgeDocuments();
});
$('#knowledge-form').elements.scope.addEventListener('change', async () => {
  updateKnowledgeScopeHint();
  await loadKnowledgeDocuments();
});
$('#knowledge-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const fields = new FormData(form);
  const content = String(fields.get('content') || '');
  if (new Blob([content]).size > 2 * 1024 * 1024) {
    setStatus($('#knowledge-status'), '资料正文不能超过 2 MiB。', 'error');
    return;
  }
  const conversationId = knowledgeScope();
  const sourceName = String(fields.get('sourceName') || '').trim();
  setButtonBusy(button, true, '正在切片并建立索引…');
  try {
    await api('/api/knowledge/documents', {
      method: 'POST',
      body: JSON.stringify({
        title: String(fields.get('title') || '').trim(),
        sourceName,
        mediaType: sourceName.toLowerCase().endsWith('.md')
          ? 'text/markdown' : 'text/plain',
        content,
        ...(conversationId ? { conversationId: String(conversationId) } : {}),
      }),
    });
    form.elements.title.value = '';
    form.elements.sourceName.value = '';
    form.elements.content.value = '';
    setStatus($('#knowledge-status'), '资料已切片、持久化并进入检索索引。', 'success');
    toast('知识资料已可用于 AI 检索', 'success');
    await loadKnowledgeDocuments();
  } catch (error) {
    setStatus($('#knowledge-status'), error.message, 'error');
  } finally {
    setButtonBusy(button, false);
  }
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
    const avatar = form.elements.avatar.files?.[0];
    if (avatar) {
      if (avatar.size > 10 * 1024 * 1024) throw new Error('头像不能超过 10 MiB');
      setButtonBusy(button, true, '上传头像 0%');
      const uploaded = await uploadFile(avatar, progress => {
        button.textContent = `上传头像 ${Math.round(progress * 100)}%`;
      });
      await api('/api/me/avatar', {
        method: 'POST', body: JSON.stringify({ fileId: String(uploaded.id) }),
      });
    }
    const result = await api('/api/me', {
      method: 'PATCH', body: JSON.stringify({
        displayName: form.elements.displayName.value,
        biography: form.elements.biography.value,
      }),
    });
    state.me = result.user;
    form.elements.avatar.value = '';
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

$('#edit-group-profile').addEventListener('click', () => {
  if (!state.conversation) return;
  const form = $('#group-profile-form');
  form.elements.title.value = state.conversation.title || '';
  form.elements.announcement.value = state.conversation.announcement || '';
  setStatus($('#group-profile-status'));
  elements.groupProfileDialog.showModal();
});

$('#group-profile-form').addEventListener('submit', async event => {
  event.preventDefault();
  if (!state.conversation) return;
  const form = event.currentTarget;
  const values = new FormData(form);
  const button = form.querySelector('[type="submit"]');
  setButtonBusy(button, true, '保存中…');
  try {
    const updated = await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}`, {
      method: 'PATCH',
      body: JSON.stringify({
        title: values.get('title').trim(),
        announcement: values.get('announcement').trim(),
      }),
    });
    state.conversation = { ...state.conversation, ...updated };
    setStatus($('#group-profile-status'), '群资料已保存。', 'success');
    await refreshCurrentGroup('群资料已更新');
    setTimeout(() => elements.groupProfileDialog.close(), 500);
  } catch (error) { setStatus($('#group-profile-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

$('#leave-group').addEventListener('click', async () => {
  if (!state.conversation || !confirm('确定退出当前群聊吗？退出后将不能继续查看或发送消息。')) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/leave`, {
      method: 'POST', body: '{}',
    });
    clearConversationView();
    await loadConversations();
    toast('已退出群聊', 'success');
  } catch (error) { toast(error.message, 'error'); }
});

$('#dissolve-group').addEventListener('click', async () => {
  if (!state.conversation || !confirm('确定解散当前群聊吗？该操作会停止所有成员继续发送消息。')) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/dissolve`, {
      method: 'POST', body: '{}',
    });
    clearConversationView();
    await loadConversations();
    toast('群聊已解散，历史记录仍保留在数据库中', 'success');
  } catch (error) { toast(error.message, 'error'); }
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
