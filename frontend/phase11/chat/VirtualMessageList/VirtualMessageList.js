/** 可变高度虚拟列表。数据库保存全集，DOM 只是正在翻阅的那几页。 */
export class VirtualMessageList {
  constructor(element, createRow, { threshold = 400, maximumRows = 160, estimate = 118 } = {}) {
    Object.assign(this, { element, createRow, threshold, maximumRows, estimate });
    this.messages = [];
    this.nodes = new Map();
    this.heights = new Map();
    this.before = [];
    this.after = [];
    this.version = '';
    this.frame = null;
    this.onScroll = () => {
      this.anchor = this.captureAnchor();
      if (this.frame === null) this.frame = requestAnimationFrame(() => { this.frame = null; this.draw(); });
    };
    element.addEventListener('scroll', this.onScroll, { passive: true });
    this.observer = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(() => this.measure());
  }
  static key(message) { return String(message.id || `pending:${message.clientMessageId}`); }
  setMessages(messages, version = '', { before = [], after = [] } = {}) {
    this.messages = messages;
    this.version = version;
    this.before = before;
    this.after = after;
    const retained = new Set(messages.map(VirtualMessageList.key));
    for (const key of this.heights.keys()) if (!retained.has(key)) this.heights.delete(key);
    this.draw();
  }
  prefix() {
    const result = [0];
    for (const message of this.messages)
      result.push(result.at(-1) + (this.heights.get(VirtualMessageList.key(message)) || this.estimate));
    return result;
  }
  range(prefix) {
    if (this.messages.length <= this.threshold) return [0, this.messages.length];
    const top = Math.max(0, this.element.scrollTop - 500);
    let low = 0, high = this.messages.length;
    while (low < high) {
      const middle = (low + high) >> 1;
      if (prefix[middle + 1] < top) low = middle + 1; else high = middle;
    }
    const start = Math.min(low, Math.max(0, this.messages.length - 1));
    const bottom = this.element.scrollTop + this.element.clientHeight + 500;
    let end = start;
    while (end < this.messages.length && end - start < this.maximumRows && prefix[end] < bottom) ++end;
    return [start, Math.min(this.messages.length, Math.max(start + 1, end))];
  }
  spacer(height) {
    const node = document.createElement('li');
    node.className = 'virtual-spacer';
    node.style.height = `${Math.max(0, height)}px`;
    node.setAttribute('aria-hidden', 'true');
    return node;
  }
  draw() {
    const anchor=this.captureAnchor();
    const prefix = this.prefix();
    const [start, end] = this.range(prefix);
    const cache = new Map();
    const rows = this.messages.slice(start, end).map((message, offset) => {
      const key = VirtualMessageList.key(message);
      const signature = this.version + (offset===0 ? ":head:" : ":body:") + JSON.stringify(message);
      const old = this.nodes.get(key);
      const entry = old?.signature === signature ? old : { signature, node: this.createRow(message, {windowStart:offset===0}) };
      cache.set(key, entry);
      return entry.node;
    });
    // AI 增量气泡由独立流组件维护，不能在滚动时销毁或重复朗读。
    const streams = [...this.element.children].filter(node => node.dataset.generationId && !node.dataset.messageId);
    this.after = [...new Set([...this.after, ...streams])].filter(node => !node.dataset.generationId ||
      !this.messages.some(message => message.clientMessageId === `ai:${node.dataset.generationId}`));
    this.observer?.disconnect();
    this.element.classList.toggle('virtualized', this.messages.length > this.threshold);
    this.start=start;this.end=end;this.topSpacer=this.spacer(prefix[start]||0);this.bottomSpacer=this.spacer((prefix.at(-1)||0)-(prefix[end]||0));
    this.element.replaceChildren(...this.before, this.topSpacer, ...rows, this.bottomSpacer, ...this.after);
    if (!rows.length && !this.after.length) {
      const empty = document.createElement('li');
      empty.className = 'list-empty';
      empty.textContent = '这里还没有消息，发送第一句话吧。';
      this.element.append(empty);
    }
    this.nodes = cache;
    rows.forEach(node => this.observer?.observe(node));
    this.measure(anchor);
  }
  captureAnchor(){
    const top=this.element.getBoundingClientRect().top;
    for(const [key,entry] of this.nodes){const rect=entry.node.getBoundingClientRect();if(rect.bottom>=top&&rect.top<top+this.element.clientHeight)return{key,offset:rect.top-top};}
    return null;
  }
  measure(anchor=this.anchor) {
    let changed=false;
    const gap=parseFloat(getComputedStyle(this.element).rowGap)||0;
    for (const [key, entry] of this.nodes) {
      const style=getComputedStyle(entry.node),margin=(parseFloat(style.marginTop)||0)+(parseFloat(style.marginBottom)||0);
      const measured = entry.node.getBoundingClientRect().height + margin + gap;
      if(measured>0&&Math.abs((this.heights.get(key)||this.estimate)-measured)>.5){this.heights.set(key,measured);changed=true;}
    }
    if(changed){const prefix=this.prefix();if(this.topSpacer)this.topSpacer.style.height=`${prefix[this.start]||0}px`;if(this.bottomSpacer)this.bottomSpacer.style.height=`${Math.max(0,prefix.at(-1)-(prefix[this.end]||0))}px`;}
    if(anchor){const node=this.nodes.get(anchor.key)?.node;if(node){const delta=node.getBoundingClientRect().top-this.element.getBoundingClientRect().top-anchor.offset;if(Math.abs(delta)>.5)this.element.scrollTop+=delta;}}
    this.anchor=this.captureAnchor();
    if(changed&&this.messages.length>this.threshold){const [start,end]=this.range(this.prefix());if((start!==this.start||end!==this.end)&&this.frame===null)this.frame=requestAnimationFrame(()=>{this.frame=null;this.draw();});}
  }
  /**
   * 滚动到指定消息并返回它的 DOM 行。
   * 只做「按 prefix 估算位置 → 重绘 → 从新窗口取节点」三件事，
   * 不改变 messages / heights / prefix 的任何不变量，也不修改 range() 的语义。
   * 返回 null 表示这条消息不在当前 messages 里（调用方自己决定兜底）。
   */
  scrollToKey(key) {
    const index = this.messages.findIndex(message => VirtualMessageList.key(message) === String(key));
    if (index < 0) return null;
    const prefix = this.prefix();
    const total = prefix.at(-1) || 0;
    const rowHeight = this.heights.get(String(key)) || this.estimate;
    // 尽量让目标行落在视口中线附近，同时留在可滚动范围内。
    const desired = (prefix[index] || 0) - Math.round(this.element.clientHeight / 2 - rowHeight / 2);
    const ceiling = Math.max(0, total - this.element.clientHeight);
    this.element.scrollTop = Math.min(Math.max(0, desired), ceiling);
    this.draw();
    return this.nodes.get(String(key))?.node || null;
  }
  clear() { this.element.replaceChildren(); this.setMessages([]); }
  dispose() {
    this.element.removeEventListener('scroll', this.onScroll);
    if (this.frame !== null) cancelAnimationFrame(this.frame);
    this.observer?.disconnect();
    this.nodes.clear();
    this.heights.clear();
  }
}
