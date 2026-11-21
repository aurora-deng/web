import { activePreferences, preferences } from './PreferenceProfile.js?v=4364119c78f60e44';

function element(tag, content = '') {
  const node = document.createElement(tag);
  node.textContent = content;
  return node;
}

export function openPreferenceSettings(agent, { local = false } = {}) {
  if (!document.querySelector('[data-model-connection-css]')) {
    const css = element('link');
    css.rel = 'stylesheet';
    css.href = '/phase11/shared/ModelConnection/ModelConnection.css?v=4364119c78f60e44';
    css.dataset.modelConnectionCss = '';
    document.head.append(css);
  }
  const dialog = element('dialog');
  dialog.className = 'model-connection personal-agent-settings';
  const header = element('header');
  const close = element('button', '×');
  close.type = 'button';
  close.setAttribute('aria-label', '关闭个性化设置');
  close.onclick = () => dialog.close();
  header.append(element('h2', '小研 · 个性化记忆'), close);

  const description = element('p', local
    ? '记忆保存在这台电脑的浏览器里。本机检索的原始文件留在 Windows，由资料页控制是否发送命中片段。启用个性化后，本轮允许发送表达标签和命中的记忆要点给自有 Linux 模型。'
    : '按当前账号和浏览器保存。启用后，表达标签和命中的记忆要点可能随问题发给你当前选择的模型。');
  const toggles = [
    ['enabled', '启用个性化记忆'],
    ['useModel', '用当前模型提取偏好、压缩长对话和定期概括'],
    ['saveHistory', '保留最近问题要点，用于跨对话检索'],
    ['trackViews', '用公开故事的分类浏览次数辅助相关话题'],
  ];
  const controls = element('div');
  controls.className = 'personal-agent-options';
  const status = element('p');
  status.setAttribute('role', 'status');
  const summary = element('p');
  const styleList = element('ul');
  const memoryList = element('ul');
  const form = element('form');
  const kind = element('select');
  kind.setAttribute('aria-label', '要点用途');
  kind.append(new Option('资料或习惯 · 按问题检索', 'note'), new Option('长期限制 · 每轮参考', 'constraint'));
  const note = element('input');
  note.maxLength = 240;
  note.required = true;
  note.placeholder = '例如：我的系统开发学习项目正在做 ACK 延迟定位';
  note.setAttribute('aria-label', '资料或习惯要点');
  const save = element('button', '记住这条要点');
  save.type = 'submit';
  form.append(kind, note, save);
  const history = element('details');
  const recent = element('ul');
  history.append(element('summary', '最近问题要点与公开分类'), recent);
  const clear = element('button', '清空个性化记忆');
  clear.type = 'button';
  dialog.append(header, description, controls, element('h3', '当前表达偏好'), styleList, summary,
    element('h3', '主动保存的资料、习惯和限制'), form, memoryList, history, clear, status);

  function row(list, label, action) {
    const li = element('li', label);
    const remove = element('button', '移除');
    remove.type = 'button';
    remove.setAttribute('aria-label', '移除 ' + label);
    remove.onclick = () => mutate(action);
    li.append(' ', remove);
    list.append(li);
  }

  function paint(profile) {
    for (const input of controls.querySelectorAll('input')) {
      input.checked = profile[input.name];
      input.disabled = input.name !== 'enabled' && !profile.enabled;
    }
    styleList.replaceChildren();
    const style = activePreferences(profile, Date.now());
    for (const [key, value] of Object.entries(style)) {
      const fromCompression = profile.observations.some(item => item.key === key && item.value === value && item.source === 'compaction');
      row(styleList, preferences[key][value] + (fromCompression ? ' · 来自会话压缩' : ''),
        () => agent.editMemory({ type: 'forget', key }));
    }
    if (!styleList.children.length) styleList.append(element('li', '尚未学习到稳定的表达偏好'));
    memoryList.replaceChildren();
    for (const item of profile.notes) row(memoryList, (item.source === 'constraint' ? '长期限制 · ' : '') + item.text,
      () => agent.editMemory({ type: 'remove', text: item.text }));
    recent.replaceChildren(...profile.recent.slice(0, 5).map(item => element('li', (item.origin === 'compaction' ? '会话压缩 · ' : '') + item.text)),
      ...profile.topics.slice(0, 5).map(item => element('li', `${item.label} · 浏览权重 ${item.count.toFixed(1)}`)));
    if (!recent.children.length) recent.append(element('li', '暂无要点或浏览记录'));
    summary.textContent = profile.summary ? '阶段摘要 · ' + profile.summary.text : '';
  }

  async function mutate(change) {
    const active = [...dialog.querySelectorAll('input,button,select')].filter(control => control !== close && !control.disabled);
    active.forEach(control => { control.disabled = true; });
    try { paint(await change()); status.textContent = '已保存到当前账号的本机空间。'; }
    catch (error) {
      active.forEach(control => { control.disabled = false; });
      agent.profile().then(paint, () => {});
      status.textContent = error.message;
      return false;
    }
    active.filter(control => !(controls.contains(control) && control.localName === 'input')).forEach(control => { control.disabled = false; });
    return true;
  }

  for (const [key, label] of toggles) {
    const line = element('label', label);
    const input = element('input');
    input.type = 'checkbox';
    input.name = key;
    input.disabled = true;
    input.onchange = () => mutate(() => agent.configure(key, input.checked));
    line.prepend(input);
    controls.append(line);
  }
  form.onsubmit = async event => {
    event.preventDefault();
    if (await mutate(() => agent.editMemory({ type: 'add', text: note.value, kind: kind.value }))) note.value = '';
  };
  clear.onclick = () => {
    if (confirm('清空当前账号在这个小研入口学到的偏好和要点？')) mutate(() => agent.editMemory({ type: 'clear' }));
  };
  agent.profile().then(paint, () => { status.textContent = '无法打开本机记忆存储，仍可正常对话。'; });
  dialog.addEventListener('close', () => dialog.remove(), { once: true });
  document.body.append(dialog);
  dialog.showModal();
  return dialog;
}
