/** Static DOM subscriptions; dispose aborts every listener registered here. */
export function bindChat(context) {
  const {businessApi,userPickers,unreadWatermarks,pageBudget,wsBackoff,sseBackoff,$,elements,state,messageView,history,sender,trainingStateLabels,runtime,actions,types}=context;
  const {mountNavigation,UserPicker,groupMessages,BusinessApi,AnnouncementPlayer,UnreadWatermarks,ChatHistory,canRecallMessage,ReliableSender,VirtualMessageList,RecoveryBudget,ReconnectBackoff,formatChatTime,formatDayKey,formatDateDivider}=types;
  const setStatus = (...args) => actions.setStatus(...args);
  const toast = (...args) => actions.toast(...args);
  const createClientId = (...args) => actions.createClientId(...args);
  const conversationTitle = (...args) => actions.conversationTitle(...args);
  const clearConversationView = (...args) => actions.clearConversationView(...args);
  const setButtonBusy = (...args) => actions.setButtonBusy(...args);
  const api = (...args) => actions.api(...args);
  const declaredMediaType = (...args) => actions.declaredMediaType(...args);
  const uploadFile = (...args) => actions.uploadFile(...args);
  const formatBytes = (...args) => actions.formatBytes(...args);
  const updateProfileView = (...args) => actions.updateProfileView(...args);
  const setRealtimeState = (...args) => actions.setRealtimeState(...args);
  const showChat = (...args) => actions.showChat(...args);
  const showLoggedOut = (...args) => actions.showLoggedOut(...args);
  const renderConversations = (...args) => actions.renderConversations(...args);
  const loadConversations = (...args) => actions.loadConversations(...args);
  const paintConversationChrome = (...args) => actions.paintConversationChrome(...args);
  const openConversation = (...args) => actions.openConversation(...args);
  const refreshCurrentGroup = (...args) => actions.refreshCurrentGroup(...args);
  const openTrainingConsent = (...args) => actions.openTrainingConsent(...args);
  const loadTrainingCandidates = (...args) => actions.loadTrainingCandidates(...args);
  const appendMessage = (...args) => actions.appendMessage(...args);
  const failPendingMessage = (...args) => actions.failPendingMessage(...args);
  const sendSocket = (...args) => actions.sendSocket(...args);
  const loadAiModels = (...args) => actions.loadAiModels(...args);
  const readAiTarget = (...args) => actions.readAiTarget(...args);
  const refreshCancelButton = (...args) => actions.refreshCancelButton(...args);
  const loadContacts = (...args) => actions.loadContacts(...args);
  const createDirectConversation = (...args) => actions.createDirectConversation(...args);
  const openConversationDialog = (...args) => actions.openConversationDialog(...args);
  const showSelectedFile = (...args) => actions.showSelectedFile(...args);
  const knowledgeScope = (...args) => actions.knowledgeScope(...args);
  const loadKnowledgeDocuments = (...args) => actions.loadKnowledgeDocuments(...args);
  const updateKnowledgeScopeHint = (...args) => actions.updateKnowledgeScopeHint(...args);
  const searchWorkspace = (...args) => actions.searchWorkspace(...args);

  const listen=(target,type,handler,options={})=>target.addEventListener(type,handler,{...options,signal:runtime.lifecycle.signal});
listen($('#login-form'), 'submit', async event => {
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

listen($('#show-register'), 'click', () => {
  setStatus($('#register-status'));
  elements.registerDialog.showModal();
});

listen($('#register-form'), 'submit', async event => {
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


listen($('#message-file'), 'change', event => {
  const file = event.currentTarget.files?.[0] || null;
  if (file && file.size > (state.capabilities?.uploads.chat.maxBytes || 25 * 1024 * 1024)) {
    toast(`文件不能超过 ${formatBytes(state.capabilities?.uploads.chat.maxBytes || 25 * 1024 * 1024)}`, 'error');
    event.currentTarget.value = '';
    showSelectedFile(null);
    return;
  }
  showSelectedFile(file);
});

listen($('#clear-selected-file'), 'click', () => {
  $('#message-file').value = '';
  showSelectedFile(null);
});

listen(elements.messageForm, 'submit', async event => {
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

listen(elements.messageForm.elements.message, 'keydown', event => {
  if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
    event.preventDefault();
    elements.messageForm.requestSubmit();
  }
});
listen(elements.messageForm.elements.message, 'input', event => {
  event.target.style.height = 'auto';
  event.target.style.height = `${Math.min(event.target.scrollHeight, 150)}px`;
});

listen($('#toggle-ai'), 'click', event => {
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
listen(elements.messageForm.elements.aiUserId, 'change', () => {
  state.aiTargetManual = true;
});

listen($('#cancel-generation'), 'click', event => {
  const generationId = event.currentTarget.dataset.generationId;
  if (!generationId) return;
  sendSocket({ v: 1, type: 'ai.cancel', id: createClientId(), replyTo: generationId });
});





listen($('#show-conversations'), 'click', loadConversations);
listen($('#show-friends'), 'click', async () => {
  elements.friendDialog.showModal();
  await loadContacts();
});
listen($('#manage-friends'), 'click', async () => {
  elements.friendDialog.showModal();
  await loadContacts();
});
listen($('#show-requests'), 'click', async () => {
  elements.friendDialog.showModal();
  await loadContacts();
  $('#friend-request-list').scrollIntoView({ block: 'nearest' });
});
listen($('#show-knowledge'), 'click', async () => {
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
listen($('#knowledge-form').elements.scope, 'change', async () => {
  updateKnowledgeScopeHint();
  await loadKnowledgeDocuments();
});
listen($('#knowledge-form'), 'submit', async event => {
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
listen($('#show-training'), 'click', () => openTrainingConsent());
listen($('#training-form'), 'submit', async event => {
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
listen($('#show-model-status'), 'click', () => {
  setRealtimeState('ws', state.realtime.ws);
  setRealtimeState('sse', state.realtime.sse);
  elements.modelDialog.showModal();
});
listen($('#new-conversation'), 'click', openConversationDialog);
listen($('#new-direct-conversation'), 'click', () => {
  openConversationDialog();
  $('#direct-conversation-form').elements.peerId.focus();
});
listen($('#new-group-conversation'), 'click', () => {
  openConversationDialog();
  $('#group-conversation-form').elements.title.focus();
});
listen($('#empty-create-conversation'), 'click', openConversationDialog);
listen($('#welcome-create-conversation'), 'click', openConversationDialog);

listen($('#conversation-filter-input'), 'input', event => {
  state.conversationQuery = event.currentTarget.value.trim();
  renderConversations();
});
document.querySelectorAll('[data-conversation-filter]').forEach(button => {
  listen(button, 'click', () => {
    state.conversationFilter = button.dataset.conversationFilter;
    document.querySelectorAll('[data-conversation-filter]').forEach(item => {
      item.classList.toggle('active', item === button);
    });
    renderConversations();
  });
});

listen($('#open-global-search'), 'click', () => {
  if (!state.conversation) {
    toast('请先选择一个会话；当前业务接口按会话权限执行搜索。');
    return;
  }
  $('#search-messages').click();
});

listen($('#toggle-details'), 'click', () => elements.detailPane.classList.toggle('open'));
listen($('#close-details'), 'click', () => elements.detailPane.classList.remove('open'));
listen($('#mobile-back'), 'click', () => elements.chatPanel.classList.remove('conversation-open'));

listen(document, 'keydown', event => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k' && state.me) {
    event.preventDefault();
    $('#conversation-filter-input').focus();
  }
  if (event.key === 'Escape') elements.detailPane.classList.remove('open');
});

listen($('#friend-request-form'), 'submit', async event => {
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

listen($('#direct-conversation-form'), 'submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const peerId = new FormData(form).get('peerId');
  setButtonBusy(button, true, '创建中…');
  try { await createDirectConversation(peerId); form.reset(); }
  catch (error) { setStatus($('#conversation-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});

listen($('#group-conversation-form'), 'submit', async event => {
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

listen($('#show-profile'), 'click', () => {
  $('#profile-form').elements.displayName.value = state.me?.displayName || '';
  $('#profile-form').elements.biography.value = state.me?.biography || '';
  $('#profile-id-hint').textContent = `@${state.me?.username} · 用户 ID ${state.me?.id}`;
  setStatus($('#profile-status'));
  elements.profileDialog.showModal();
});

listen($('#profile-form'), 'submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  setButtonBusy(button, true, '保存中…');
  try {
    const avatar = form.elements.avatar.files?.[0];
    if (avatar) {
      if (avatar.size > state.capabilities.uploads.avatar.maxBytes) throw new Error(`头像不能超过 ${formatBytes(state.capabilities.uploads.avatar.maxBytes)}`);
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

listen($('#search-messages'), 'click', () => {
  $('#search-results').replaceChildren();
  setStatus($('#search-status'));
  elements.searchDialog.showModal();
  $('#search-form').elements.query.focus();
});

listen($('#search-form'), 'submit', event => {
  event.preventDefault(); state.search.cursor='0';
  state.search.query=event.currentTarget.elements.query.value.trim();
  state.search.conversation=event.currentTarget.elements.scope.value==='all'?'0':String(state.conversation?.id||'0');
  searchWorkspace(false);
});
listen($('#search-more'), 'click',()=>searchWorkspace(true));
listen($('#conversation-muted'), 'change',async event=>{
  const input=event.currentTarget, conversation=state.conversation, user=state.me?.id;
  if(!conversation) return; input.disabled=true;
  try {
    const result=await api(`/api/conversations/${conversation.id}/preferences`,{method:'PATCH',data:{notificationsMuted:String(input.checked)}});
    if(state.me?.id!==user)return;
    const current=state.conversations.find(c=>Number(c.id)===Number(conversation.id));
    const muted=state.preferenceSnapshots.absorb(conversation.id,result.notificationsMuted,result.version);
    if(current)current.notificationsMuted=muted;
    if(Number(state.conversation?.id)===Number(conversation.id))paintConversationChrome(current||conversation);
    renderConversations(); toast(result.notificationsMuted?'已开启免打扰，消息与未读仍正常同步':'已恢复会话提醒');
  }catch(error){input.checked=Boolean(conversation.notificationsMuted);toast(error.message,'error');}
  finally{input.disabled=!state.conversation;}
});

listen($('#add-member'), 'click', () => {
  setStatus($('#member-status'));
  elements.memberDialog.showModal();
});

listen($('#member-form'), 'submit', async event => {
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

listen($('#edit-group-profile'), 'click', () => {
  if (!state.conversation) return;
  const form = $('#group-profile-form');
  form.elements.title.value = state.conversation.title || '';
  form.elements.announcement.value = state.conversation.announcement || '';
  setStatus($('#group-profile-status'));
  elements.groupProfileDialog.showModal();
});

listen($('#group-profile-form'), 'submit', async event => {
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

listen($('#leave-group'), 'click', async () => {
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

listen($('#dissolve-group'), 'click', async () => {
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

listen($('#logout'), 'click', async () => {
  state.leaving = true;
  try { await api('/api/auth/logout', { method: 'POST' }); }
  catch (error) { toast(error.message, 'error'); }
  showLoggedOut();
  setStatus(elements.authStatus, '已安全退出。', 'success');
});

document.querySelectorAll('[data-close-dialog]').forEach(button => {
  listen(button, 'click', () => button.closest('dialog').close());
});
document.querySelectorAll('dialog').forEach(dialog => {
  listen(dialog, 'click', event => {
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


/** Capability discovery happens once per login; missing feature is visible, never silently green. */


}
