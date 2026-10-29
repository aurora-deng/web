import { mountNavigation } from './shared/AppNavigation/AppNavigation.js';
mountNavigation('chat');
import { BusinessApi } from './shared/BusinessApi/BusinessApi.js';
import { AnnouncementPlayer } from './shared/AnnouncementPlayer/AnnouncementPlayer.js';
import { ChatHistory, canRecallMessage } from './chat/ChatHistory.js?v=20261012';
import { ReliableSender } from './chat/ReliableSender.js?v=20261012';

import { VirtualMessageList } from './chat/VirtualMessageList/VirtualMessageList.js';
import { RecoveryBudget, ReconnectBackoff } from './shared/RecoveryBudget/RecoveryBudget.js';
// 新增：全站唯一时间格式化（会话列表 / 消息流 / 搜索结果共用同一实现）
import { formatChatTime, formatDayKey, formatDateDivider } from './shared/TimeFormat/TimeFormat.js';

const pageBudget = new RecoveryBudget();
const wsBackoff = new ReconnectBackoff();
const sseBackoff = new ReconnectBackoff();
let sseTimer = null;

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
  trainingDialog: $('#training-dialog'),
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
  viewVersion: 0,
  listVersion: 0,
  catchupTimer: null,
  recovery: null,
  recoverRequested: false,
  transientMessages: new Map(),
  searchHit: null,
  unreadPending: new Map(),   // 服务端基线请求发出之后、返回之前到达的本地未读增量
  bumped: null,               // 最近一次有新消息的会话，用于一次性上浮动效
  aiModels: null,             // null = 还没读过；[] = 读过但为空或失败
  aiModelError: '',           // 模型列表读取失败的原因（用于下拉里的兜底选项文案）
  conversationAiMember: 0,    // 当前会话自动识别到的 AI 成员 userId，0 = 没有
  aiTargetManual: false,      // 用户是否手选过模型：手选优先级高于自动识别，且不跨会话保留
};

// 跳转定位期间要连续翻十几页历史，逐页重绘会让列表抖动；
// 只在这一段屏蔽重绘，定位结束后统一渲染一次。
let suppressMessageRender = false;

const messageView = new VirtualMessageList(elements.messageList, createMessageElement);
const history = new ChatHistory((conversationId, query, signal) => pageBudget.run(() => api(
  `/api/conversations/${conversationId}/messages?${new URLSearchParams(query)}`, { signal },
), signal), () => renderCurrentMessages());

function savePendingMessages() {
  if (!state.me) return;
  try {
    // 只保留当前标签页、当前账号的未确认文字命令，不保存密码或 Session Token。
    sessionStorage.setItem(`phase11:pending:${state.me.id}`, JSON.stringify(sender.snapshot()));
  } catch { /* 存储不可用时仍可在当前页面内重试，不阻止正常发送。 */ }
}

async function verifyOwnMessage(payload) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 4000);
  try {
    return await api(`/api/conversations/${payload.conversationId}/messages/by-client-id?` +
      new URLSearchParams({ clientMessageId: payload.id }), { signal: controller.signal });
  } finally { clearTimeout(timer); }
}

const sender = new ReliableSender({
  send: payload => Boolean(state.me && !state.leaving) && sendSocket(payload),
  verify: verifyOwnMessage,
  changed: () => { savePendingMessages(); renderCurrentMessages(); },
  persisted: (entry, message) => {
    savePendingMessages();
    if (!state.me) return;
    const cached = history.records.get(String(message.id));
    const full = !message.createdAt && cached ? cached : { ...message, senderId: state.me.id };
    appendMessage(full);
    advanceConversationSequence(message.conversationId, message.sequence);
    renderConversations();
    if (Number(message.conversationId) === Number(state.conversation?.id) &&
        Number(message.sequence) > history.cursor) scheduleCatchup();
    // 精简 accepted 没有服务端时间，查询完整记录后才能正确判断撤回时间窗。
    if (!full.createdAt) verifyOwnMessage(entry.payload).then(found => {
      if (found && Number(found.senderId) === Number(state.me?.id)) appendMessage(found);
    }).catch(() => scheduleCatchup());
  },
});

function settleHistoryPending() {
  history.messages.forEach(message => {
    if (Number(message.senderId) === Number(state.me?.id)) sender.confirmMessage(message);
  });
}

function scheduleCatchup() {
  clearTimeout(state.catchupTimer);
  state.catchupTimer = setTimeout(() => recoverRealtime(), 100);
}

/** 重连不仅取新消息，也重查可见窗口，恢复断线期间发生的撤回和权限变化。 */
async function recoverRealtime() {
  state.recoverRequested = true;
  if (state.recovery) return state.recovery;
  const userId = state.me?.id;
  const recovery = (async () => {
    while (state.recoverRequested && userId && state.me?.id === userId && !state.leaving) {
      state.recoverRequested = false;
      const loaded = await loadConversations();
      if (!loaded || state.me?.id !== userId) break;
      const conversation = state.conversation;
      const version = state.viewVersion;
      if (conversation) {
        try {
          const members = await api(`/api/conversations/${conversation.id}/members`);
          if (version !== state.viewVersion || state.me?.id !== userId) continue;
          state.members = members;
          await history.synchronize(Math.max(history.latest, Number(conversation.nextSequence) - 1),
                                    { refreshLoaded: true });
          if (version !== state.viewVersion || state.me?.id !== userId) continue;
          settleHistoryPending();
          renderConversationMembers();
          renderGroupGovernance();
          renderCurrentMessages();
          acknowledge({ conversationId: conversation.id, sequence: history.cursor });
        } catch (error) { setStatus(elements.chatStatus, error.message, 'error'); }
      }
      if (state.me?.id === userId) await sender.connected();
    }
  })().finally(() => { if (state.recovery === recovery) state.recovery = null; });
  state.recovery = recovery;
  return recovery;
}

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
  return conversation.displayTitle || conversation.title || (conversation.kind === 'group'
    ? `群聊 #${conversation.id}`
    : `私聊 #${conversation.id}`);
}

/**
 * 会话列表的全序：未读优先 → 最后消息时间倒序 → id 倒序。
 * 末位用 id 兜底是为了让「同一毫秒到达的两条消息」也有确定顺序，
 * 否则每次重绘都可能交换位置，用户会看到列表闪。
 */
function conversationOrder(left, right) {
  const leftUnread = state.unread.get(Number(left.id)) || 0;
  const rightUnread = state.unread.get(Number(right.id)) || 0;
  if (Boolean(leftUnread) !== Boolean(rightUnread)) return leftUnread ? -1 : 1;
  const leftAt = Number(left.lastMessageAt) || 0;
  const rightAt = Number(right.lastMessageAt) || 0;
  if (leftAt !== rightAt) return rightAt - leftAt;
  return Number(right.id) - Number(left.id);
}

/**
 * 记一条本地未读增量。同时写进 state.unread（立刻可见）和 state.unreadPending
 * （供 loadConversations 在服务端基线之上叠加），避免请求期间的增量被 clear() 吃掉。
 */
function bumpUnread(conversationId) {
  const id = Number(conversationId);
  state.unread.set(id, (state.unread.get(id) || 0) + 1);
  state.unreadPending.set(id, (state.unreadPending.get(id) || 0) + 1);
  state.bumped = { id, at: Date.now() };
}

function myMembership() {
  return state.members.find(member => Number(member.userId) === Number(state.me?.id));
}

function clearConversationView() {
  state.viewVersion += 1;
  history.reset();
  messageView.clear();
  state.conversation = null;
  state.members = [];
  $('#conversation-title').textContent = '请选择会话';
  $('#conversation-subtitle').textContent = '选择左侧会话，开始交流';
  // 走同一个 paintAvatar，才能把上一段会话留下的背景图一起清掉。
  paintAvatar($('#conversation-avatar'), null, '✦');
  $('#detail-title').textContent = '尚未选择会话';
  $('#detail-kind').textContent = '选择会话后显示详情';
  paintAvatar($('#detail-avatar'), null, '✦');
  $('#composer-destination').textContent = '当前会话';
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

/**
 * 把消息序列映射成「带日期分隔标记的行」。
 * 不改 VirtualMessageList：日期标记作为行对象的附加字段随行一起渲染，
 * 因此 heights / prefix / range 的下标语义完全不变。
 * 只有需要分隔的行才创建新对象，其余行保持原引用，
 * 这样 VirtualMessageList 的 `JSON.stringify(message)` 签名缓存不会被无谓击穿。
 */
function withDateDividers(rows) {
  const result = [];
  let previousDay = '';
  rows.forEach((message, index) => {
    const day = formatDayKey(message.createdAt);
    const needsDivider = day && (index === 0 || day !== previousDay);
    if (day) previousDay = day;
    result.push(needsDivider ? { ...message, dateBreak: day } : message);
  });
  return result;
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
  // 只有在「确实换了账号」时才清视图：同一个账号重新进入时不能把用户正在看的会话清掉。
  const switched = state.me && String(state.me.id) !== String(me?.id);
  window.dispatchEvent(new CustomEvent('nebula:user',{detail:me}));
  state.me = me;
  state.leaving = false;
  if (switched) clearConversationView();
  sender.clear();
  try { sender.restore(JSON.parse(sessionStorage.getItem(`phase11:pending:${me.id}`) || '[]')); }
  catch { /* 不信任损坏的缓存；恢复方法还会逐项验证载荷。 */ }
  updateProfileView();
  elements.authPanel.classList.add('hidden');
  elements.chatPanel.classList.remove('hidden');
  connectRealtime();
  startAnnouncements();
  await Promise.allSettled([loadConversations(), loadFriendsPreview(), refreshPendingBadge()]);
}

function showLoggedOut() {
  window.dispatchEvent(new CustomEvent('nebula:user',{detail:null}));
  announcementPlayer?.dispose();
  savePendingMessages();
  sender.clear();
  // 会话区立刻回到「未选择会话」态：showLoggedOut 只隐藏面板不清 DOM，
  // 下一个账号登录时【state.me 已经是 null，账号比对条件不会成立】，
  // 头部就会一直显示上一个账号的对端标题与头像，直到他点开任意会话。
  // 退出是会话真正的终点（不是重连），在这里清最准确。
  clearConversationView();
  state.transientMessages.clear();
  state.me = null;
  state.csrf = '';
  state.conversation = null;
  state.conversations = [];
  state.friends = [];
  state.blocked = [];
  state.members = [];
  state.unread.clear();
  state.unreadPending.clear();
  state.bumped = null;
  state.aiModels = null;            // 切号后不能沿用上一个账号的模型列表
  state.aiModelError = '';
  state.conversationAiMember = 0;
  state.aiTargetManual = false;
  state.seenMessageIds.clear();
  state.pendingRecalls.forEach(item => clearTimeout(item.timer));
  state.pendingRecalls.clear();
  clearTimeout(state.catchupTimer);
  clearTimeout(state.reconnectTimer);
  clearTimeout(sseTimer);
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
        && !conversation.containsAi && !state.aiConversations.has(Number(conversation.id))) return false;
    return !query || conversationTitle(conversation).toLocaleLowerCase().includes(query);
  });
  // 排序必须发生在渲染入口：state.conversations 的顺序只代表「上次服务端返回的顺序」，
  // 本地收到新消息后只更新了字段，没有重排。
  visible.sort(conversationOrder);
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
    if (state.bumped && state.bumped.id === Number(conversation.id)
        && Date.now() - state.bumped.at < 600) button.classList.add('is-bumped');

    const avatar = document.createElement('span');
    avatar.className = 'avatar';
    paintAvatar(avatar, conversation, conversation.kind === 'group' ? '群' : initials(conversationTitle(conversation), '聊'));
    const copy = document.createElement('span');
    copy.className = 'conversation-copy';
    const title = document.createElement('strong');
    title.textContent = conversationTitle(conversation);
    const detail = document.createElement('small');
    detail.textContent = conversation.lastMessagePreview || '暂无消息，开始交流吧';
    copy.append(title, detail);
    button.append(avatar, copy);
    if (Number(conversation.lastMessageAt)) {
      const time = document.createElement('time');
      time.dateTime = new Date(Number(conversation.lastMessageAt)).toISOString();
      time.textContent = formatChatTime(conversation.lastMessageAt);
      button.append(time);
    }
    // notificationsMuted 对私聊来自 contact_preferences（真实值），对群聊恒为 false
    // （PostgresStore.cpp:1318 无对端联系人）。会话级免打扰又没有写接口
    // （PATCH /api/conversations/:id 只收 title + announcement），
    // 所以群聊里宁可不画，也不给用户一个「看起来能设」的假象。
    if (conversation.kind !== 'group' && conversation.notificationsMuted) {
      const muted = document.createElement('span');
      muted.className = 'muted-mark';
      muted.textContent = '♩';
      muted.title = '已免打扰';
      button.append(muted);
    }

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
  const userId = state.me?.id;
  const version = ++state.listVersion;
  // 从发起请求这一刻开始记账：请求返回之前到达的未读，服务端基线里可能还没有。
  state.unreadPending.clear();
  elements.conversationLoading.classList.remove('hidden');
  try {
    const conversations = await api('/api/conversations');
    if (state.me?.id !== userId || state.listVersion !== version) return false;
    state.conversations = conversations;
    const unread = new Map();
    conversations.forEach(conversation => {
      const server = Math.max(0, Number(conversation.unreadCount) || 0);
      const pending = state.unreadPending.get(Number(conversation.id)) || 0;
      // 取较大值：服务端可能已经把这条算进基线了（pending 重复），也可能还没有（pending 是唯一来源）。
      // 宁可多一个红点，也不能让到达过的消息静默消失。
      const total = Math.max(server, pending);
      if (total) unread.set(Number(conversation.id), total);
    });
    state.unread = unread;
    state.unreadPending.clear();
    if (state.conversation) {
      const current = state.conversations.find(
        item => Number(item.id) === Number(state.conversation.id),
      );
      if (current) {
        state.conversation = current;
        paintConversationChrome(current);   // 备注/标题/头像变了要立刻反映到头部与详情栏
      } else clearConversationView();
    }
    renderConversations();
    setStatus(elements.chatStatus);
    return true;
  } catch (error) {
    setStatus(elements.chatStatus, error.message, 'error');
    return false;
  } finally {
    elements.conversationLoading.classList.add('hidden');
  }
}

/**
 * 会话头部与详情栏的身份区。会话列表、头部、详情栏、输入区目的地四处必须同源，
 * 都走 conversationTitle() 与 paintAvatar()，否则备注/头像会在某一处漏掉。
 */
function paintConversationChrome(conversation) {
  if (!conversation) return;
  const title = conversationTitle(conversation);
  const group = conversation.kind === 'group';
  const owner = { avatarUrl: conversation.avatarUrl, displayName: title };
  $('#conversation-title').textContent = title;
  $('#conversation-subtitle').textContent = group
    ? `群组会话 · ID ${conversation.id}`
    : `点对点会话 · ID ${conversation.id}`;
  paintAvatar($('#conversation-avatar'), owner, group ? '群' : '聊');
  $('#composer-destination').textContent = title;
  $('#detail-title').textContent = title;
  $('#detail-kind').textContent = group
    ? `群聊 · 会话 ID ${conversation.id}` : `私聊 · 会话 ID ${conversation.id}`;
  paintAvatar($('#detail-avatar'), owner, group ? '群' : '聊');
}

async function openConversation(conversation) {
  const version = ++state.viewVersion;
  state.conversation = conversation;
  renderConversations();

  paintConversationChrome(conversation);
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
    const [, members] = await Promise.all([
      history.open(conversation.id),
      api(`/api/conversations/${encodeURIComponent(conversation.id)}/members`),
    ]);
    if (version !== state.viewVersion) return;
    const messages = history.messages;
    state.members = members;
    const aiMember = members.find(member => member.aiAccount &&
      Number(member.userId) !== Number(state.me?.id));
    delete elements.messageForm.dataset.aiReply;
    state.conversationAiMember = aiMember ? Number(aiMember.userId) : 0;
    // 换会话是「自动识别优先」的新上下文：上一次的手选不该跨会话留着。
    state.aiTargetManual = false;
    // 只在已经读过模型列表时重建；否则等 loadAiModels() 回来时用
    // state.conversationAiMember 作为优先项，自动选中不会丢。
    if (state.aiModels) applyAiModels(state.aiModels);
    if (aiMember || messages.some(message => message.clientMessageId?.startsWith('ai:'))) {
      state.aiConversations.add(Number(conversation.id));
    }
    settleHistoryPending();
    renderCurrentMessages('bottom');
    renderConversations();
    renderConversationMembers();
    const me = members.find(member => Number(member.userId) === Number(state.me?.id));
    $('#add-member').classList.toggle(
      'hidden', conversation.kind !== 'group' || !['owner', 'administrator'].includes(me?.role),
    );
    renderGroupGovernance();
    await history.synchronize(Math.max(history.latest, Number(conversation.nextSequence) - 1));
    if (version !== state.viewVersion) return;
    acknowledge({ conversationId: conversation.id, sequence: history.cursor });
    setStatus(elements.chatStatus);
  } catch (error) {
    if (version === state.viewVersion) setStatus(elements.chatStatus, error.message, 'error');
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

function deliveryLabel(message) {
  if (message.state === 'recalled') return '已撤回';
  if (message.deliveryState === 'sending') return '发送中…';
  if (message.deliveryState === 'waiting') return '等待连接，尚未确认';
  if (message.deliveryState === 'unknown') return '结果未知，正在核对或等待重试';
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
  return canRecallMessage(message, state.me?.id, state.conversation, state.members);
}

function requestRecall(message, button) {
  if (!canRecall(message)) { renderCurrentMessages(); return; }
  if (Number(message.senderId) !== Number(state.me?.id) &&
      !window.confirm('以群管理权限撤回这条消息？撤回后所有成员看到撤回占位。')) return;
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
  const pending = { id: String(message.id), timer: setTimeout(() => {
    state.pendingRecalls.delete(commandId);
    renderCurrentMessages();
    setStatus(elements.chatStatus, '撤回结果暂未确认，正在同步服务器状态。');
    recoverRealtime();
  }, 5000) };
  state.pendingRecalls.set(commandId, pending);
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
  if (message.dateBreak) {
    // 绝对定位的块级子元素：不参与 .message 的 flex 布局，行高由 margin 承担并被 measure() 计入。
    const divider = document.createElement('div');
    divider.className = 'date-divider';
    divider.textContent = formatDateDivider(message.createdAt);
    item.classList.add('has-date-break');
    item.append(divider);
  }

  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  const messageSender = mine ? state.me : state.members.find(
    member => Number(member.userId) === Number(message.senderId));
  paintAvatar(avatar, messageSender, mine ? initials(state.me?.displayName || state.me?.username)
    : (aiMessage ? 'AI' : `#${message.senderId || '?'}`));
  const content = document.createElement('div');
  content.className = 'message-content';
  const meta = document.createElement('p');
  meta.className = 'message-author';
  const time = formatChatTime(message.createdAt);
  meta.textContent = options.streaming ? 'AI 正在生成'
    : `${aiMessage ? (messageSender?.displayName || 'AI 账号') : mine ? '我' : (messageSender?.displayName || messageSender?.username || `用户 ${message.senderId}`)}${time ? ` · ${time}` : ''}`;
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
  if (message.clientMessageId && ['unknown', 'waiting', 'failed'].includes(message.deliveryState) &&
      sender.entries.has(message.clientMessageId)) {
    const retry = document.createElement('button');
    retry.type = 'button';
    retry.className = 'message-retry';
    retry.textContent = '核对并重试';
    retry.addEventListener('click', () => sender.retry(message.clientMessageId));
    footer.append(retry);
  }
  if (aiMessage && generationId && !options.streaming && message.state !== 'recalled') {
    const reply=document.createElement('button');reply.type='button';reply.className='message-citations';reply.textContent='回复助手';
    // 顺序很关键：selectAiTarget 必须是最后一个写入者。
    // loadAiModels() 在第一个 await 之前会同步 replaceChildren('正在读取可用模型…')，
    // 若先选中再读模型，刚选中的 option 会被清掉、value 变成空串，接口回来后
    // applyAiModels 只能落到第一个模型——用户以为在回复 42，实际发给 7。
    // aiTargetManual 也必须在 await 之后才置 true，否则 applyAiModels 会读到被清空的 value。
    reply.onclick=async()=>{
      elements.messageForm.dataset.aiReply=String(message.id);
      $('#toggle-ai').setAttribute('aria-pressed','true');
      $('#ai-target-wrap').classList.remove('hidden');
      elements.messageForm.elements.message.placeholder='回复这条助手消息…';
      elements.messageForm.elements.message.focus();
      if(!state.aiModels)await loadAiModels();   // 内部已 catch，不会抛
      state.aiTargetManual=true;
      selectAiTarget(message.senderId);
    };
    footer.append(reply);
    const sources = document.createElement('button');
    sources.type = 'button';
    sources.className = 'message-citations';
    sources.textContent = '查看引用';
    sources.addEventListener('click', () => openCitationDetails(generationId));
    footer.append(sources);
  }
  if (aiMessage && !options.streaming && message.state !== 'recalled' &&
      message.id && state.conversation &&
      state.aiConversations.has(Number(state.conversation.id))) {
    const consent = document.createElement('button');
    consent.type = 'button';
    consent.className = 'message-training';
    consent.textContent = '授权训练';
    consent.addEventListener('click', () => openTrainingConsent(message));
    footer.append(consent);
  }
  if (canRecall(message)) {
    const recall = document.createElement('button');
    recall.type = 'button';
    recall.className = 'message-recall';
    const pending = [...state.pendingRecalls.values()].some(entry => entry.id === String(message.id));
    recall.disabled = pending;
    recall.textContent = pending ? '撤回中…' : mine ? '撤回' : '管理撤回';
    recall.addEventListener('click', () => requestRecall(message, recall));
    footer.append(recall);
  }
  content.append(meta, bubble);
  if (attachments.childElementCount) content.append(attachments);
  content.append(footer);
  item.append(avatar, content);
  return item;
}

/** 统一重排历史、待确认命令和 AI 流式气泡；保留阅读锚点，加载旧页不跳回底部。 */
function renderCurrentMessages(mode = 'preserve') {
  if (!state.conversation || history.conversationId !== Number(state.conversation.id)) return;
  // 跳转定位期间连翻十几页历史，逐页重绘会让列表抖动并打断高亮，这里只屏蔽中间过程。
  if (suppressMessageRender) return;
  const list = elements.messageList;
  const top = list.scrollTop;
  const nearBottom = list.scrollHeight - top - list.clientHeight < 80;
  const anchor = [...list.children].find(item => item.dataset.messageId &&
    item.offsetTop + item.offsetHeight >= top);
  const anchorId = anchor?.dataset.messageId;
  const anchorOffset = anchor ? anchor.offsetTop - top : 0;
  const streams = [...list.children].filter(item => item.dataset.generationId && !item.dataset.messageId);
  const pending = [...sender.entries.values()]
    .filter(entry => entry.payload.conversationId === Number(state.conversation.id))
    .map(entry => ({ conversationId: entry.payload.conversationId, senderId: state.me?.id,
      clientMessageId: entry.payload.id, body: entry.payload.content, createdAt: entry.createdAt,
      state: 'active', deliveryState: entry.status }));
  const files = [...state.transientMessages.values()].filter(message =>
    Number(message.conversationId) === Number(state.conversation.id));
  const pager = document.createElement('li');
  pager.className = 'history-pager';
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'history-load-button';
  button.disabled = !history.loaded || history.loadingOlder || !history.hasOlder;
  button.textContent = !history.loaded ? '正在加载最新消息…' : history.loadingOlder ? '正在加载…'
    : history.hasOlder ? '加载更早消息' : '已到最早消息';
  button.addEventListener('click', () => history.older().then(settleHistoryPending)
    .catch(error => setStatus(elements.chatStatus, error.message, 'error')));
  pager.append(button);
  if (history.loaded) {
    const copy = document.createElement('small');
    copy.textContent = `已显示 ${history.records.size} 条 · 同步至 #${history.cursor}`;
    pager.append(copy);
  }
  const sync = document.createElement('button');
  sync.type = 'button';
  sync.className = 'history-sync-button';
  sync.textContent = '同步消息';
  sync.addEventListener('click', () => recoverRealtime());
  pager.append(sync);
  if (history.hasNewer) {
    const newer = document.createElement('button');
    newer.type = 'button'; newer.className = 'history-newer-button';
    newer.disabled = history.loadingNewer;
    newer.textContent = history.loadingNewer ? '正在加载…' : '加载较新消息';
    newer.addEventListener('click', () => history.newer().then(settleHistoryPending)
      .catch(error => setStatus(elements.chatStatus, error.message, 'error')));
    pager.append(newer);
  }
  const rows = withDateDividers([...history.messages, ...pending, ...files]);
  rows.forEach(message => rememberMessageId(message.id));
  messageView.setMessages(rows, JSON.stringify([state.me?.id, state.members]),
    { before: [pager], after: streams });
  const version = state.viewVersion;
  requestAnimationFrame(() => {
    if (version !== state.viewVersion) return;
    const restored = anchorId && existingMessage(anchorId);
    if (mode === 'bottom') list.scrollTop = list.scrollHeight;
    else if (restored) list.scrollTop = restored.offsetTop - anchorOffset;
    else list.scrollTop = top;
    if (mode === 'follow' && nearBottom) list.scrollTop = list.scrollHeight;
  });
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

function previousPromptForMessage(messageId) {
  let current = existingMessage(messageId)?.previousElementSibling;
  while (current) {
    if (current.classList.contains('mine') &&
        !current.classList.contains('message-recalled'))
      return current.querySelector('.message-bubble')?.textContent?.trim() || '';
    current = current.previousElementSibling;
  }
  return '';
}

async function openTrainingConsent(message = null) {
  const form = $('#training-form');
  setStatus($('#training-status'));
  if (message) {
    form.elements.sourceMessageId.value = String(message.id || '');
    form.elements.sanitizedPrompt.value = previousPromptForMessage(message.id);
    form.elements.sanitizedResponse.value = message.body || '';
    form.elements.consentGranted.checked = false;
    $('#training-source-hint').textContent =
      `来源消息 #${message.id}。请先删除隐私、密钥和内部地址，再勾选明确同意。`;
  } else {
    form.reset();
    form.elements.sourceMessageId.value = '';
    $('#training-source-hint').textContent =
      '请在 AI 回复下点击“授权训练”，系统会带入这一轮对话。';
  }
  elements.trainingDialog.showModal();
  await loadTrainingCandidates();
}

// 审核发生在服务器命令行工具里（tools/phase11_training_admin.cpp:141-144），
// 网页只负责提交与撤销。文案必须让用户知道「不是我这里出了问题」。
const trainingStateLabels = {
  submitted: '已提交 · 等待管理员在管理工具中离线审核',
  approved: '已通过审核',
  rejected: '未通过审核',
  exported: '已进入数据集',
  revoked: '已撤销',
};

function renderTrainingCandidates(candidates) {
  const rows = candidates.map(candidate => {
    const item = document.createElement('li');
    const copy = document.createElement('div');
    const title = document.createElement('strong');
    title.textContent = `样本 #${candidate.id} · ${trainingStateLabels[candidate.state] || candidate.state}`;
    const prompt = document.createElement('small');
    prompt.textContent = candidate.sanitizedPrompt;
    const response = document.createElement('p');
    response.textContent = candidate.sanitizedResponse;
    copy.append(title, prompt, response);
    const revoke = document.createElement('button');
    revoke.type = 'button';
    revoke.className = 'mini-button danger compact-button';
    revoke.textContent = candidate.state === 'revoked' ? '已撤销' : '撤销授权';
    revoke.disabled = candidate.state === 'revoked';
    revoke.addEventListener('click', async () => {
      revoke.disabled = true;
      try {
        await api(`/api/training/candidates/${candidate.id}/revoke`, {
          method: 'POST', body: '{}',
        });
        toast('训练授权已撤销', 'success');
        await loadTrainingCandidates();
      } catch (error) {
        setStatus($('#training-status'), error.message, 'error');
        revoke.disabled = false;
      }
    });
    item.append(copy, revoke);
    return item;
  });
  $('#training-list').replaceChildren(...(rows.length
    ? rows : [actionListEmpty('还没有提交过训练授权。')]));
  $('#training-count').textContent = String(rows.length);
}

async function loadTrainingCandidates() {
  $('#training-list').replaceChildren(actionListEmpty('正在加载授权记录…'));
  try {
    renderTrainingCandidates(await api('/api/training/candidates'));
  } catch (error) {
    $('#training-list').replaceChildren(actionListEmpty('授权记录加载失败'));
    setStatus($('#training-status'), error.message, 'error');
  }
}

function existingMessage(messageId) {
  return [...elements.messageList.children].find(item => item.dataset.messageId === String(messageId));
}

/** 清除上一次的搜索高亮（含未到期的定时器），避免连续跳转留下多个高亮行。 */
function clearSearchHit() {
  if (state.searchHit?.timer) clearTimeout(state.searchHit.timer);
  state.searchHit = null;
  elements.messageList.querySelectorAll('.search-hit')
    .forEach(node => node.classList.remove('search-hit'));
}

/**
 * 把目标序号拉进当前历史窗口。
 * ChatHistory 只有「从 cursor 往后补」和「从 oldest 往前翻」两条路，
 * 目标在窗口之外时只能反复 older()；每轮 100 条，受 pageBudget 并发闸门约束。
 */
async function ensureSequenceLoaded(sequence) {
  const target = Number(sequence) || 0;
  if (!target) return false;
  if (!history.loaded && history.ready) await history.ready;
  let rounds = 0;
  while (history.loaded && target < history.oldest && history.hasOlder && rounds < 30) {
    const previousOldest = history.oldest;
    setStatus($('#search-status'), `正在定位…（已加载 ${history.records.size} 条）`);
    await history.older();
    ++rounds;
    if (history.oldest >= previousOldest) break;   // 没有前进就停，绝不死循环
  }
  return history.loaded && target >= history.oldest && target <= history.latest;
}

/**
 * 让目标行进入视口并高亮 2000ms。
 * .message-list 有 scroll-behavior:smooth，平滑滚动会让「先设位置再测量」全部失效，
 * 所以定位期间临时改成 auto，定位完成后恢复原值。
 */
function revealMessageRow(messageId) {
  const key = String(messageId);
  const list = elements.messageList;
  const previousBehavior = list.style.scrollBehavior;
  clearSearchHit();
  list.style.scrollBehavior = 'auto';
  // 第一帧：让 renderCurrentMessages 的锚点恢复先跑完；第二帧：定位置、取节点。
  requestAnimationFrame(() => requestAnimationFrame(() => {
    const node = messageView.scrollToKey(key) || existingMessage(key);
    if (!node) {
      list.style.scrollBehavior = previousBehavior;
      // 兜底：实在取不到行也不要把用户丢在原地，至少回到最新消息。
      list.scrollTop = list.scrollHeight;
      setStatus(elements.chatStatus, '已在会话中定位，但该消息暂不可见；可用“同步消息”重试。', 'error');
      return;
    }
    node.classList.add('search-hit');
    // scrollIntoView 必须在 scroll-behavior 还是 auto 的时候调用，
    // 否则 CSS 的 smooth 会让它开始一段动画，定位结束时位置还没到，测量就白做了。
    node.scrollIntoView({ block: 'center', behavior: 'auto' });
    list.style.scrollBehavior = previousBehavior;
    state.searchHit = { key, timer: window.setTimeout(() => {
      node.classList.remove('search-hit');
      state.searchHit = null;
    }, 2000) };
  }));
}

/** 搜索结果 → 定位 → 关闭弹窗 → 高亮。失败时把原因留在搜索弹窗里，不清空结果。 */
async function jumpToMessage(message) {
  const conversation = state.conversation;
  if (!conversation || !message) return;
  if (Number(message.conversationId) !== Number(conversation.id)) {
    throw new Error('这条结果属于其它会话，请重新打开该会话后再搜索');
  }
  let found = false;
  suppressMessageRender = true;
  try {
    found = await ensureSequenceLoaded(message.sequence);
  } finally {
    suppressMessageRender = false;
  }
  if (history.conversationId !== Number(conversation.id)) return;   // 期间切了会话，放弃
  if (!found) {
    // 定位失败也要把刚拉进来的历史画出来，否则列表会停留在跳转前的旧窗口。
    renderCurrentMessages();
    throw new Error(`未能定位到第 ${message.sequence} 条消息，请点“同步消息”后重试`);
  }
  elements.searchDialog.close();
  renderCurrentMessages();
  revealMessageRow(message.id);
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
  if (message.id) {
    if (Number(message.senderId) === Number(state.me?.id)) state.transientMessages.delete(message.clientMessageId);
    if (Number(message.conversationId) === history.conversationId) history.ingest(message);
    rememberMessageId(message.id);
  } else if (message.clientMessageId) state.transientMessages.set(message.clientMessageId, { ...message });
  renderCurrentMessages('follow');
}

function acceptPendingMessage(message) {
  sender.confirm(message);
  advanceConversationSequence(message.conversationId, message.sequence);
  renderConversations();
}

function applyRecall(message) {
  history.ingest(message);
  state.pendingRecalls.forEach((entry, commandId) => {
    if (entry.id === String(message.id)) {
      clearTimeout(entry.timer);
      state.pendingRecalls.delete(commandId);
    }
  });
  renderCurrentMessages();
}

function failPendingMessage(clientMessageId) {
  const transient = state.transientMessages.get(clientMessageId);
  if (transient) { transient.deliveryState = 'failed'; renderCurrentMessages(); }
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
  if (!state.conversation || !history.loaded || !message?.sequence ||
      Number(message.conversationId) !== Number(state.conversation.id)) return;
  const base = {
    v: 1,
    id: createClientId(),
    conversationId: Number(state.conversation.id),
    // 只上报连续恢复到的书签，不能把较高的乱序广播误报为已经读完中间缺页。
    sequence: Math.min(Number(message.sequence), history.cursor),
  };
  if (!base.sequence) return;
  sendSocket({ ...base, type: 'chat.ack' });
  sendSocket({ ...base, id: createClientId(), type: 'chat.read' });
}

/**
 * 把 AI 目标选中到指定账号；options 里没有就先补一个，避免 select.value 静默变空。
 * `<select>` 的 .value 赋一个不在 options 里的值不会报错，只会静默变成空串，
 * 症状是「AI 模式发不出消息」且提示文案对不上原因——所以必须显式补 option。
 */
function selectAiTarget(userId) {
  const select = elements.messageForm.elements.aiUserId;
  const value = String(userId || '');
  if (!value) { select.value = select.options[0]?.value || ''; return; }
  if (![...select.options].some(option => option.value === value)) {
    select.append(new Option(`会话 AI 成员 #${value}`, value));
  }
  select.value = value;
}

/**
 * 用模型列表重建 options，并优先保住当前该有的选中项。
 * 优先级：用户手选 > 会话自动识别 > 第一个可用模型。
 * 不能拿「上一次会话遗留的 select.value」当默认值，否则会把上个会话的
 * `会话 AI 成员 #N` 带进一个没有 AI 成员的会话里，用户看到的是错的。
 * 也绝不在「有明确目标」时静默换成第一个模型。
 */
function applyAiModels(models) {
  const select = elements.messageForm.elements.aiUserId;
  const member = String(state.conversationAiMember || '');
  const current = String(select.value || '');
  const preferred = state.aiTargetManual ? current : member;
  select.replaceChildren();
  if (!models.length) {
    select.append(new Option(state.aiModelError
      ? '模型列表读取失败，可展开「高级」填写账号 ID'
      : '本站暂无已配置模型', ''));
    // 没有模型列表不等于要丢掉目标：会话里有 AI 成员是活的事实，保留它并显式标注出来。
    if (preferred) selectAiTarget(preferred); else select.value = '';
    return;
  }
  models.forEach(model => {
    select.append(new Option(
      `${model.name || `模型 #${model.accountId}`} · ${model.runtime || 'unknown'}`,
      String(model.accountId),
    ));
  });
  if (preferred) selectAiTarget(preferred);
  else select.value = select.options[0].value;
}

/** 只在第一次打开 AI 模式时打一次接口；失败不阻塞手填兜底。 */
async function loadAiModels() {
  const select = elements.messageForm.elements.aiUserId;
  if (state.aiModels) { applyAiModels(state.aiModels); return; }
  select.replaceChildren(new Option('正在读取可用模型…', ''));
  select.disabled = true;
  try {
    const models = await api('/api/ai/web/models');
    state.aiModels = Array.isArray(models) ? models : [];
    state.aiModelError = '';
  } catch (error) {
    state.aiModels = [];
    state.aiModelError = error.message;
  } finally {
    select.disabled = false;
  }
  applyAiModels(state.aiModels);
}

/** 手填的「高级」输入优先；否则用下拉值。 */
function readAiTarget() {
  const manual = $('#ai-user-manual')?.value.trim() || '';
  if (/^\d+$/.test(manual)) return manual;
  return String(elements.messageForm.elements.aiUserId.value || '').trim();
}

function refreshCancelButton() {
  const active = [...state.activeGenerations.values()]
    .find(item => Number(item.conversationId) === Number(state.conversation?.id));
  $('#cancel-generation').classList.toggle('hidden', !active);
  $('#cancel-generation').dataset.generationId = active?.generationId || '';
}

function onRealtimeMessage(message) {
  if (message.type === 'chat.message') {
    // 先收敛待确认命令，再做双通道去重。发送成功证明可能先于 accepted 抵达。
    if (Number(message.senderId) === Number(state.me?.id)) sender.confirmMessage(message);
    const duplicate = rememberMessageId(message.id);
    advanceConversationSequence(message.conversationId, message.sequence);
    const summary = state.conversations.find(item => Number(item.id) === Number(message.conversationId));
    if (summary && Number(message.sequence) >= Number(summary.nextSequence) - 1) {
      summary.lastMessagePreview = message.body || (message.attachments?.length ? '[附件]' : '');
      summary.lastMessageAt = message.createdAt;
      state.bumped = { id: Number(message.conversationId), at: Date.now() };
      renderConversations();
    }
    const isCurrent = Number(message.conversationId) === Number(state.conversation?.id);
    if (isCurrent) {
      appendMessage(message);
      acknowledge(message);
      if (Number(message.sequence) > history.cursor) scheduleCatchup();
    } else if (!duplicate) {
      // 同一账号的另一浏览器标签页可能收到自己刚发送的广播；发送者游标已经在
      // 服务端推进，因此这里只给真正来自其他成员的消息增加本地未读数。
      if (Number(message.senderId) !== Number(state.me?.id)) bumpUnread(message.conversationId);
      renderConversations();
    }
    return;
  }
  if (message.type === 'chat.message-recalled') {
    const summary = state.conversations.find(item => Number(item.id) === Number(message.conversationId));
    if (summary && Number(message.sequence) === Number(summary.nextSequence) - 1) {
      summary.lastMessagePreview = '这条消息已被撤回';renderConversations();
    }
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
      // 自己已读之后，该会话的本地增量必须作废，否则下一次 loadConversations 的 max() 会把它算回来。
      state.unreadPending.delete(Number(message.conversationId));
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
    sender.reject(message.replyTo, message.status);
    const recalledMessage = state.pendingRecalls.get(message.replyTo);
    if (recalledMessage) {
      clearTimeout(recalledMessage.timer);
      state.pendingRecalls.delete(message.replyTo);
      renderCurrentMessages();
    }
    setStatus(elements.chatStatus, message.status || '消息发送失败', 'error');
  } else if (message.type === 'chat.recall.accepted') {
    clearTimeout(state.pendingRecalls.get(message.replyTo)?.timer);
    state.pendingRecalls.delete(message.replyTo);
    renderCurrentMessages();
    scheduleCatchup();
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

function connectRealtime() { connectSocket(); connectEvents(); }

function connectSocket() {
  clearTimeout(state.reconnectTimer);
  state.socket?.close();
  setRealtimeState('ws', 'error');
  const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
  const socket = new WebSocket(`${protocol}://${location.host}/ws`);
  state.socket = socket;
  socket.addEventListener('open', () => {
    if (state.socket !== socket || !state.me || state.leaving) return;
    setRealtimeState('ws', 'online');
    wsBackoff.reset();
    recoverRealtime();
  });
  socket.addEventListener('close', () => {
    if (state.socket !== socket) return;
    sender.disconnected();
    setRealtimeState('ws', state.me && !state.leaving ? 'error' : 'offline');
    if (state.me && !state.leaving) state.reconnectTimer = setTimeout(connectSocket, wsBackoff.next());
  });
  socket.addEventListener('error', () => {
    if (state.socket === socket && state.me) setRealtimeState('ws', 'error');
  });
  socket.addEventListener('message', event => {
    if (state.socket !== socket || !state.me || state.leaving) return;
    try { onRealtimeMessage(JSON.parse(event.data)); }
    catch { setStatus(elements.chatStatus, '收到无法解析的实时消息', 'error'); }
  });

}

function connectEvents() {
  clearTimeout(sseTimer);
  state.events?.close();
  setRealtimeState('sse', 'error');
  const events = new EventSource('/events');
  state.events = events;
  for (const name of ['announcement.created','announcement.audio.ready']) events.addEventListener(name,()=>window.dispatchEvent(new Event('nebula:announcement')));
  events.addEventListener('open', () => {
    if (state.events !== events || !state.me || state.leaving) return;
    setRealtimeState('sse', 'online');
    sseBackoff.reset();
    recoverRealtime();
  });
  events.addEventListener('error', () => {
    if (state.events !== events) return;
    setRealtimeState('sse', state.me ? 'error' : 'offline');
    events.close();
    if (state.me && !state.leaving) sseTimer = setTimeout(connectEvents, sseBackoff.next());
  });
  // 回放窗口过期时，数据库历史是最终恢复来源。
  events.addEventListener('replay-reset', () => scheduleCatchup());
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
      await recoverRealtime();
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
  // 备注会改变会话摘要里的 displayTitle（SQL: PostgresStore.cpp:1288），
  // 免打扰会改变 notificationsMuted。不重拉会话列表，列表和会话头就会一直显示旧值。
  await loadConversations();
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
    if(new URL(location.href).searchParams.get('editProfile')==='1') $('#show-profile').click();
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
  let aiMode = $('#toggle-ai').getAttribute('aria-pressed') === 'true';
  let aiUserId = readAiTarget();
  const mentioned=state.members.find(member=>member.aiAccount&&[member.username,member.displayName].filter(Boolean).some(name=>content.includes('@'+name)));
  if(mentioned){aiMode=true;aiUserId=String(mentioned.userId);}
  if (selectedFile && aiMode) {
    setStatus(elements.chatStatus, 'AI 模式暂只接收文字提示词；请先关闭 AI 模式再发送文件。', 'error');
    return;
  }
  if (aiMode && !aiUserId) {
    setStatus(elements.chatStatus, state.aiModels?.length
      ? '请选择一个 AI 模型后再发送。'
      : 'AI 模式暂无可用模型：展开「高级」填写 AI 账号 ID，或关闭 AI 模式改成普通消息。', 'error');
    // 手填框在折叠的「高级」里，先展开再聚焦，否则 focus() 落在不可见元素上等于没反应。
    const manual = $('#ai-user-manual');
    if (manual && manual.offsetParent === null) manual.closest('details')?.setAttribute('open', '');
    (manual || form.elements.aiUserId).focus();
    return;
  }
  const messageId = createClientId();
  const conversationId = Number(state.conversation.id);
  if (selectedFile) {
    const sendButton = form.querySelector('.send-button');
    state.uploadingFile = true;
    setButtonBusy(sendButton, true, '上传 0%');
    appendMessage({
      conversationId,
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
      const result = await api(`/api/conversations/${conversationId}/file-messages`, {
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
  const payload = {
    v: 1,
    type: aiMode ? 'ai.generate' : 'chat.send',
    conversationId,
    id: messageId,
    ...(aiMode ? { to: Number(aiUserId),aiTrigger:form.dataset.aiReply?'reply':mentioned?'mention':'explicit',...(form.dataset.aiReply?{replyTo:form.dataset.aiReply}:{}) } : {}),
    content,
  };
  if (aiMode) {
    if (!sendSocket(payload)) {
      setStatus(elements.chatStatus, 'AI 请求未发送：请等待实时通道重连后重试。', 'error');
      return;
    }
    state.activeGenerations.set(messageId, { generationId: messageId, conversationId: state.conversation.id });
    refreshCancelButton();
    setStatus(elements.chatStatus, 'AI 请求已进入后台队列。', 'success');
  } else {
    try { sender.submit(payload); }
    catch (error) { setStatus(elements.chatStatus, error.message, 'error'); return; }
    setStatus(elements.chatStatus);
  }
  input.value = '';
  input.style.height = '';
  delete form.dataset.aiReply;
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
  // 只在打开时读一次；失败不抛，交给提交时的空值分支给出可操作提示。
  if (active) loadAiModels();
});

// 只有真实用户操作才会触发 change（脚本赋值不会），用它区分「手选」与「自动识别」。
elements.messageForm.elements.aiUserId.addEventListener('change', () => {
  state.aiTargetManual = true;
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
$('#show-training').addEventListener('click', () => openTrainingConsent());
$('#training-form').addEventListener('submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const sourceMessageId = String(form.elements.sourceMessageId.value || '').trim();
  if (!sourceMessageId) {
    setStatus($('#training-status'),
      '请先在一条已完成的 AI 回复下点击“授权训练”。', 'error');
    return;
  }
  if (!form.elements.consentGranted.checked) {
    setStatus($('#training-status'), '需要明确勾选训练授权。', 'error');
    return;
  }
  setButtonBusy(button, true, '正在提交…');
  try {
    await api('/api/training/candidates', {
      method: 'POST',
      body: JSON.stringify({
        sourceMessageId,
        sanitizedPrompt: form.elements.sanitizedPrompt.value.trim(),
        sanitizedResponse: form.elements.sanitizedResponse.value.trim(),
        consentGranted: 'true',
      }),
    });
    form.reset();
    $('#training-source-hint').textContent =
      '样本已提交；审核由管理员在服务器命令行工具执行，网页不提供审核入口。';
    setStatus($('#training-status'), '训练样本已提交，等待离线审核。', 'success');
    toast('训练授权已记录', 'success');
    await loadTrainingCandidates();
  } catch (error) {
    setStatus($('#training-status'), error.message, 'error');
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
    // 记住发起搜索的会话：结果行可能在用户切换到其它会话之后才被点击。
    const searchedConversationId = Number(state.conversation.id);
    const messages = await api(`/api/search/messages?conversationId=${encodeURIComponent(state.conversation.id)}&query=${encodeURIComponent(query)}&limit=50`);
    const senderLabel = senderId => {
      if (Number(senderId) === Number(state.me?.id)) return '我';
      const member = state.members.find(item => Number(item.userId) === Number(senderId));
      return member?.displayName || member?.username || `用户 ${senderId}`;
    };
    $('#search-results').replaceChildren(...messages.map(message => {
      const item = document.createElement('li');
      const row = document.createElement('button');
      row.type = 'button';
      row.className = 'search-result';
      const body = document.createElement('p');
      body.textContent = message.body || '（这条消息没有正文）';
      const meta = document.createElement('small');
      // 不再展示“消息序号”这类内部术语，改为用户能定位的「谁 · 什么时候」。
      meta.textContent = `${senderLabel(message.senderId)} · ${formatChatTime(message.createdAt)}`;
      row.append(body, meta);
      row.addEventListener('click', async () => {
        // 搜索结果只在本会话内有权限语义：切走之后点旧结果必须拒绝，而不是跳到别的会话。
        if (Number(state.conversation?.id) !== searchedConversationId) {
          setStatus($('#search-status'), '会话已切换，请重新搜索后再定位。', 'error');
          return;
        }
        row.disabled = true;
        setStatus($('#search-status'), '正在定位…');
        try { await jumpToMessage(message); }
        catch (error) { setStatus($('#search-status'), error.message, 'error'); }
        finally { row.disabled = false; }
      });
      item.append(row);
      return item;
    }));
    // 服务端只返回「最早 50 条匹配」（PostgresStore.cpp:1542 无 offset），不能说“找到 N 条”。
    setStatus($('#search-status'), messages.length
      ? `显示最早 ${messages.length} 条匹配${messages.length >= 50 ? '（结果已达上限，请换更精确的关键词）' : ''}`
      : '没有匹配消息，换一个关键词试试。', messages.length ? 'success' : '');
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

// 公告播放器按账号建立，退出时销毁，不沿用另一个账号的通知设置。
let announcementPlayer;
async function startAnnouncements() {
  announcementPlayer?.dispose();
  try {
    const a = new BusinessApi(); await a.restore();
    let box = document.querySelector('.announcements');
    if (!box) { box = document.createElement('div'); box.className = 'announcements'; document.body.append(box); }
    box.replaceChildren(); announcementPlayer = new AnnouncementPlayer(a, box); await announcementPlayer.start();
  } catch {}
}
