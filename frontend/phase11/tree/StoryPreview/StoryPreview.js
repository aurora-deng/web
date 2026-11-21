export function storyText(post) {
  const body = post.body?.trim() || '';
  const example = post.bundledExample === true;
  const lines = (example ? body.replace(/^【站内示例】/, '') : body).trim().split(/\r?\n/);
  const first = lines.shift() || '';
  const title = [...first];
  const media = post.media || [];
  const image = media.find(item => item.kind === 'image');
  const voice = media.some(item => item.kind === 'audio');
  return {
    example,
    image,
    voice,
    title: title.slice(0, 48).join('') || (voice ? '语音故事' : image ? '图片故事' : '故事'),
    excerpt: [title.slice(48).join(''), ...lines].join('\n').trim(),
  };
}
