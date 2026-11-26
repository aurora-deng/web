import { accountsApi } from '../../accounts/AccountsApi/AccountsApi.js?v=0613976363093f95';
import { profilesApi } from '../ProfilesApi/ProfilesApi.js?v=0613976363093f95';
import { uploadAttachment } from '../../files/AttachmentUpload/AttachmentUpload.js?v=0613976363093f95';
import { uploadCapabilities } from '../../shared/UploadCapabilities/UploadCapabilities.js?v=0613976363093f95';
import { applyTheme } from '../../shared/Theme/Theme.js?v=0613976363093f95';
import { LocalAssistantApi } from '../../shared/LocalAssistantApi/LocalAssistantApi.js?v=0613976363093f95';
import { openSpeechSettings } from '../../shared/SpeechSettings/SpeechSettings.js?v=0613976363093f95';
import { WebSpeechApi } from '../../speech/WebSpeechApi/WebSpeechApi.js?v=0613976363093f95';

/** 本人设置复用账号、动态、上传与语音的原有接口。 */
export function mountProfileSettings(root, api, refreshProfile) {
  const profileForm = root.querySelector('#profile-edit-form');
  const avatarForm = root.querySelector('#profile-avatar-form');
  const status = root.querySelector('#settings-status');
  const theme = root.querySelector('#profile-theme');
  let enabled = false;

  async function run(button, action) {
    if (!enabled || button.disabled) return;
    button.disabled = true;
    status.textContent = '';
    try { await action(); }
    catch (error) {
      status.textContent = error.status === 401 ? '登录已过期，请返回缘续-聊天重新登录。' : error.message;
    } finally { button.disabled = false; }
  }

  profileForm.onsubmit = event => {
    event.preventDefault();
    run(profileForm.querySelector('[type=submit]'), async () => {
      const fields = profileForm.elements;
      const result = await api.request(accountsApi.patchMe({}), { method: 'PATCH', data: {
        displayName: fields.displayName.value.trim(), biography: fields.biography.value.trim(),
      } });
      window.dispatchEvent(new CustomEvent('nebula:profile-updated', { detail: result.user }));
      await api.request(profilesApi.patchMeProfileDisplay({}), { method: 'PATCH', data: {
        showBiography: String(fields.showBiography.checked),
        showAvatar: String(fields.showAvatar.checked),
        allowTreeFriendRequests: String(fields.allowTreeFriendRequests.checked),
      } });
      await refreshProfile();
      status.textContent = '个人资料和隐私设置已保存。';
    });
  };

  avatarForm.onsubmit = event => {
    event.preventDefault();
    run(avatarForm.querySelector('[type=submit]'), async () => {
      const input = avatarForm.elements.avatar;
      const file = input.files[0];
      if (!file?.size) throw new Error('请选择一张图片。');
      const uploads = await uploadCapabilities(api);
      const imageAllowed = file.type.startsWith('image/') &&
        uploads.chat.allowed.some(type => type.mediaTypes.includes(file.type));
      if (!imageAllowed || file.size > uploads.avatar.maxBytes)
        throw new Error(`请选择支持的图片，大小不超过 ${uploads.avatar.maxBytes / 1048576} MiB。`);
      status.textContent = '正在上传头像…';
      const uploaded = await uploadAttachment(file, {
        request: api.request.bind(api), csrf: api.csrf, chunkSize: uploads.chat.chunkBytes,
      });
      await api.request(accountsApi.postMeAvatar({}), { method: 'POST', data: { fileId: String(uploaded.id) } });
      input.value = '';
      await refreshProfile();
      status.textContent = '头像已更新。';
    });
  };

  root.querySelector('[data-speech-settings]').onclick = event => run(event.currentTarget, async () => {
    const local = new LocalAssistantApi();
    const capabilities = await local.start();
    await openSpeechSettings(capabilities.available ? local : new WebSpeechApi(api));
  });
  root.querySelector('[data-personal-assistant-settings]').onclick = () => {
    if (enabled) window.dispatchEvent(new Event('nebula:assistant-settings'));
  };
  theme.onchange = () => { if (enabled) applyTheme(theme.value); };
  theme.value = document.documentElement.dataset.theme || 'dark';
  window.addEventListener('nebula:theme', event => { theme.value = event.detail; });

  return {
    notificationPanel: root.querySelector('[data-notification-preferences]'),
    setProfile(profile) {
      enabled = Boolean(profile);
      if (!profile) return;
      for (const field of ['username', 'displayName', 'biography']) {
        profileForm.elements[field].value = profile[field] || '';
      }
      for (const field of ['showBiography', 'showAvatar', 'allowTreeFriendRequests']) {
        profileForm.elements[field].checked = profile[field] === true;
      }
    },
  };
}
