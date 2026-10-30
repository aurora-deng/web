export const dependencies={"resources":["$","elements","state","messageView","history","sender","runtime"],"commands":["settleHistoryPending","recoverRealtime","setStatus","initials","createClientId","conversationTitle","api","formatBytes","paintAvatar","openConversation","openCitationDetails","openTrainingConsent","sendSocket","selectAiTarget","applyAiModels","loadAiModels"],"types":["groupMessages","canRecallMessage","formatChatTime","formatDateDivider"]};
/** MessageController: explicit use cases; shared state/other controllers arrive through context. */
export function createMessageController(context) {
  const {$,elements,state,messageView,history,sender,runtime,actions,types}=context;
  const {groupMessages,canRecallMessage,formatChatTime,formatDateDivider}=types;
  const settleHistoryPending = (...args) => actions.settleHistoryPending(...args);
  const recoverRealtime = (...args) => actions.recoverRealtime(...args);
  const setStatus = (...args) => actions.setStatus(...args);
  const initials = (...args) => actions.initials(...args);
  const createClientId = (...args) => actions.createClientId(...args);
  const conversationTitle = (...args) => actions.conversationTitle(...args);
  const api = (...args) => actions.api(...args);
  const formatBytes = (...args) => actions.formatBytes(...args);
  const paintAvatar = (...args) => actions.paintAvatar(...args);
  const openConversation = (...args) => actions.openConversation(...args);
  const openCitationDetails = (...args) => actions.openCitationDetails(...args);
  const openTrainingConsent = (...args) => actions.openTrainingConsent(...args);
  const sendSocket = (...args) => actions.sendSocket(...args);
  const selectAiTarget = (...args) => actions.selectAiTarget(...args);
  const applyAiModels = (...args) => actions.applyAiModels(...args);
  const loadAiModels = (...args) => actions.loadAiModels(...args);
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

function withDateDividers(rows) { return groupMessages(rows); }

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
  item.classList.toggle('message-grouped', Boolean(message.grouped) && !options.windowStart);
  item.classList.toggle('message-group-next', Boolean(message.continuesGroup));
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

function renderCurrentMessages(mode = 'preserve') {
  if (!state.conversation || history.conversationId !== Number(state.conversation.id)) return;
  // 跳转定位期间连翻十几页历史，逐页重绘会让列表抖动并打断高亮，这里只屏蔽中间过程。
  if (runtime.suppressMessageRender) return;
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

function existingMessage(messageId) {
  return [...elements.messageList.children].find(item => item.dataset.messageId === String(messageId));
}

function clearSearchHit() {
  if (state.searchHit?.timer) clearTimeout(state.searchHit.timer);
  state.searchHit = null;
  elements.messageList.querySelectorAll('.search-hit')
    .forEach(node => node.classList.remove('search-hit'));
}

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

async function jumpToMessage(message) {
  if (!message || !state.me) return;
  const userId=state.me.id;
  if (Number(state.conversation?.id)!==Number(message.conversationId)) {
    const target=state.conversations.find(c=>Number(c.id)===Number(message.conversationId));
    if (!target) throw new Error('会话已不可访问，请刷新列表');
    await openConversation(target);
  }
  const version=state.viewVersion, conversation=state.conversation;
  const context=await api(`/api/conversations/${conversation.id}/messages/${message.id}/context?radius=30`);
  if(version!==state.viewVersion||state.me?.id!==userId) return;
  if(!history.focusContext(context.items,conversation.id)) return;
  elements.searchDialog.close(); renderCurrentMessages(); revealMessageRow(message.id);
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

function appendMessage(message) {
  if (!message) return;
  if (message.id) {
    if (Number(message.senderId) === Number(state.me?.id)) state.transientMessages.delete(message.clientMessageId);
    if (Number(message.conversationId) === history.conversationId) history.ingest(message);
    rememberMessageId(message.id);
  } else if (message.clientMessageId) state.transientMessages.set(message.clientMessageId, { ...message });
  renderCurrentMessages('follow');
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

async function searchWorkspace(append) {
  const search=state.search, version=++search.version, user=state.me?.id;
  const controller=new AbortController(); search.controller?.abort(); search.controller=controller;
  if(!append)$('#search-results').replaceChildren();
  $('#search-more').disabled=true;setStatus($('#search-status'),'正在搜索…');
  try {
    const page=await api('/api/search/messages/page?'+new URLSearchParams({q:search.query,conversationId:search.conversation,before:append?search.cursor:'0',limit:30}),{signal:controller.signal});
    if(version!==search.version||state.me?.id!==user)return;
    search.cursor=page.nextCursor;
    for(const message of page.items){
      const item=document.createElement('li'),row=document.createElement('button');row.type='button';row.className='search-result';
      const text=document.createElement('p'),meta=document.createElement('small');
      text.textContent=message.body||'附件消息';
      meta.textContent=`${conversationTitle(state.conversations.find(c=>Number(c.id)===Number(message.conversationId))||{id:message.conversationId})} · ${formatChatTime(message.createdAt)}`;
      row.append(text,meta);row.onclick=async()=>{row.disabled=true;try{await jumpToMessage(message);}catch(error){setStatus($('#search-status'),error.message,'error');}finally{row.disabled=false;}};
      item.append(row);$('#search-results').append(item);
    }
    setStatus($('#search-status'),$('#search-results').children.length?'按最新消息排序；可继续翻页或点选定位':'没有匹配消息');
    $('#search-more').disabled=search.cursor==='0';
  }catch(error){if(version===search.version&&error.name!=='AbortError')setStatus($('#search-status'),error.message,'error');}
}

  return {rememberMessageId,withDateDividers,deliveryLabel,canRecall,requestRecall,createMessageElement,renderCurrentMessages,scrollMessages,existingMessage,clearSearchHit,ensureSequenceLoaded,revealMessageRow,jumpToMessage,messageByClientId,refreshMessageReceipts,appendMessage,applyRecall,failPendingMessage,searchWorkspace};
}
