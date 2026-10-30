export const dependencies={"resources":["$","elements","state","trainingStateLabels","runtime"],"commands":["setStatus","toast","api","existingMessage","actionListEmpty"],"types":[]};
/** AiController: explicit use cases; shared state/other controllers arrive through context. */
export function createAiController(context) {
  const {$,elements,state,trainingStateLabels,runtime,actions}=context;

  const setStatus = (...args) => actions.setStatus(...args);
  const toast = (...args) => actions.toast(...args);
  const api = (...args) => actions.api(...args);
  const existingMessage = (...args) => actions.existingMessage(...args);
  const actionListEmpty = (...args) => actions.actionListEmpty(...args);
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

function generationElement(generationId) {
  return [...elements.messageList.children].find(item => item.dataset.generationId === String(generationId));
}

function selectAiTarget(userId) {
  const select = elements.messageForm.elements.aiUserId;
  const value = String(userId || '');
  if (!value) { select.value = select.options[0]?.value || ''; return; }
  if (![...select.options].some(option => option.value === value)) {
    select.append(new Option(`会话 AI 成员 #${value}`, value));
  }
  select.value = value;
}

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

  return {openCitationDetails,previousPromptForMessage,openTrainingConsent,renderTrainingCandidates,loadTrainingCandidates,generationElement,selectAiTarget,applyAiModels,loadAiModels,readAiTarget,refreshCancelButton};
}
