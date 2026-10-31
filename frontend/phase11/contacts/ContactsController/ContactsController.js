import { contactsApi } from "../ContactsApi/ContactsApi.js?v=dc39cdb5489c964d";
export const dependencies={"resources":["$","elements","state"],"commands":["setStatus","toast","initials","setButtonBusy","api","paintAvatar","loadConversations","actionListEmpty","createDirectConversation"],"types":[]};
/** ContactsController: explicit use cases; shared state/other controllers arrive through context. */
export function createContactsController(context) {
  const {$,elements,state,actions}=context;

  const setStatus = (...args) => actions.setStatus(...args);
  const toast = (...args) => actions.toast(...args);
  const initials = (...args) => actions.initials(...args);
  const setButtonBusy = (...args) => actions.setButtonBusy(...args);
  const api = (...args) => actions.api(...args);
  const paintAvatar = (...args) => actions.paintAvatar(...args);
  const loadConversations = (...args) => actions.loadConversations(...args);
  const actionListEmpty = (...args) => actions.actionListEmpty(...args);
  const createDirectConversation = (...args) => actions.createDirectConversation(...args);
function contactName(user) {
  return user.remark || user.displayName || user.username;
}

function contactPresence(user) {
  if (user.online) {
    const devices = Math.max(1, Number(user.connections) || 1);
    return `在线 · ${devices} 个连接`;
  }
  if (user.lastActiveAt) {
    return `最后活跃 ${new Date(Number(user.lastActiveAt)).toLocaleString()}`;
  }
  return '离线 · 暂无活跃记录';
}

async function saveContact(user, changes) {
  await api(contactsApi.patchFriendsId({"id":(user.id)}), {
    method: 'PATCH',
    body: JSON.stringify({
      remark: changes.remark ?? user.remark ?? '',
      notificationsMuted: String(
        changes.notificationsMuted ?? Boolean(user.notificationsMuted),
      ),
    }),
  });
  await loadContacts();
  // 备注会改变会话摘要里的 displayTitle（SQL: PostgresStore.cpp:1288），
  // 免打扰会改变 notificationsMuted。不重拉会话列表，列表和会话头就会一直显示旧值。
  await loadConversations();
}

function contactRow(user) {
  const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  paintAvatar(avatar, user, initials(user.displayName || user.username));
  const copy = document.createElement('span');
  copy.className = 'item-copy';
  const name = document.createElement('strong');
  name.textContent = contactName(user);
  const detail = document.createElement('small');
  detail.className = `contact-presence ${user.online ? 'online' : ''}`;
  detail.textContent = contactPresence(user);
  const biography = document.createElement('small');
  biography.className = 'contact-biography';
  biography.textContent = `@${user.username} · ID ${user.id}${user.notificationsMuted ? ' · 已免打扰' : ''}`;
  copy.append(name, detail, biography);
  const actions = document.createElement('span');
  actions.className = 'mini-actions contact-actions';
  const chat = document.createElement('button');
  chat.type = 'button';
  chat.className = 'mini-button';
  chat.textContent = '私聊';
  chat.addEventListener('click', () => openFriendConversation(user, chat));
  const remark = document.createElement('button');
  remark.type = 'button';
  remark.className = 'mini-button';
  remark.textContent = '备注';
  remark.addEventListener('click', async () => {
    const value = window.prompt('设置仅自己可见的好友备注（留空可清除）', user.remark || '');
    if (value === null) return;
    try { await saveContact(user, { remark: value.trim() }); }
    catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  const mute = document.createElement('button');
  mute.type = 'button';
  mute.className = 'mini-button';
  mute.textContent = user.notificationsMuted ? '取消免打扰' : '免打扰';
  mute.addEventListener('click', async () => {
    try { await saveContact(user, { notificationsMuted: !user.notificationsMuted }); }
    catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  const remove = document.createElement('button');
  remove.type = 'button';
  remove.className = 'mini-button danger';
  remove.textContent = '删除';
  remove.addEventListener('click', async () => {
    if (!window.confirm(`确定删除联系人“${contactName(user)}”吗？历史消息会保留。`)) return;
    try {
      await api(contactsApi.deleteFriendsId({"id":(user.id)}), { method: 'DELETE' });
      await loadContacts();
    } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  const block = document.createElement('button');
  block.type = 'button';
  block.className = 'mini-button danger';
  block.textContent = '拉黑';
  block.addEventListener('click', async () => {
    if (!window.confirm(`拉黑“${contactName(user)}”会同时解除好友关系，是否继续？`)) return;
    try {
      await api(contactsApi.postBlocksId({"id":(user.id)}), { method: 'POST' });
      await loadContacts();
    } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  actions.append(chat, remark, mute, remove, block);
  item.append(avatar, copy, actions);
  return item;
}

function sidebarFriendRow(user) {
  const item = document.createElement('li');
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'sidebar-friend';
  button.title = `给 ${contactName(user)} 发送消息`;

  const avatar = document.createElement('span');
  avatar.className = 'avatar avatar-friend';
  paintAvatar(avatar, user, initials(user.displayName || user.username));
  const copy = document.createElement('span');
  copy.className = 'friend-copy';
  const name = document.createElement('strong');
  name.textContent = contactName(user);
  const detail = document.createElement('small');
  detail.className = `contact-presence ${user.online ? 'online' : ''}`;
  detail.textContent = contactPresence(user);
  const biography = document.createElement('small');
  biography.className = 'friend-biography';
  biography.textContent = `@${user.username}${user.notificationsMuted ? ' · 免打扰' : ''}`;
  const action = document.createElement('span');
  action.className = 'friend-chat-action';
  action.textContent = '私聊';
  copy.append(name, detail, biography);
  button.append(avatar, copy, action);
  button.addEventListener('click', () => openFriendConversation(user, button));
  item.append(button);
  return item;
}

function blockedRow(user) {
  const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  paintAvatar(avatar, user, initials(user.displayName || user.username));
  const copy = document.createElement('span');
  copy.className = 'item-copy';
  const name = document.createElement('strong');
  name.textContent = user.displayName || user.username;
  const detail = document.createElement('small');
  detail.textContent = `@${user.username} · ID ${user.id}`;
  copy.append(name, detail);
  const button = document.createElement('button');
  button.type = 'button';
  button.className = 'mini-button';
  button.textContent = '解除';
  button.addEventListener('click', async () => {
    try {
      await api(contactsApi.deleteBlocksId({"id":(user.id)}), { method: 'DELETE' });
      await loadContacts();
    } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
  });
  item.append(avatar, copy, button);
  return item;
}

function renderContactLists() {
  $('#friend-count').textContent = String(state.friends.length);
  $('#blocked-count').textContent = String(state.blocked.length);
  $('#friend-list').replaceChildren(...(state.friends.length
    ? state.friends.map(contactRow) : [actionListEmpty('还没有好友')]));
  $('#blocked-list').replaceChildren(...(state.blocked.length
    ? state.blocked.map(blockedRow) : [actionListEmpty('黑名单为空')]));
}

function renderFriendPreview() {
  $('#sidebar-friend-count').textContent = String(state.friends.length);
  const rows = state.friends.length
    ? state.friends.map(sidebarFriendRow)
    : [actionListEmpty('还没有好友，点击“管理”添加')];
  elements.sidebarFriendList.replaceChildren(...rows);
}

async function loadFriendsPreview() {
  if (!state.me) return;
  try {
    state.friends = await api(contactsApi.getFriends({}));
    renderFriendPreview();
  } catch (error) {
    elements.sidebarFriendList.replaceChildren(actionListEmpty(error.message));
  }
}

async function openFriendConversation(user, button) {
  setButtonBusy(button, true, '打开中…');
  try {
    await createDirectConversation(user.id);
  } catch (error) {
    setStatus(elements.chatStatus, error.message, 'error');
    toast(error.message, 'error');
  } finally {
    setButtonBusy(button, false);
  }
}

function friendRequestRow(request) {
  const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  avatar.textContent = `#${request.senderId}`;
  const copy = document.createElement('span');
  copy.className = 'item-copy';
  const name = document.createElement('strong');
  name.textContent = `用户 ${request.senderId}`;
  const detail = document.createElement('small');
  detail.textContent = `申请编号 ${request.id}`;
  copy.append(name, detail);
  const actions = document.createElement('span');
  actions.className = 'mini-actions';
  for (const [label, accept] of [['接受', 'true'], ['拒绝', 'false']]) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `mini-button ${accept === 'false' ? 'danger' : ''}`;
    button.textContent = label;
    button.addEventListener('click', async () => {
      try {
        await api(contactsApi.postFriendRequestsId({"id":(request.id)}), {
          method: 'POST', body: JSON.stringify({ accept }),
        });
        toast(accept === 'true' ? '已接受好友申请' : '已拒绝好友申请');
        await loadContacts();
      } catch (error) { setStatus($('#friend-status'), error.message, 'error'); }
    });
    actions.append(button);
  }
  item.append(avatar, copy, actions);
  return item;
}

async function loadContacts() {
  const status = $('#friend-status');
  setStatus(status, '正在加载…');
  try {
    const [friends, requests, blocked] = await Promise.all([
      api(contactsApi.getFriends({})), api(contactsApi.getFriendRequests({})), api(contactsApi.getBlocks({})),
    ]);
    state.friends = friends;
    state.blocked = blocked;
    renderFriendPreview();
    $('#request-count').textContent = String(requests.length);
    renderContactLists();
    $('#friend-request-list').replaceChildren(...(requests.length ? requests.map(friendRequestRow) : [actionListEmpty('没有待处理申请')]));
    $('#friend-badge').textContent = String(requests.length);
    $('#friend-badge').classList.toggle('hidden', requests.length === 0);
    setStatus(status);
  } catch (error) { setStatus(status, error.message, 'error'); }
}

  return {contactName,contactPresence,saveContact,contactRow,sidebarFriendRow,blockedRow,renderContactLists,renderFriendPreview,loadFriendsPreview,openFriendConversation,friendRequestRow,loadContacts};
}
