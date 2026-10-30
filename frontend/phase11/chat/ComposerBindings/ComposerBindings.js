import { createChatScope } from '../ChatScope/ChatScope.js?v=227af12a6c379d08';
export const dependencies={"resources":["$","elements","state","sender"],"commands":["setStatus","toast","createClientId","setButtonBusy","api","declaredMediaType","uploadFile","formatBytes","appendMessage","failPendingMessage","sendSocket","readAiTarget","refreshCancelButton","showSelectedFile"],"types":[],"events":true};
/** Composer UI subscriptions. All listeners belong to the page lifecycle. */
export function bindComposer(context) {
  const scope=createChatScope(context,dependencies);
  const {$,elements,state,sender,listen}=scope;
  const {setStatus,toast,createClientId,setButtonBusy,api,declaredMediaType,uploadFile,formatBytes,appendMessage,failPendingMessage,sendSocket,readAiTarget,refreshCancelButton,showSelectedFile}=scope.actions;
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
}
