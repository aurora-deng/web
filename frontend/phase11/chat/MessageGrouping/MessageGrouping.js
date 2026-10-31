import { formatDayKey } from '../../shared/TimeFormat/TimeFormat.js?v=dc39cdb5489c964d';
/** One data row per message; neither DOM adjacency nor an extra divider row drives grouping. */
export function groupMessages(rows) {
  let previous = null;
  const resultRows=[];
  rows.forEach(message => {
    const day = formatDayKey(message.createdAt), lastDay = formatDayKey(previous?.createdAt);
    const time = Number(message.createdAt), lastTime = Number(previous?.createdAt);
    const system = m => !m || !m.senderId || m.state === 'recalled' || m.type === 'system';
    const grouped = !system(message) && !system(previous) && day && day === lastDay &&
      String(message.senderId) === String(previous.senderId) &&
      Number.isFinite(time) && Number.isFinite(lastTime) && time >= lastTime && time-lastTime < 300000;
    const result = {...message, dateBreak:day && day!==lastDay ? day : undefined, grouped:Boolean(grouped)};
    if(grouped && resultRows.length) resultRows.at(-1).continuesGroup=true;
    resultRows.push(result);previous = message;
  });
  return resultRows;
}
