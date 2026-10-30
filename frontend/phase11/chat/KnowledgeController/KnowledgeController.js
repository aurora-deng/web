export const dependencies={"resources":["$","elements","state"],"commands":["setStatus","conversationTitle","api","actionListEmpty"],"types":[]};
/** KnowledgeController: explicit use cases; shared state/other controllers arrive through context. */
export function createKnowledgeController(context) {
  const {$,elements,state,actions}=context;

  const setStatus = (...args) => actions.setStatus(...args);
  const conversationTitle = (...args) => actions.conversationTitle(...args);
  const api = (...args) => actions.api(...args);
  const actionListEmpty = (...args) => actions.actionListEmpty(...args);
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

  return {knowledgeScope,renderKnowledgeDocuments,loadKnowledgeDocuments,updateKnowledgeScopeHint};
}
