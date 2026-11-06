import { knowledgeApi } from "../KnowledgeApi/KnowledgeApi.js?v=144f1e6009a3d461";
import { createChatScope } from '../../chat/ChatScope/ChatScope.js?v=144f1e6009a3d461';
export const dependencies={"resources":["$","elements","state"],"commands":["setStatus","toast","conversationTitle","setButtonBusy","api","knowledgeScope","loadKnowledgeDocuments","updateKnowledgeScopeHint"],"types":[],"events":true};
/** Knowledge UI subscriptions. All listeners belong to the page lifecycle. */
export function bindKnowledge(context) {
  const scope=createChatScope(context,dependencies);
  const {$,elements,state,listen}=scope;
  const {setStatus,toast,conversationTitle,setButtonBusy,api,knowledgeScope,loadKnowledgeDocuments,updateKnowledgeScopeHint}=scope.actions;
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
    await api(knowledgeApi.postKnowledgeDocuments({}), {
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
}
