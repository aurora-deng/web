import { BusinessApi } from '../../shared/BusinessApi/BusinessApi.js?v=1f08ad808b467825';
import { mountNavigation } from '../../shared/AppNavigation/AppNavigation.js?v=1f08ad808b467825';
import { adminApi } from '../AdminApi/AdminApi.js?v=1f08ad808b467825';
import { sharedApi } from '../../shared/SharedApi/SharedApi.js?v=1f08ad808b467825';
const api = new BusinessApi(), $ = id => document.getElementById(id);
let rows = [], cursor = '0', pending, loading = false, writing = false, generation = 0, currentUser;
const roleNames = {user:'普通用户', moderator:'内容管理员', administrator:'站点管理员'};
const node = (tag, text) => {const el = document.createElement(tag); el.textContent = text; return el;};
function status(text, error = false) {$('status').textContent = text; $('status').toggleAttribute('data-error', error);}
function deny(message) {generation++; rows = []; pending = undefined; $('people').replaceChildren(); $('workspace').hidden = true; if ($('confirm').open) $('confirm').close(); status(message, true);}
function fail(error) {if (error.status === 401 || error.status === 403) deny('登录已失效或管理权限已变更，请重新登录。'); else status(error.message, true);}
function button(label, run) {const el = node('button', label); el.type = 'button'; el.disabled = writing; el.onclick = run; return el;}
function propose(user, action, value, label) {
  if (writing) return;
  pending = {user, action, value}; $('change-title').textContent = label;
  $('change-target').textContent = `${user.name} (@${user.username} / #${user.id})`;
  $('reason').value = ''; $('confirm').showModal(); $('reason').focus();
}
function render() {
  const term = $('filter').value.trim().toLocaleLowerCase();
  const selected = rows.filter(u => [u.id, u.name, u.username].some(v => String(v).toLocaleLowerCase().includes(term)));
  $('people').replaceChildren();
  for (const user of selected) {
    const row = document.createElement('tr'), identity = node('td', user.name);
    identity.append(node('small', `@${user.username} · #${user.id}${String(user.id) === String(currentUser) ? ' · 当前账号' : ''}`));
    const role = document.createElement('td'), select = document.createElement('select'); select.setAttribute('aria-label', `${user.username} 的站点角色`);
    for (const [value, label] of Object.entries(roleNames)) {const option = node('option', label); option.value = value; select.append(option);} select.value = user.role; select.disabled = writing; role.append(select);
    const state = document.createElement('td'), badge = node('span', user.active ? '正常' : '已停用'); badge.className = 'badge' + (user.active ? '' : ' disabled'); state.append(badge);
    const actions = document.createElement('td');
    actions.append(button('应用角色', () => {if (select.value !== user.role) propose(user, 'role', select.value, `授予${roleNames[select.value]}`);}),
      button(user.active ? '停用账号' : '启用账号', () => propose(user, 'active', !user.active, user.active ? '停用账号' : '启用账号')),
      button('吊销会话', () => propose(user, 'revoke', undefined, '吊销全部登录会话')));
    row.append(identity, role, state, actions); $('people').append(row);
  }
  $('empty').hidden = selected.length !== 0; $('count').textContent = `已加载 ${rows.length} 人 · 当前显示 ${selected.length} 人`;
}
async function load(reset = false) {
  if (loading || writing) return;
  loading = true; const ticket = ++generation; $('refresh').disabled = true; $('more').disabled = true;
  try {
    const page = await api.request(adminApi.getAdminUsers({query:{after:reset ? '0' : cursor}}));
    if (ticket !== generation) return;
    const existing = new Map((reset ? [] : rows).map(u => [String(u.id), u])); for (const user of page) existing.set(String(user.id), user); rows = [...existing.values()];
    cursor = String(page.at(-1)?.id || '0'); $('more').hidden = page.length < 50; render(); status('权限由服务器实时校验；所有变更需要填写原因。');
  } catch (error) {fail(error);} finally {loading = false; $('refresh').disabled = false; $('more').disabled = false;}
}
$('filter').oninput = render; $('refresh').onclick = () => load(true); $('more').onclick = () => load();
$('cancel').onclick = () => {if (!writing) {$('confirm').close(); pending = undefined;}};
$('confirm').addEventListener('cancel', event => {if (writing) event.preventDefault(); else pending = undefined;});
$('change-form').onsubmit = async event => {
  event.preventDefault(); if (!pending || writing) return;
  const reason = $('reason').value.trim(); if (!reason || [...reason].length > 200) {status('请填写 1–200 字操作原因。', true); return;}
  const change = pending; writing = true; $('submit').disabled = true; $('cancel').disabled = true; render();
  const data = {action:change.action, reason}; if (change.action === 'role') data.role = change.value; if (change.action === 'active') data.active = String(change.value);
  let done = false;
  try {await api.request(adminApi.postAdminUsersUser({path:{user:change.user.id}}), {data}); done = true; pending = undefined; $('confirm').close();}
  catch (error) {if (error.uncertain) {pending = undefined; $('confirm').close(); status('网络中断，变更结果尚未确认。请先刷新人员列表和审计记录，不要重复提交。', true);} else fail(error);}
  finally {writing = false; $('submit').disabled = false; $('cancel').disabled = false; render();}
  if (done) await load(true);
};
(async () => {
  try {
    const session = await mountNavigation('admin', {api});
    if (session?.role !== 'administrator') {deny('此页面仅供已登录的站点管理员使用。'); return;}
    currentUser = session.user.id; $('identity').textContent = `${session.user.displayName} · 站点管理员`; $('workspace').hidden = false;
    try {const access = await api.request(sharedApi.getPrivateAgentControlAccess({})); $('control-entry').hidden = String(access?.id) !== String(currentUser);} catch { $('control-entry').hidden = true; }
    await load(true);
  } catch (error) {fail(error);}
})();
