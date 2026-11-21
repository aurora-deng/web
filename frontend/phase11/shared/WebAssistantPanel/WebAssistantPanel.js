import { webAiApi } from '../../ai/WebAiApi/WebAiApi.js?v=7f8c7f1b31e4435c';
import { BusinessApi } from '../BusinessApi/BusinessApi.js?v=7f8c7f1b31e4435c';
import { modelConnection, openModelConnection, requestOwnModel, resetModelConnection, restoreModelMode } from '../ModelConnection/ModelConnection.js?v=7f8c7f1b31e4435c';
import { xiaoYanAvatar } from '../XiaoYan/XiaoYan.js?v=7f8c7f1b31e4435c';
import { WebAssistantHistory } from './WebAssistantHistory.js?v=7f8c7f1b31e4435c';

const make = (tag, content, className) => {
  const element = document.createElement(tag);
  if (content) element.textContent = content;
  if (className) element.className = className;
  return element;
};
const id = () => Array.from(crypto.getRandomValues(new Uint8Array(20)), number => number.toString(16).padStart(2, '0')).join('');

function waitForUpdate(signal) {
  return new Promise((resolve, reject) => {
    if (signal.aborted) return reject(signal.reason);
    const abort = () => { clearTimeout(timer); reject(signal.reason); };
    const timer = setTimeout(() => { signal.removeEventListener('abort', abort); resolve(); }, 450);
    signal.addEventListener('abort', abort, { once: true });
  });
}

export class WebAssistantPanel {
  constructor(host, { close, hasPrivateSpace = false } = {}) {
    this.host = host;
    this.api = new BusinessApi();
    this.models = [];
    host.innerHTML = `
      <header class="assistant-heading">
        <div><strong>小研</strong><small>网页对话</small></div>
        <a class="assistant-mode" href="/phase11/assistant/" hidden>私人空间</a>
        <a data-full-view href="/phase11/assistant/?mode=web">展开对话</a>
        <button type="button" data-close aria-label="关闭小研">×</button>
      </header>
      <p class="assistant-status" role="status">正在读取账号与模型…</p>
      <div class="assistant-threadbar">
        <select data-web-thread aria-label="小研网页会话"></select>
        <button type="button" data-new>新对话</button>
        <button type="button" data-connection>模型连接</button>
      </div>
      <details class="web-assistant-notice" open>
        <summary>对话的发送范围</summary>
        <p data-route-summary></p>
        <label><input data-web-consent type="checkbox">我了解发送范围，本站和外部 API 仅用于非保密对话。</label>
      </details>
      <div class="assistant-messages" aria-live="polite"></div>
      <form class="assistant-composer">
        <textarea required maxlength="8000" rows="3" placeholder="和小研聊聊…" aria-label="给小研的消息"></textarea>
        <footer><span data-provider></span><button type="button" data-stop disabled>停止</button><button type="submit">发送</button></footer>
      </form>`;
    host.querySelector('.assistant-heading').prepend(xiaoYanAvatar());
    host.querySelector('[data-close]').hidden = !close;
    host.querySelector('[data-full-view]').hidden = !close;
    host.querySelector('.assistant-mode').hidden = !hasPrivateSpace;
    host.querySelector('[data-close]').onclick = () => close?.();
    this.status = host.querySelector('[role=status]');
    this.list = host.querySelector('.assistant-messages');
    this.input = host.querySelector('textarea');
    this.threads = host.querySelector('[data-web-thread]');
    this.form = host.querySelector('form');
    this.consent = host.querySelector('[data-web-consent]');
    this.threads.onchange = () => { this.history?.select(this.threads.value); this.paint(); };
    host.querySelector('[data-new]').onclick = () => { if (!this.controller && this.history) { this.history.create(id()); this.paint(); this.input.focus(); } };
    host.querySelector('[data-connection]').onclick = () => this.connections();
    host.querySelector('[data-stop]').onclick = () => this.stop();
    this.form.onsubmit = event => { event.preventDefault(); this.send(); };
    this.input.onkeydown = event => {
      if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) { event.preventDefault(); this.form.requestSubmit(); }
    };
    this.provider();
  }

  start() {
    if (this.history) return Promise.resolve();
    this.loading ??= this.load().finally(() => { this.loading = null; });
    return this.loading;
  }

  async load() {
    try {
      this.owner = String((await this.api.restore()).id);
      if (this.disposed) return;
      this.history = new WebAssistantHistory(this.owner);
      restoreModelMode(this.history.mode,this.history.ollama);
      let modelError = false;
      try { this.models = await this.api.request(webAiApi.getAiWebModels({})); }
      catch { modelError = true; }
      if (!this.models.some(model => String(model.accountId) === this.history.model)) this.history.model = String(this.models[0]?.accountId || '');
      this.history.save();
      this.status.textContent = modelError ? '本站模型目录暂不可用，可以配置其他连接。' : '对话保留在当前浏览器标签页。';
      this.paint();
      this.provider();
    } catch (error) {
      this.status.textContent = error.status === 401 ? '登录 NEBULA 后即可使用小研。' : error.message;
    }
  }

  connections() {
    if (this.controller) { this.status.textContent = '结束当前回复后再切换模型。'; return; }
    if (!this.history) { this.start(); return; }
    if (this.connectionDialog?.open) return;
    this.connectionDialog = openModelConnection({
      serverModel: this.history.model,
      serverModels: this.models,
      testServer: (signal, model) => this.generateServer([{ role: 'user', content: 'Reply briefly with OK.' }], signal, { model, maxTokens: 8 }),
      onChange: value => {
        this.history.setConnection(value);
        this.consent.checked = false;
        this.provider();
      },
    });
  }

  provider() {
    const connection = modelConnection();
    const name = this.models.find(model => String(model.accountId) === this.history?.model)?.name;
    this.host.querySelector('[data-provider]').textContent = connection.provider === 'server' ? name || '本站模型'
      : `${connection.provider === 'ollama' ? '本机 Ollama' : '自带 API'} · ${connection.model || '待配置'}`;
    this.host.querySelector('[data-route-summary]').textContent = {
      server: '输入和本段上下文将交给本站 Linux AI。',
      ollama: '输入和本段上下文由浏览器发给你这台电脑的 Ollama。',
      custom: '输入、上下文和 API Key 将由浏览器发给你填写的服务商。',
    }[connection.provider];
  }

  paint() {
    this.threads.replaceChildren(...(this.history?.threads || []).map(thread => new Option(thread.title, thread.id)));
    if (!this.history?.current) this.threads.append(new Option('新对话', ''));
    this.threads.value = this.history?.selected || '';
    this.list.replaceChildren(...(this.history?.current?.messages || []).map(message => this.bubble(message)));
    if (!this.list.children.length) {
      const empty = make('div', null, 'assistant-empty');
      empty.append(xiaoYanAvatar(), make('strong', '今天想聊点什么？'));
      this.list.append(empty);
    }
    this.list.scrollTop = this.list.scrollHeight;
  }

  bubble(message) {
    const row = make('article', null, `assistant-bubble ${message.role}`);
    row.append(make('small', message.role === 'user' ? '我' : '小研'), make('p', message.content));
    return row;
  }

  async generateServer(messages, signal, { model = this.history?.model, maxTokens = 512, onText = () => {} } = {}) {
    if (!model) throw new Error('先在模型连接中选择一个可用模型。');
    const requestId = id();
    const payload = { requestId, aiUserId: model, nonConfidentialConsent: 'true', messageCount: String(messages.length), maxOutputTokens: String(maxTokens) };
    messages.forEach((message, index) => { payload[`role${index}`] = message.role; payload[`content${index}`] = message.content; });
    let created = false;
    let answer = '';
    let after = 0;
    try {
      await this.api.request(webAiApi.postAiWebGenerations({}), { method: 'POST', data: payload, signal });
      created = true;
      while (!signal.aborted) {
        const result = await this.api.request(webAiApi.getAiWebGenerationsGeneration({ generation: requestId }, `after=${after}`), { signal });
        answer += result.pieces.join('');
        after = result.next;
        onText(answer);
        if (!['queued', 'running'].includes(result.state) && result.pieces.length < 64) {
          if (result.state !== 'completed') throw new Error(result.state === 'cancelled' ? '生成已停止' : '本站模型未完成回答，请测试连接。');
          return answer;
        }
        await waitForUpdate(signal);
      }
      throw signal.reason;
    } finally {
      if (created) this.api.request(webAiApi.deleteAiWebGenerationsGeneration({ generation: requestId }), { method: 'DELETE' }).catch(() => {});
    }
  }

  busy(running) {
    this.threads.disabled = running;
    this.host.querySelectorAll('[data-new],[data-connection],button[type=submit]').forEach(button => { button.disabled = running; });
    this.host.querySelector('[data-stop]').disabled = !running;
  }

  async send() {
    if (this.controller || this.loading) return;
    const content = this.input.value.trim();
    if (!content) return;
    if (!this.consent.checked) { this.status.textContent = '请先确认当前模型的发送范围。'; return; }
    await this.start();
    if (!this.history || this.controller || this.disposed) return;
    const connection = modelConnection();
    if (connection.provider === 'custom' && !connection.endpoint) { this.status.textContent = '请在模型连接中重新填写外部地址与 Key。'; return; }
    const thread = this.history.current || this.history.create(id());
    const message = { role: 'user', content };
    const context = [...thread.messages.slice(-12), message];
    if (context.reduce((length, item) => length + new TextEncoder().encode(item.content).length, 0) > 30000) {
      this.status.textContent = '这段对话的上下文太长，请新建对话。';
      return;
    }
    this.history.append(thread, message);
    this.input.value = '';
    this.paint();
    const bubble = this.bubble({ role: 'assistant', content: '正在思考…' });
    this.list.append(bubble);
    this.list.scrollTop = this.list.scrollHeight;
    this.controller = new AbortController();
    const signal = this.controller.signal;
    const timeout = setTimeout(() => this.stop(), 180000);
    this.busy(true);
    this.status.textContent = '小研正在回答…';
    const output = bubble.querySelector('p');
    try {
      const answer = connection.provider === 'server'
        ? await this.generateServer(context, signal, { onText: text => { if (text) output.textContent = text; } })
        : await requestOwnModel(connection, context, { signal });
      if (this.disposed) return;
      if (!answer.trim()) throw new Error('模型没有返回可显示的文字。');
      this.history.append(thread, { role: 'assistant', content: answer });
      this.paint();
      this.status.textContent = '回答已保存到当前标签页。';
    } catch (error) {
      if (this.disposed) return;
      output.textContent = error.name === 'AbortError' ? '已停止。' : error.name === 'TypeError' ? '连接失败，请检查模型地址和浏览器访问限制。' : error.message;
      this.status.textContent = '本次回答未完成。';
    } finally {
      clearTimeout(timeout);
      this.controller = null;
      if (!this.disposed) this.busy(false);
    }
  }

  stop() { this.controller?.abort(); }
  dispose() { this.disposed = true; this.stop(); this.connectionDialog?.close(); resetModelConnection(); }
}
