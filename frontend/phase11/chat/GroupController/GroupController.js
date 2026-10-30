export const dependencies={"resources":["$","state"],"commands":["toast","initials","api","paintAvatar","loadConversations","openConversation","actionListEmpty"],"types":[]};
/** GroupController: explicit use cases; shared state/other controllers arrive through context. */
export function createGroupController(context) {
  const {$,state,actions}=context;

  const toast = (...args) => actions.toast(...args);
  const initials = (...args) => actions.initials(...args);
  const api = (...args) => actions.api(...args);
  const paintAvatar = (...args) => actions.paintAvatar(...args);
  const loadConversations = (...args) => actions.loadConversations(...args);
  const openConversation = (...args) => actions.openConversation(...args);
  const actionListEmpty = (...args) => actions.actionListEmpty(...args);
function myMembership() {
  return state.members.find(member => Number(member.userId) === Number(state.me?.id));
}

function renderConversationMembers() {
  $('#detail-member-count').textContent = String(state.members.length);
  $('#detail-member-list').replaceChildren(...(state.members.length ? state.members.map(member => {
    const item = document.createElement('li');
  const avatar = document.createElement('span');
  avatar.className = 'avatar';
  paintAvatar(avatar, Number(member.userId) === Number(state.me?.id) ? state.me : member,
    Number(member.userId) === Number(state.me?.id)
      ? '我' : (member.aiAccount ? 'AI' : initials(member.displayName || member.username, `#${member.userId}`)));
    const copy = document.createElement('span');
    copy.className = 'member-copy';
    const name = document.createElement('strong');
    name.textContent = Number(member.userId) === Number(state.me?.id)
      ? (state.me.displayName || state.me.username)
      : `${member.displayName || member.username || `用户 ${member.userId}`}${member.aiAccount ? ' · AI' : ''}`;
    const progress = document.createElement('small');
    const muted = Number(member.mutedUntil) > Date.now();
    progress.textContent = muted
      ? `禁言至 ${new Date(Number(member.mutedUntil)).toLocaleString()}`
      : `已读至 #${member.lastReadSequence || 0}`;
    copy.append(name, progress);
    const role = document.createElement('span');
    role.className = `member-role ${member.role === 'owner' ? 'owner' : member.role === 'administrator' ? 'admin' : ''}`;
    role.textContent = member.role === 'owner' ? '群主'
      : member.role === 'administrator' ? '管理员' : '成员';
    item.append(avatar, copy, role);

    const mine = myMembership();
    const isSelf = Number(member.userId) === Number(state.me?.id);
    const ownerCanAct = mine?.role === 'owner' && member.role !== 'owner' && !isSelf;
    const adminCanAct = mine?.role === 'administrator' && member.role === 'member' && !isSelf;
    if (ownerCanAct || adminCanAct) {
      const actions = document.createElement('span');
      actions.className = 'member-actions';
      if (ownerCanAct) {
        const roleButton = document.createElement('button');
        roleButton.type = 'button';
        roleButton.className = 'mini-button';
        roleButton.textContent = member.role === 'administrator' ? '取消管理员' : '设为管理员';
        roleButton.addEventListener('click', () => updateMemberRole(
          member.userId,
          member.role === 'administrator' ? 'member' : 'administrator',
        ));
        actions.append(roleButton);
      }
      const muteButton = document.createElement('button');
      muteButton.type = 'button';
      muteButton.className = 'mini-button';
      muteButton.textContent = muted ? '解除禁言' : '禁言 10 分钟';
      muteButton.addEventListener('click', () => updateMemberMute(
        member.userId, muted ? 0 : 600,
      ));
      const removeButton = document.createElement('button');
      removeButton.type = 'button';
      removeButton.className = 'mini-button danger';
      removeButton.textContent = '移出群聊';
      removeButton.addEventListener('click', () => removeMember(member));
      actions.append(muteButton, removeButton);
      item.append(actions);
    }
    return item;
  }) : [actionListEmpty('暂无成员信息')]));
}

function renderGroupGovernance() {
  const group = state.conversation?.kind === 'group';
  $('#group-governance').classList.toggle('hidden', !group);
  if (!group) return;
  const mine = myMembership();
  const privileged = ['owner', 'administrator'].includes(mine?.role);
  $('#group-announcement').textContent = state.conversation.announcement || '暂无群公告';
  $('#edit-group-profile').classList.toggle('hidden', !privileged);
  $('#leave-group').classList.toggle('hidden', !mine || mine.role === 'owner');
  $('#dissolve-group').classList.toggle('hidden', mine?.role !== 'owner');
}

async function refreshCurrentGroup(message) {
  toast(message, 'success');
  const currentId = Number(state.conversation?.id);
  await loadConversations();
  const current = state.conversations.find(item => Number(item.id) === currentId);
  if (current) await openConversation(current);
}

async function updateMemberRole(userId, role) {
  if (!state.conversation) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/members/${encodeURIComponent(userId)}/role`, {
      method: 'POST', body: JSON.stringify({ role }),
    });
    await refreshCurrentGroup(role === 'administrator' ? '已设置管理员' : '已取消管理员');
  } catch (error) { toast(error.message, 'error'); }
}

async function updateMemberMute(userId, seconds) {
  if (!state.conversation) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/members/${encodeURIComponent(userId)}/mute`, {
      method: 'POST', body: JSON.stringify({ seconds }),
    });
    await refreshCurrentGroup(seconds ? '成员已禁言 10 分钟' : '成员禁言已解除');
  } catch (error) { toast(error.message, 'error'); }
}

async function removeMember(member) {
  if (!state.conversation || !confirm(`确定将“${member.displayName || member.username}”移出群聊吗？`)) return;
  try {
    await api(`/api/conversations/${encodeURIComponent(state.conversation.id)}/members/${encodeURIComponent(member.userId)}/remove`, {
      method: 'POST', body: '{}',
    });
    await refreshCurrentGroup('成员已移出群聊');
  } catch (error) { toast(error.message, 'error'); }
}

  return {myMembership,renderConversationMembers,renderGroupGovernance,refreshCurrentGroup,updateMemberRole,updateMemberMute,removeMember};
}
