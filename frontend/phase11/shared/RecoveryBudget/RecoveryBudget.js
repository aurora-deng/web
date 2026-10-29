/** 分页像进站闸机：只限制恢复请求，不把普通发消息也串到同一条队列。 */
export class RecoveryBudget {
  constructor({ interval = 200, capacity = 4, now = () => Date.now(),
                wait = ms => new Promise(resolve => setTimeout(resolve, ms)) } = {}) {
    Object.assign(this, { interval, capacity, now, wait });
    this.pending = 0;
    this.tail = Promise.resolve();
    this.next = 0;
  }
  run(operation, signal) {
    if (this.pending >= this.capacity) return Promise.reject(new Error('历史恢复繁忙，请稍后重试'));
    ++this.pending;
    const task = this.tail.catch(() => {}).then(async () => {
      if (signal?.aborted) throw new DOMException('会话已切换', 'AbortError');
      await this.wait(Math.max(0, this.next - this.now()));
      if (signal?.aborted) throw new DOMException('会话已切换', 'AbortError');
      this.next = this.now() + this.interval;
      return operation();
    });
    this.tail = task;
    return task.finally(() => { --this.pending; });
  }
}

/** Equal jitter：在指数退避的一半至全长随机分布，防止同一时间全部重连。 */
export class ReconnectBackoff {
  constructor(random = Math.random) { this.random = random; this.attempt = 0; }
  reset() { this.attempt = 0; }
  next() {
    const ceiling = Math.min(30000, 1600 * 2 ** Math.min(this.attempt++, 5));
    return Math.round(ceiling * (0.5 + this.random() * 0.5));
  }
}
