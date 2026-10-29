/**
 * 全站唯一的时间格式化实现。纯函数：不读 DOM、不读全局、不依赖时区设置以外的任何状态。
 * 只接收 (value, now)，方便直接单测。
 *
 * 规则（会话列表 / 消息流 / 帖子流共用）：
 *   今天        → 14:32
 *   昨天        → 昨天 14:32
 *   2~6 天前    → 周三 14:32
 *   今年更早    → 10-28
 *   跨年        → 2025-10-28
 */
const WEEKDAYS = ['日', '一', '二', '三', '四', '五', '六'];
const DAY = 86400000;

function pad2(value) { return String(value).padStart(2, '0'); }

function asDate(value) {
  const stamp = Number(value);
  if (!Number.isFinite(stamp) || stamp <= 0) return null;
  const date = new Date(stamp);
  return Number.isNaN(date.getTime()) ? null : date;
}

function startOfDay(date) {
  return new Date(date.getFullYear(), date.getMonth(), date.getDate()).getTime();
}

function dayDistance(date, reference) {
  return Math.round((startOfDay(reference) - startOfDay(date)) / DAY);
}

/** 本地时区的日历日标识 YYYY-MM-DD；无效输入返回空串。 */
export function formatDayKey(value) {
  const date = asDate(value);
  return date
    ? `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`
    : '';
}

/** 会话列表角标、消息行元信息用：相对当下的时刻标签。 */
export function formatChatTime(value, now = Date.now()) {
  const date = asDate(value);
  if (!date) return '';
  const reference = asDate(now) || new Date();
  const distance = dayDistance(date, reference);
  const clock = `${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
  // distance<0 只可能出现在两台机器的时钟不一致时，按「今天」处理，不要显示「明天」。
  if (distance <= 0) return clock;
  if (distance === 1) return `昨天 ${clock}`;
  if (distance < 7) return `周${WEEKDAYS[date.getDay()]} ${clock}`;
  if (date.getFullYear() === reference.getFullYear()) {
    return `${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
  }
  return `${date.getFullYear()}-${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
}

/** 消息流里跨天时的日期分隔条文案。 */
export function formatDateDivider(value, now = Date.now()) {
  const date = asDate(value);
  if (!date) return '';
  const reference = asDate(now) || new Date();
  const distance = dayDistance(date, reference);
  if (distance <= 0) return '今天';
  if (distance === 1) return '昨天';
  const monthDay = `${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
  if (date.getFullYear() === reference.getFullYear()) {
    return `${monthDay} 周${WEEKDAYS[date.getDay()]}`;
  }
  return `${date.getFullYear()}-${monthDay}`;
}

/** 帖子/动态流用：MM-DD HH:mm，跨年补年份。 */
export function formatShortDateTime(value, now = Date.now()) {
  const date = asDate(value);
  if (!date) return '';
  const reference = asDate(now) || new Date();
  const clock = `${pad2(date.getHours())}:${pad2(date.getMinutes())}`;
  const monthDay = `${pad2(date.getMonth() + 1)}-${pad2(date.getDate())}`;
  return date.getFullYear() === reference.getFullYear()
    ? `${monthDay} ${clock}`
    : `${date.getFullYear()}-${monthDay} ${clock}`;
}
