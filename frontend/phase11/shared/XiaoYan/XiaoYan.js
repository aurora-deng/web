export const xiaoYanImage = '/phase11/shared/XiaoYan/XiaoYan.png';

export function xiaoYanAvatar(className = 'assistant-orb') {
  const frame = document.createElement('span');
  frame.className = className;
  const portrait = document.createElement('img');
  portrait.src = xiaoYanImage;
  portrait.alt = '';
  portrait.decoding = 'async';
  frame.append(portrait);
  return frame;
}
