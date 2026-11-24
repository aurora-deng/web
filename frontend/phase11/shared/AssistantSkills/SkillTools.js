import { accountsApi } from '../../accounts/AccountsApi/AccountsApi.js?v=1b40f8c19ed29802';
import { chatApi } from '../../chat/ChatApi/ChatApi.js?v=1b40f8c19ed29802';
import { treeApi } from '../../tree/TreeApi/TreeApi.js?v=1b40f8c19ed29802';
import { decimalId, safeStudyProgress, tools } from './SkillCatalog.js?v=1b40f8c19ed29802';

const scope = /^[a-f0-9]{32}$/;

function localDay() {
  const date = new Date();
  return [date.getFullYear(), date.getMonth() + 1, date.getDate()].map(value => String(value).padStart(2, '0')).join('-');
}

async function studyRequest(path, signal, fetcher) {
  const response = await fetcher(path, {
    credentials: 'same-origin', cache: 'no-store', redirect: 'error',
    signal: AbortSignal.any([signal, AbortSignal.timeout(6000)]),
  });
  if (!response.ok) throw new Error(response.status === 401 || response.status === 403
    ? '研途登录已变化，请重新打开小研。' : '研途任务统计暂不可用。可以关闭「参考研途」后继续。');
  return response.json();
}

export async function requireSkillOwner(api, owner, signal) {
  const result = await api.request(accountsApi.getMe({}), { signal });
  if (!decimalId(owner) || decimalId(result?.user?.id) !== decimalId(owner)) throw new Error('账号已变化，请重新打开小研。');
}

export async function readSkillTool(tool, { api, owner, signal, fetcher = fetch, day = localDay() }) {
  if (!Object.values(tools).includes(tool)) throw new Error('此工具未登记。');
  owner = decimalId(owner);
  if (!owner) throw new Error('请登录后使用小研技能。');
  if (tool === tools.study) {
    const session = await studyRequest('/api/study/session', signal, fetcher);
    if (session.accountId !== owner || typeof session.scope !== 'string' || !scope.test(session.scope)
        || session.url !== `/api/study/${session.scope}/`) throw new Error('研途工作区与当前账号不匹配。');
    const result = await studyRequest(session.url + 'api/assistant-summary?day=' + encodeURIComponent(day), signal, fetcher);
    if (result.accountId !== owner || result.day !== day) throw new Error('研途工作区与当前账号不匹配。');
    return safeStudyProgress(result);
  }

  await requireSkillOwner(api, owner, signal);
  const chat = tool === tools.chat;
  const rows = await api.request(chat ? chatApi.getConversations({}) : treeApi.getTreeCategories({}), { signal });
  await requireSkillOwner(api, owner, signal);
  if (!Array.isArray(rows)) throw new Error('服务器未返回可用选项。');
  const choices = new Map();
  for (const row of rows) {
    const id = decimalId(row?.id);
    // The current message composer uses safe JS numbers for conversation IDs.
    if (!id || (chat && !Number.isSafeInteger(Number(id))) || choices.has(id)) continue;
    const value = chat ? row.displayTitle || row.title : row.name;
    const label = typeof value === 'string' && value.trim() ? value.trim().slice(0, 100) : `#${id}`;
    choices.set(id, { id, label: chat ? `${row.kind === 'group' ? '群聊' : '会话'} · ${label}` : label });
    if (choices.size === 250) break;
  }
  return [...choices.values()];
}
