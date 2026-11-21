const MAX_THREADS = 16;
const MAX_MESSAGES = 30;

function history(messages) {
  return Array.isArray(messages) ? messages.filter(message => message &&
    ['user', 'assistant'].includes(message.role) && typeof message.content === 'string' && message.content.length <= 8000)
    .map(({ role, content }) => ({ role, content })).slice(-MAX_MESSAGES) : [];
}

function decode(storage, key) {
  try { return JSON.parse(storage.getItem(key)); } catch { return null; }
}

/** Both web surfaces use one session-local history for the signed-in account. */
export class WebAssistantHistory {
  constructor(owner, storage = sessionStorage) {
    this.storage = storage;
    this.key = `nebula:xiaoyan:web:${owner}`;
    const saved = decode(storage, this.key);
    this.threads = Array.isArray(saved?.threads) ? saved.threads.filter(thread =>
      thread && typeof thread.id === 'string' && typeof thread.title === 'string')
      .map(thread => ({ id: thread.id, title: thread.title.slice(0, 64), messages: history(thread.messages) }))
      .slice(0, MAX_THREADS) : [];
    if (!saved) {
      for (const [surface, title] of [['friend', '原好友对话'], ['sidebar', '原侧栏对话']]) {
        const messages = history(decode(storage, `nebula:web-chat:${owner}:${surface}`));
        if (messages.length) this.threads.push({ id: `legacy-${surface}`, title, messages });
      }
    }
    this.model = typeof saved?.model === 'string' ? saved.model : '';
    this.mode = ['server', 'ollama', 'custom'].includes(saved?.mode) ? saved.mode : 'server';
    this.ollama = typeof saved?.ollama?.endpoint === 'string' && typeof saved?.ollama?.model === 'string'
      ? { endpoint: saved.ollama.endpoint, model: saved.ollama.model } : null;
    this.selected = this.threads.some(thread => thread.id === saved?.selected) ? saved.selected : this.threads[0]?.id || '';
    this.save();
  }

  get current() { return this.threads.find(thread => thread.id === this.selected); }

  select(id) {
    if (this.threads.some(thread => thread.id === id)) this.selected = id;
    this.save();
  }

  create(id) {
    // Replacing an empty conversation does not add another empty entry to history.
    this.threads = this.threads.filter(thread => thread.messages.length);
    this.threads.unshift({ id, title: '新对话', messages: [] });
    this.threads = this.threads.slice(0, MAX_THREADS);
    this.selected = id;
    this.save();
    return this.current;
  }

  append(thread, message) {
    if (!thread.messages.length && thread.title === '新对话') thread.title = message.content.replace(/\s+/g, ' ').slice(0, 36);
    thread.messages = [...thread.messages, { role: message.role, content: message.content }].slice(-MAX_MESSAGES);
    this.save();
  }

  setConnection(value) {
    this.mode = value.provider;
    if (value.provider === 'ollama') this.ollama = { endpoint: value.endpoint, model: value.model };
    if (value.provider === 'server') this.model = value.serverModel;
    this.save();
  }

  save() {
    try { this.storage.setItem(this.key, JSON.stringify({ selected: this.selected, model: this.model, mode: this.mode, ollama: this.ollama, threads: this.threads })); }
    catch { /* Current messages remain accessible when browser session storage is full or disabled. */ }
  }
}
