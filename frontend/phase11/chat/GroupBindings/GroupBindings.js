import { chatApi } from "../ChatApi/ChatApi.js?v=dc39cdb5489c964d";
import { createChatScope } from '../ChatScope/ChatScope.js?v=dc39cdb5489c964d';
export const dependencies={"resources":["$","elements","state"],"commands":["setStatus","toast","clearConversationView","setButtonBusy","api","loadConversations","openConversation","refreshCurrentGroup"],"types":[],"events":true};
/** Group UI subscriptions. All listeners belong to the page lifecycle. */
export function bindGroup(context) {
  const scope=createChatScope(context,dependencies);
  const {$,elements,state,listen}=scope;
  const {setStatus,toast,clearConversationView,setButtonBusy,api,loadConversations,openConversation,refreshCurrentGroup}=scope.actions;
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
    await api(chatApi.postConversationsIdMembers({"id":(state.conversation.id)}), {
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
    const updated = await api(chatApi.patchConversationsId({"id":(state.conversation.id)}), {
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
    await api(chatApi.postConversationsIdLeave({"id":(state.conversation.id)}), {
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
    await api(chatApi.postConversationsIdDissolve({"id":(state.conversation.id)}), {
      method: 'POST', body: '{}',
    });
    clearConversationView();
    await loadConversations();
    toast('群聊已解散，历史记录仍保留在数据库中', 'success');
  } catch (error) { toast(error.message, 'error'); }
});
}
