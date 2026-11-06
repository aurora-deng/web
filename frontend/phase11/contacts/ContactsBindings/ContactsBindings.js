import { contactsApi } from "../ContactsApi/ContactsApi.js?v=144f1e6009a3d461";
import { createChatScope } from '../../chat/ChatScope/ChatScope.js?v=144f1e6009a3d461';
export const dependencies={"resources":["$","elements"],"commands":["setStatus","setButtonBusy","api","loadContacts"],"types":[],"events":true};
/** Contacts UI subscriptions. All listeners belong to the page lifecycle. */
export function bindContacts(context) {
  const scope=createChatScope(context,dependencies);
  const {$,elements,listen}=scope;
  const {setStatus,setButtonBusy,api,loadContacts}=scope.actions;
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

listen($('#friend-request-form'), 'submit', async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  const receiverId = new FormData(form).get('receiverId');
  setButtonBusy(button, true, '发送中…');
  try {
    await api(contactsApi.postFriendRequests({}), { method: 'POST', body: JSON.stringify({ receiverId }) });
    form.reset();
    setStatus($('#friend-status'), '好友申请已发送。', 'success');
  } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  finally { setButtonBusy(button, false); }
});
}
