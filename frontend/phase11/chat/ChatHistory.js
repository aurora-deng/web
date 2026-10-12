/** 单个可见会话的历史窗口。实时最高序号不是补发游标：收到 #105 不能跳过 #101–104。 */
export class ChatHistory {
  constructor(fetchPage, changed = () => {}, pageSize = 100) {
    this.fetchPage = fetchPage;
    this.changed = changed;
    this.pageSize = pageSize;
    this.reset();
  }

  reset(conversationId = 0) {
    this.epoch = (this.epoch || 0) + 1;
    this.conversationId = Number(conversationId);
    this.records = new Map();
    this.cursor = 0;
    this.loaded = false;
    this.loadingOlder = false;
    this.hasOlder = false;
    this.target = 0;
    this.refreshRequested = false;
    this.syncPromise = null;
    this.ready = null;
  }

  get messages() { return [...this.records.values()].sort((a, b) => a.sequence - b.sequence); }
  get oldest() { return this.messages[0]?.sequence || 0; }
  get latest() { return this.messages.at(-1)?.sequence || 0; }

  merge(message) {
    if (!message?.id || Number(message.conversationId) !== this.conversationId) return;
    const key = String(message.id);
    const previous = this.records.get(key);
    // 撤回是终态。较早发出的 HTTP 响应即使晚到，也不能让已撤回的正文重新出现。
    if (previous?.state === 'recalled' && message.state !== 'recalled') return;
    this.records.set(key, { ...previous, ...message });
  }

  ingest(message) {
    // 旧页尚未载入时，不因某条旧撤回事件凭空扩展整个历史窗口。
    if (message.state === 'recalled' && this.loaded && message.sequence < this.oldest &&
        !this.records.has(String(message.id))) return;
    this.merge(message);
    if (this.loaded && Number(message.sequence) === this.cursor + 1) this.cursor += 1;
    this.changed();
  }

  open(conversationId) {
    this.reset(conversationId);
    const epoch = this.epoch;
    this.ready = (async () => {
      const page = await this.fetchPage(this.conversationId, { before: 0, limit: this.pageSize });
      if (epoch !== this.epoch) return;
      page.forEach(message => this.merge(message));
      this.cursor = Number(page.at(-1)?.sequence || 0);
      this.hasOlder = Number(page[0]?.sequence || 0) > 1;
      this.loaded = true;
      this.changed();
    })();
    return this.ready;
  }

  async older() {
    if (!this.loaded || !this.hasOlder || this.loadingOlder) return;
    const epoch = this.epoch;
    const conversation = this.conversationId;
    const before = this.oldest;
    this.loadingOlder = true;
    this.changed();
    try {
      const page = await this.fetchPage(conversation, { before, limit: this.pageSize });
      if (epoch !== this.epoch) return;
      page.forEach(message => this.merge(message));
      this.hasOlder = page.length > 0 && Number(page[0].sequence) > 1;
    } finally {
      if (epoch === this.epoch) { this.loadingOlder = false; this.changed(); }
    }
  }

  synchronize(target, { refreshLoaded = false } = {}) {
    this.target = Math.max(this.target, Number(target) || 0);
    this.refreshRequested ||= refreshLoaded;
    if (this.syncPromise) return this.syncPromise;
    const epoch = this.epoch;
    const conversation = this.conversationId;
    const promise = (async () => {
      await this.ready;
      // 固定每轮的目标水位，避免持续收消息时无限追赶；并发请求会合并到下一轮。
      while (epoch === this.epoch && this.loaded &&
             (this.refreshRequested || this.cursor < this.target)) {
        let after = this.refreshRequested ? Math.max(0, this.oldest - 1) : this.cursor;
        this.refreshRequested = false;
        const ceiling = this.target;
        while (after < ceiling && epoch === this.epoch) {
          const page = await this.fetchPage(conversation, { after, limit: this.pageSize });
          if (epoch !== this.epoch) return;
          const last = Number(page.at(-1)?.sequence || 0);
          if (!page.length || last <= after) throw new Error('历史补发未完成，请重试同步');
          page.forEach(message => this.merge(message));
          after = last;
          this.cursor = Math.max(this.cursor, last);
          this.changed();
        }
      }
    })().finally(() => { if (this.syncPromise === promise) this.syncPromise = null; });
    this.syncPromise = promise;
    return promise;
  }
}

/** 只控制按钮展示，最终授权仍由 ChatService 执行。与后端两分钟时间窗/角色规则对应。 */
export function canRecallMessage(message, userId, conversation, members, now = Date.now()) {
  const actor = members.find(member => Number(member.userId) === Number(userId));
  const created = Number(message.createdAt);
  if (!actor || !message.id || message.state === 'recalled' || !created ||
      now < created || now - created > 120000 ||
      (conversation?.state && conversation.state !== 'active')) return false;
  if (Number(message.senderId) === Number(userId)) return true;
  if (conversation?.kind !== 'group') return false;
  if (actor.role === 'owner') return true;
  const sender = members.find(member => Number(member.userId) === Number(message.senderId));
  return actor.role === 'administrator' && (!sender || sender.role === 'member');
}
