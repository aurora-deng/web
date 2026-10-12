/**
 * 有界的文字发送队列。超时只表示结果未知；先查同一幂等键，再重试原命令。
 * 不自动重试 AI 生成，因为普通消息的幂等语义不能直接推给模型任务。
 */
export class ReliableSender {
  constructor({ send, verify, changed, persisted, timeout = 5000, maxAttempts = 3,
                // 包装原生定时器，避免把发送队列当成浏览器 Window 接收者。
                schedule = (callback, delay) => setTimeout(callback, delay),
                cancel = timer => clearTimeout(timer) }) {
    Object.assign(this, { send, verify, changed, persisted, timeout, maxAttempts, schedule, cancel });
    this.entries = new Map();
  }

  submit(payload, createdAt = Date.now()) {
    if (this.entries.size >= 32) throw new Error('待确认消息已达 32 条，请先确认或重试现有消息');
    if (this.entries.has(payload.id)) throw new Error('客户端消息编号已存在');
    const entry = { payload: Object.freeze({ ...payload }), createdAt, attempts: 0,
                    status: 'waiting', timer: null, checking: false, error: '' };
    this.entries.set(payload.id, entry);
    this.attempt(entry);
    return entry;
  }

  restore(records) {
    for (const record of Array.isArray(records) ? records.slice(0, 32) : []) {
      const p = record?.payload;
      if (p?.type !== 'chat.send' || typeof p.id !== 'string' || !p.id || p.id.length > 128 ||
          !Number.isSafeInteger(p.conversationId) || p.conversationId <= 0 ||
          typeof p.content !== 'string' || !p.content ||
          new TextEncoder().encode(p.content).length > 16384 || this.entries.has(p.id)) continue;
      this.entries.set(p.id, { payload: Object.freeze({ ...p }), createdAt: record.createdAt,
        attempts: 0, status: 'unknown', timer: null, checking: false, error: '' });
    }
  }

  snapshot() {
    return [...this.entries.values()].map(({ payload, createdAt }) => ({ payload, createdAt }));
  }

  active(entry) { return this.entries.get(entry.payload.id) === entry; }
  notify(entry) { if (this.active(entry)) this.changed(entry); }
  stopTimer(entry) { if (entry.timer !== null) this.cancel(entry.timer); entry.timer = null; }

  attempt(entry) {
    if (!this.active(entry)) return;
    this.stopTimer(entry);
    let sent = false;
    try { sent = this.send(entry.payload); } catch { /* 连接关闭与 send 可以发生竞争。 */ }
    entry.status = sent ? 'sending' : 'waiting';
    if (sent) {
      entry.attempts += 1;
      entry.timer = this.schedule(() => this.check(entry), this.timeout);
    }
    this.notify(entry);
  }

  async check(entry) {
    if (!this.active(entry) || entry.checking) return;
    this.stopTimer(entry);
    entry.checking = true;
    entry.status = 'unknown';
    this.notify(entry);
    try {
      const message = await this.verify(entry.payload);
      if (!this.active(entry)) return;
      if (message) { this.confirmMessage(message); return; }
      if (entry.error) { entry.status = 'failed'; return; }
      if (entry.attempts < this.maxAttempts) {
        // 每次都是原 id / conversationId / content，延迟有上限且重试次数有界。
        entry.timer = this.schedule(() => this.attempt(entry), 800 * 2 ** Math.max(0, entry.attempts - 1));
      }
    } catch {
      // 查不到服务器不能证明没写入；保留未知结果，等待重连或用户手动确认。
      entry.status = 'unknown';
    } finally {
      entry.checking = false;
      this.notify(entry);
    }
  }

  confirm(reply) {
    const entry = this.entries.get(reply.replyTo);
    if (!entry || Number(reply.conversationId) !== entry.payload.conversationId) return;
    this.finish(entry, { id: reply.id, sequence: reply.sequence,
      conversationId: reply.conversationId, clientMessageId: reply.replyTo,
      body: entry.payload.content, createdAt: 0, state: 'active' });
  }

  confirmMessage(message) {
    const entry = this.entries.get(message.clientMessageId);
    if (entry && Number(message.conversationId) === entry.payload.conversationId) this.finish(entry, message);
  }

  finish(entry, message) {
    this.stopTimer(entry);
    this.entries.delete(entry.payload.id);
    entry.status = 'saved';
    this.persisted(entry, message);
  }

  reject(replyTo, error) {
    const entry = this.entries.get(replyTo);
    if (!entry) return;
    entry.error = error || '服务器拒绝了消息';
    return this.check(entry);
  }

  retry(id) {
    const entry = this.entries.get(id);
    if (!entry || entry.checking) return;
    entry.attempts = 0;
    entry.error = '';
    return this.check(entry);
  }

  disconnected() {
    this.entries.forEach(entry => {
      this.stopTimer(entry);
      if (entry.status !== 'failed') entry.status = 'unknown';
      this.notify(entry);
    });
  }

  connected() {
    return Promise.allSettled([...this.entries.values()]
      .filter(entry => entry.status !== 'failed').map(entry => this.check(entry)));
  }

  clear() { this.entries.forEach(entry => this.stopTimer(entry)); this.entries.clear(); }
}
