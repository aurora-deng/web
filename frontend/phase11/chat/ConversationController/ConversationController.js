import { sharedApi } from "../../shared/SharedApi/SharedApi.js?v=144f1e6009a3d461";
import { chatApi } from "../ChatApi/ChatApi.js?v=144f1e6009a3d461";
export const dependencies={"resources":["$","userPickers","unreadWatermarks","elements","state","messageView","history","sender","runtime"],"commands":["savePendingMessages","settleHistoryPending","setStatus","toast","initials","api","paintAvatar","renderConversationMembers","renderGroupGovernance","renderCurrentMessages","acknowledge","applyAiModels","loadAiModels","refreshCancelButton","connectRealtime","refreshPendingBadge","loadFriendsPreview"],"types":["UserPicker","BusinessApi","AnnouncementPlayer","formatChatTime"]};
/** ConversationController: explicit use cases; shared state/other controllers arrive through context. */
export function createConversationController(context) {
  const {$,userPickers,unreadWatermarks,elements,state,messageView,history,sender,runtime,actions,types}=context;
  const {UserPicker,BusinessApi,AnnouncementPlayer,formatChatTime}=types;
  const savePendingMessages = (...args) => actions.savePendingMessages(...args);
  const settleHistoryPending = (...args) => actions.settleHistoryPending(...args);
  const setStatus = (...args) => actions.setStatus(...args);
  const toast = (...args) => actions.toast(...args);
  const initials = (...args) => actions.initials(...args);
  const api = (...args) => actions.api(...args);
  const paintAvatar = (...args) => actions.paintAvatar(...args);
  const renderConversationMembers = (...args) => actions.renderConversationMembers(...args);
  const renderGroupGovernance = (...args) => actions.renderGroupGovernance(...args);
  const renderCurrentMessages = (...args) => actions.renderCurrentMessages(...args);
  const acknowledge = (...args) => actions.acknowledge(...args);
  const applyAiModels = (...args) => actions.applyAiModels(...args);
  const loadAiModels = (...args) => actions.loadAiModels(...args);
  const refreshCancelButton = (...args) => actions.refreshCancelButton(...args);
  const connectRealtime = (...args) => actions.connectRealtime(...args);
  const refreshPendingBadge = (...args) => actions.refreshPendingBadge(...args);
  const loadFriendsPreview = (...args) => actions.loadFriendsPreview(...args);
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

function conversationOrder(left, right) {
  const leftUnread = state.unread.get(Number(left.id)) || 0;
  const rightUnread = state.unread.get(Number(right.id)) || 0;
  if (Boolean(leftUnread) !== Boolean(rightUnread)) return leftUnread ? -1 : 1;
  const leftAt = Number(left.lastMessageAt) || 0;
  const rightAt = Number(right.lastMessageAt) || 0;
  if (leftAt !== rightAt) return rightAt - leftAt;
  return Number(right.id) - Number(left.id);
}

function bumpUnread(conversationId, sequence) {
  const id = Number(conversationId);
  const total = unreadWatermarks.observe(id, sequence);
  if (total) state.unread.set(id, total); else state.unread.delete(id);
  state.bumped = { id, at: Date.now() };
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

function updateProfileView() {
  if (!state.me) return;
  const name = state.me.displayName || state.me.username;
  $('#profile-name').textContent = name;
  $('#profile-handle').textContent = `@${state.me.username} · ID ${state.me.id}`;
  paintAvatar($('#profile-avatar'), state.me, initials(name));
  paintAvatar($('#profile-dialog-avatar'), state.me, initials(name));
}

async function showChat(me) {
  runtime.accountAbort.abort();runtime.accountAbort=new AbortController();
  // 只有在「确实换了账号」时才清视图：同一个账号重新进入时不能把用户正在看的会话清掉。
  const switched = state.me && String(state.me.id) !== String(me?.id);
  window.dispatchEvent(new CustomEvent('nebula:user',{detail:me}));
  state.me = me;
  await loadWorkspaceCapabilities();
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
  runtime.accountAbort.abort();runtime.accountAbort=new AbortController();
  state.search.controller?.abort();++state.search.version;
  state.activeGenerations.clear();
  window.dispatchEvent(new CustomEvent('nebula:user',{detail:null}));
  runtime.announcementPlayer?.dispose();
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
  state.capabilities=null; userPickers.splice(0).forEach(picker=>picker.dispose());
  unreadWatermarks.clear();
  state.preferenceSnapshots.clear();
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
  clearTimeout(runtime.sseTimer);
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
    // 私聊读 contact_preferences，群聊读 conversation_preferences（024 表按 member 维度，
    // 不区分 kind）；写入口是 PATCH /api/conversations/:id/preferences，
    // 由 ConversationBindings 的 #conversation-muted 触发。两者都能落库，所以按真实值画。
    if (conversation.notificationsMuted) {
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
  elements.conversationLoading.classList.remove('hidden');
  try {
    const conversations = await api(chatApi.getConversations({}));
    if (state.me?.id !== userId || state.listVersion !== version) return false;
    state.conversations = conversations;
    conversations.forEach(c=>{c.notificationsMuted=state.preferenceSnapshots.absorb(c.id,c.notificationsMuted,c.preferencesVersion);});
    unreadWatermarks.retain(conversations.map(item => item.id));
    const unread = new Map();
    conversations.forEach(conversation => {
      const total = unreadWatermarks.snapshot(conversation);
      if (total) unread.set(Number(conversation.id), total);
    });
    state.unread = unread;
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
  $('#conversation-muted').checked = Boolean(conversation.notificationsMuted);
  $('#conversation-muted').disabled = false;
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
      api(chatApi.getConversationsIdMembers({"id":(conversation.id)})),
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

async function createDirectConversation(peerId) {
  const conversation = await api(chatApi.postConversations({}), {
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

async function loadWorkspaceCapabilities() {
  try {
    state.capabilities=await api(sharedApi.getClientCapabilities({}));
    const uploads=state.capabilities.uploads;
    if(uploads?.chat?.allowed){
      $('#message-file').accept=uploads.chat.allowed.map(type=>type.extension).join(',');
      $('#message-file').title=`文件 ${uploads.chat.maxBytes/1048576} MiB；图片 ${uploads.chat.imageMaxBytes/1048576} MiB`;
      $('#profile-form').elements.avatar.accept=[...new Set(uploads.chat.allowed.flatMap(type=>type.mediaTypes).filter(type=>type.startsWith('image/')))].join(',');
      $('#profile-form').elements.avatar.closest('label').querySelector('small').textContent=`支持的图片类型以上传选择器为准；最大 ${uploads.avatar.maxBytes/1048576} MiB，分片上传。`;
    }
    userPickers.splice(0).forEach(p=>p.dispose());
    for(const [form,name,multiple] of [['friend-request-form','receiverId',false],['direct-conversation-form','peerId',false],['group-conversation-form','memberIds',true],['member-form','memberId',false]]) {
      userPickers.push(new UserPicker($('#'+form).elements[name],api,{multiple}));
    }
  }catch(error){toast('功能说明读取失败：'+error.message,'error');}
}

async function startAnnouncements() {
  runtime.announcementPlayer?.dispose();
  const userId=state.me?.id;
  try {
    const a = new BusinessApi(); await a.restore();
    if(!userId || state.me?.id!==userId || state.leaving)return;
    let box = document.querySelector('.announcements');
    if (!box) { box = document.createElement('div'); box.className = 'announcements'; document.body.append(box); }
    box.replaceChildren(); runtime.announcementPlayer = new AnnouncementPlayer(a, box); await runtime.announcementPlayer.start();
    if(state.me?.id!==userId || state.leaving)runtime.announcementPlayer.dispose();
  } catch {}
}

  return {advanceConversationSequence,conversationTitle,conversationOrder,bumpUnread,clearConversationView,updateProfileView,showChat,showLoggedOut,renderConversations,loadConversations,paintConversationChrome,openConversation,createDirectConversation,openConversationDialog,loadWorkspaceCapabilities,startAnnouncements};
}
