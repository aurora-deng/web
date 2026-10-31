import { chatApi } from "../ChatApi/ChatApi.js?v=dc39cdb5489c964d";
import { createChatScope } from '../ChatScope/ChatScope.js?v=dc39cdb5489c964d';
export const dependencies={"resources":["$","elements","state"],"commands":["setStatus","toast","setButtonBusy","api","renderConversations","loadConversations","paintConversationChrome","openConversation","createDirectConversation","openConversationDialog","searchWorkspace"],"types":[],"events":true};
/** Conversation UI subscriptions. All listeners belong to the page lifecycle. */
export function bindConversation(context) {
  const scope=createChatScope(context,dependencies);
  const {$,elements,state,listen}=scope;
  const {setStatus,toast,setButtonBusy,api,renderConversations,loadConversations,paintConversationChrome,openConversation,createDirectConversation,openConversationDialog,searchWorkspace}=scope.actions;
listen($('#show-conversations'), 'click', loadConversations);

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
    const conversation = await api(chatApi.postConversations({}), {
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
    const result=await api(chatApi.patchConversationsIdPreferences({"id":(conversation.id)}),{method:'PATCH',data:{notificationsMuted:String(input.checked)}});
    if(state.me?.id!==user)return;
    const current=state.conversations.find(c=>Number(c.id)===Number(conversation.id));
    const muted=state.preferenceSnapshots.absorb(conversation.id,result.notificationsMuted,result.version);
    if(current)current.notificationsMuted=muted;
    if(Number(state.conversation?.id)===Number(conversation.id))paintConversationChrome(current||conversation);
    renderConversations(); toast(result.notificationsMuted?'已开启免打扰，消息与未读仍正常同步':'已恢复会话提醒');
  }catch(error){input.checked=Boolean(conversation.notificationsMuted);toast(error.message,'error');}
  finally{input.disabled=!state.conversation;}
});
}
