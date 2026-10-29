/**
 * 未读数是两根只向前走的刻度之差，不是几个“红点增量”的较大值。
 * HTTP 快照、WS 乱序和多端已读都合并到同一水位；字符串序号避免 2^53 精度丢失。
 */
export class UnreadWatermarks {
  constructor() { this.rows = new Map(); }
  clear() { this.rows.clear(); }
  value(value) {
    if (typeof value === 'number' && !Number.isSafeInteger(value)) return 0n;
    try { const n = BigInt(value ?? 0); return n > 0n ? n : 0n; } catch { return 0n; }
  }
  observe(id, highest, read = 0) {
    const key = String(id);
    const row = this.rows.get(key) || { highest: 0n, read: 0n };
    const high = this.value(highest), seen = this.value(read);
    if (high > row.highest) row.highest = high;
    if (seen > row.read) row.read = seen;
    // 已读回执可能比消息/列表先到，读过的序号也必然已经存在。
    if (row.read > row.highest) row.highest = row.read;
    this.rows.set(key, row);
    return this.count(id);
  }
  snapshot(conversation) {
    const high = conversation.latestSequence ??
      (this.value(conversation.nextSequence) > 0n ? this.value(conversation.nextSequence) - 1n : 0n);
    return this.observe(conversation.id, high,
      conversation.readSequence ?? conversation.lastReadSequence ?? 0);
  }
  count(id) {
    const row = this.rows.get(String(id));
    const n = row ? row.highest - row.read : 0n;
    return Number(n > BigInt(Number.MAX_SAFE_INTEGER) ? BigInt(Number.MAX_SAFE_INTEGER) : n);
  }
  retain(ids) { const visible = new Set(ids.map(String)); for (const key of this.rows.keys()) if (!visible.has(key)) this.rows.delete(key); }
}
