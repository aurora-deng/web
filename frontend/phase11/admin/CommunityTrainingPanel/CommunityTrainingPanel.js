import { adminApi } from '../AdminApi/AdminApi.js?v=7f8c7f1b31e4435c';

function element(tag, text) {
  const node = document.createElement(tag);
  if (text) node.textContent = text;
  return node;
}

export async function mountCommunityTrainingPanel(host, api) {
  const panel = element('section');
  const status = element('p');
  status.setAttribute('role', 'status');
  const list = element('div');
  const toolbar = element('div');
  const exportButton = element('button', '导出当前页已审核文本');
  const next = element('button', '下一页');
  const reset = element('button', '回到第一页');
  toolbar.append(exportButton, next, reset);
  panel.append(element('h2', '社区训练候选'),
    element('p', '请另填已审核的文字，移除个人信息及第三方受保护内容。撤回授权、隐藏或删除的帖子会退出后续导出。'),
    toolbar, status, list);
  let before = '0';

  async function perform(button, action) {
    button.disabled = true;
    status.textContent = '正在处理…';
    try { await action(); }
    catch (error) { status.textContent = error.message; }
    finally { button.disabled = false; }
  }
  async function page() {
    const rows = await api.request(adminApi.getAdminTreeTraining({}, new URLSearchParams({ before })));
    list.replaceChildren();
    for (const row of rows) {
      const card = element('article');
      const link = element('a', '帖子 #' + row.postId);
      link.href = '/phase11/tree/?post=' + encodeURIComponent(row.postId);
      const original = element('p', row.text);
      const label = element('label', '脱敏后的训练文本');
      const text = element('textarea');
      text.rows = 3;
      text.maxLength = 2000;
      label.append(text);
      const save = element('button', row.reviewed ? '更新审核文本' : '确认审核文本');
      save.onclick = () => perform(save, async () => {
        const review = text.value.trim();
        if (!review) throw new Error('请填写已脱敏且可使用的文字。');
        await api.request(adminApi.postAdminTreeTrainingPostReview({ post: row.postId }), { data: { text: review } });
        save.textContent = '更新审核文本';
        text.value = '';
        status.textContent = '审核已保存。';
      });
      card.append(link, original, label, save);
      list.append(card);
    }
    next.hidden = rows.length !== 50;
    next.onclick = () => perform(next, async () => { before = rows.at(-1).postId; await page(); });
    reset.hidden = before === '0';
    if (!rows.length) list.append(element('p', '这一页没有已授权的公开文字。'));
    status.textContent = '';
  }
  reset.onclick = () => perform(reset, async () => { before = '0'; await page(); });
  exportButton.onclick = () => perform(exportButton, async () => {
    const rows = await api.request(adminApi.getAdminTreeTraining({}, new URLSearchParams({ before, approved: 'true' })));
    if (!rows.length) { status.textContent = '当前范围没有可导出的审核文本。'; return; }
    const content = rows.map(row => JSON.stringify({ postId: row.postId, text: row.text })).join('\n') + '\n';
    const url = URL.createObjectURL(new Blob([content], { type: 'application/x-ndjson' }));
    const link = element('a');
    link.href = url;
    link.download = `nebula-community-${before}.jsonl`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    status.textContent = `已导出 ${rows.length} 条审核文本。`;
  });
  host.replaceChildren(panel);
  await page();
}
