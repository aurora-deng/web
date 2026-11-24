import { decimalId, draftLimits } from './SkillCatalog.js?v=1b40f8c19ed29802';

const key = 'nebula:xiaoyan:handoff:v1';
const parameter = 'xiaoyanDraft';
const lifetime = 10 * 60 * 1000;
const noncePattern = /^[a-f0-9]{32}$/;
const chatBindings = new WeakMap();
const nonce = () => Array.from(crypto.getRandomValues(new Uint8Array(16)), byte => byte.toString(16).padStart(2, '0')).join('');

function checkedBody(destination, value) {
  const body = typeof value === 'string' ? value.trim() : '';
  if (!Object.hasOwn(draftLimits, destination) || !body || body.length > draftLimits[destination]) {
    throw new Error('草稿正文为空或超出目标页面的长度限制。');
  }
  return body;
}

function validTarget(destination, target) {
  return decimalId(target) && (destination !== 'chat' || Number.isSafeInteger(Number(target)));
}

export function stageSkillDraft({ owner, destination, target, body }, {
  storage = sessionStorage, now = Date.now(), id = nonce(),
} = {}) {
  if (!decimalId(owner) || !validTarget(destination, target) || !noncePattern.test(id)) throw new Error('请先选择有效的目标。');
  const text = checkedBody(destination, body);
  storage.setItem(key, JSON.stringify({
    id, owner: decimalId(owner), destination, target: decimalId(target), body: text, createdAt: now,
  }));
  return destination === 'chat' ? `/phase11/?${parameter}=${id}` : `/phase11/tree/?${parameter}=${id}#feed`;
}

export function pendingSkillDraft(destination, owner, url = new URL(location.href), {
  storage = sessionStorage, now = Date.now(),
} = {}) {
  const id = url.searchParams.get(parameter);
  if (!id || !noncePattern.test(id) || !decimalId(owner)) return null;
  try {
    const value = JSON.parse(storage.getItem(key));
    if (value?.id !== id) return null;
    if (value.owner !== decimalId(owner) || value.destination !== destination || !Number.isSafeInteger(value.createdAt)
        || value.createdAt > now || now - value.createdAt > lifetime || !validTarget(destination, value.target)) {
      storage.removeItem(key);
      return null;
    }
    return { ...value, body: checkedBody(destination, value.body) };
  } catch {
    return null;
  }
}

export function finishSkillDraft(draft, { storage = sessionStorage, locationUrl = location.href, historyApi = history } = {}) {
  const url = new URL(locationUrl);
  if (url.searchParams.get(parameter) !== draft.id) return;
  try {
    if (JSON.parse(storage.getItem(key))?.id === draft.id) storage.removeItem(key);
  } finally {
    url.searchParams.delete(parameter);
    historyApi.replaceState(historyApi.state, '', url);
  }
}

export function prefillChatDraft(input, draft) {
  if (input.value.trim()) return false;
  input.value = draft.body;
  chatBindings.set(input, { owner: draft.owner, target: draft.target });
  return true;
}

export function matchesChatDraft(input, owner, conversation) {
  if (!input.value.trim()) {
    chatBindings.delete(input);
    return true;
  }
  const binding = chatBindings.get(input);
  return !binding || (binding.owner === decimalId(owner) && binding.target === decimalId(conversation));
}

export function clearChatDraft(input) {
  if (chatBindings.delete(input)) input.value = '';
}

export function prefillPlazaDraft(form, draft, categories) {
  const { body, categoryId, destination, publicConfirmed } = form.elements;
  if (body.value.trim() || !categories.some(category => decimalId(category.id) === draft.target)) return false;
  body.value = draft.body;
  categoryId.value = draft.target;
  destination.value = 'tree';
  destination.dispatchEvent(new Event('change'));
  publicConfirmed.checked = false;
  return true;
}
