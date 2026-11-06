import { contactsApi } from "../../contacts/ContactsApi/ContactsApi.js?v=144f1e6009a3d461";
import { chatApi } from "../ChatApi/ChatApi.js?v=144f1e6009a3d461";
import { matchesRecallReceipt, recalledFromReceipt } from '../MessageRecall/MessageRecall.js?v=144f1e6009a3d461';
export const dependencies={"resources":["$","unreadWatermarks","wsBackoff","sseBackoff","elements","state","history","sender","runtime"],"commands":["setStatus","toast","createClientId","rememberMessageId","advanceConversationSequence","bumpUnread","clearConversationView","api","renderConversations","loadConversations","openConversation","renderConversationMembers","renderGroupGovernance","createMessageElement","renderCurrentMessages","scrollMessages","refreshMessageReceipts","generationElement","appendMessage","applyRecall","refreshCancelButton","renderContactLists","renderFriendPreview","loadFriendsPreview","loadContacts"],"types":[]};
/** RealtimeController: explicit use cases; shared state/other controllers arrive through context. */
export function createRealtimeController(context) {
  const {$,unreadWatermarks,wsBackoff,sseBackoff,elements,state,history,sender,runtime,actions}=context;

  const setStatus = (...args) => actions.setStatus(...args);
  const toast = (...args) => actions.toast(...args);
  const createClientId = (...args) => actions.createClientId(...args);
  const rememberMessageId = (...args) => actions.rememberMessageId(...args);
  const advanceConversationSequence = (...args) => actions.advanceConversationSequence(...args);
  const bumpUnread = (...args) => actions.bumpUnread(...args);
  const clearConversationView = (...args) => actions.clearConversationView(...args);
  const api = (...args) => actions.api(...args);
  const renderConversations = (...args) => actions.renderConversations(...args);
  const loadConversations = (...args) => actions.loadConversations(...args);
  const openConversation = (...args) => actions.openConversation(...args);
  const renderConversationMembers = (...args) => actions.renderConversationMembers(...args);
  const renderGroupGovernance = (...args) => actions.renderGroupGovernance(...args);
  const createMessageElement = (...args) => actions.createMessageElement(...args);
  const renderCurrentMessages = (...args) => actions.renderCurrentMessages(...args);
  const scrollMessages = (...args) => actions.scrollMessages(...args);
  const refreshMessageReceipts = (...args) => actions.refreshMessageReceipts(...args);
  const generationElement = (...args) => actions.generationElement(...args);
  const appendMessage = (...args) => actions.appendMessage(...args);
  const applyRecall = (...args) => actions.applyRecall(...args);
  const refreshCancelButton = (...args) => actions.refreshCancelButton(...args);
  const renderContactLists = (...args) => actions.renderContactLists(...args);
  const renderFriendPreview = (...args) => actions.renderFriendPreview(...args);
  const loadFriendsPreview = (...args) => actions.loadFriendsPreview(...args);
  const loadContacts = (...args) => actions.loadContacts(...args);
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
    return await api(chatApi.getConversationsIdMessagesByClientId({"id":(payload.conversationId)}, String(new URLSearchParams({ clientMessageId: payload.id }))), { signal: controller.signal });
  } finally { clearTimeout(timer); }
}

function settleHistoryPending() {
  history.messages.forEach(message => {
    if (Number(message.senderId) === Number(state.me?.id)) sender.confirmMessage(message);
  });
}

function scheduleCatchup() {
  clearTimeout(state.catchupTimer);
  state.catchupTimer = setTimeout(() => recoverRealtime(), 100);
}

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
          const members = await api(chatApi.getConversationsIdMembers({"id":(conversation.id)}));
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

function acceptPendingMessage(message) {
  sender.confirm(message);
  advanceConversationSequence(message.conversationId, message.sequence);
  renderConversations();
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
      if (Number(message.senderId) !== Number(state.me?.id)) bumpUnread(message.conversationId, message.sequence);
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
      const remaining = unreadWatermarks.observe(message.conversationId, 0, message.sequence);
      if (remaining) state.unread.set(Number(message.conversationId), remaining);
      else state.unread.delete(Number(message.conversationId));
      renderConversations();
    }
    return;
  }
  if (message.type === 'conversation.preferences') {
    const muted=state.preferenceSnapshots.absorb(message.conversationId,message.notificationsMuted,message.version);
    const conversation=state.conversations.find(c=>String(c.id)===String(message.conversationId));
    if(!conversation){loadConversations();return;}
    conversation.notificationsMuted=muted;
    if(String(state.conversation?.id)===String(conversation.id))actions.paintConversationChrome(conversation);
    renderConversations();return;
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
    const pending=state.pendingRecalls.get(message.replyTo);
    if(!pending)return; // Late replies from an old account/operation cannot change this view.
    if(!matchesRecallReceipt(message,pending))return; // Keep waiting; a mismatched ACK proves nothing.
    clearTimeout(pending.timer);
    state.pendingRecalls.delete(message.replyTo);
    const recalled=recalledFromReceipt(message,pending,history.records.get(pending.id));
    if(recalled)onRealtimeMessage({...recalled,type:'chat.message-recalled'});
    renderCurrentMessages();
    scheduleCatchup();
    setStatus(elements.chatStatus,
      message.status === 'already_recalled' ? '该消息已经撤回。' : '服务器已确认撤回。',
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
  clearTimeout(runtime.sseTimer);
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
    if (state.me && !state.leaving) runtime.sseTimer = setTimeout(connectEvents, sseBackoff.next());
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
    const requests = await api(contactsApi.getFriendRequests({}));
    const badge = $('#friend-badge');
    badge.textContent = String(requests.length);
    badge.classList.toggle('hidden', requests.length === 0);
    $('#show-requests').classList.toggle('has-notice', requests.length !== 0);
  } catch { /* The dialog will show the detailed error when opened. */ }
}

  return {savePendingMessages,verifyOwnMessage,settleHistoryPending,scheduleCatchup,recoverRealtime,setRealtimeState,acceptPendingMessage,sendSocket,acknowledge,onRealtimeMessage,connectRealtime,connectSocket,connectEvents,refreshPendingBadge};
}
