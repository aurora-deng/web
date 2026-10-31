import { accountsApi } from "../AccountsApi/AccountsApi.js?v=dc39cdb5489c964d";
import { createChatScope } from '../../chat/ChatScope/ChatScope.js?v=dc39cdb5489c964d';
export const dependencies={"resources":["$","elements","state"],"commands":["setStatus","toast","setButtonBusy","api","uploadFile","formatBytes","updateProfileView","showChat","showLoggedOut"],"types":[],"events":true};
/** Account UI subscriptions. All listeners belong to the page lifecycle. */
export function bindAccount(context) {
  const scope=createChatScope(context,dependencies);
  const {$,elements,state,listen}=scope;
  const {setStatus,toast,setButtonBusy,api,uploadFile,formatBytes,updateProfileView,showChat,showLoggedOut}=scope.actions;
listen($('#login-form'), 'submit', async event => {
  event.preventDefault();
  // Event.currentTarget 只在浏览器同步派发事件期间可靠。异步处理会跨过
  // await，因此先保存稳定的表单引用，后续不要再从 event 读取表单。
  const form = event.currentTarget;
  const button = form.querySelector('[type="submit"]');
  setButtonBusy(button, true, '正在登录…');
  setStatus(elements.authStatus, '正在验证账号…');
  try {
    const result = await api(accountsApi.postAuthLogin({}), {
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
    await api(accountsApi.postAuthRegister({}), { method: 'POST', body: JSON.stringify(fields) });
    setStatus($('#register-status'), '注册成功，现在可以登录。', 'success');
    $('#login-form').elements.username.value = fields.username;
    form.reset();
    setTimeout(() => elements.registerDialog.close(), 800);
  } catch (error) { setStatus($('#register-status'), error.message, 'error'); }
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
      await api(accountsApi.postMeAvatar({}), {
        method: 'POST', body: JSON.stringify({ fileId: String(uploaded.id) }),
      });
    }
    const result = await api(accountsApi.patchMe({}), {
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


listen($('#logout'), 'click', async () => {
  state.leaving = true;
  try { await api(accountsApi.postAuthLogout({}), { method: 'POST' }); }
  catch (error) { toast(error.message, 'error'); }
  showLoggedOut();
  setStatus(elements.authStatus, '已安全退出。', 'success');
});


(async function restoreSession() {
  try {
    const result = await api(accountsApi.getMe({}));
    state.csrf = result.csrfToken;
    await showChat(result.user);
  } catch (error) {
    if (error.status && error.status !== 401) setStatus(elements.authStatus, error.message, 'error');
  }
}());
}
