import { treeApi } from '../TreeApi/TreeApi.js?v=7f8c7f1b31e4435c';
import { formatShortDateTime } from '../../shared/TimeFormat/TimeFormat.js?v=7f8c7f1b31e4435c';
import { storyText } from '../StoryPreview/StoryPreview.js?v=7f8c7f1b31e4435c';

export class CategoryRanking {
  constructor(api, { select, list, updated, onSnapshot, onOpen }) {
    Object.assign(this, { api, select, list, updated, onSnapshot, onOpen });
    this.category = '';
    this.generation = 0;
  }

  setCategory(category) {
    if (this.category !== category) {
      this.category = category;
      ++this.generation;
      this.pending = null;
      this.onSnapshot([], []);
      this.message('正在读取所选领域榜单…');
    }
    this.select.value = category;
    return this.refresh();
  }

  refresh() {
    if (this.pending) return this.pending;
    const category = this.category;
    const request = this.read(category, this.generation).catch(error => {
      if (this.pending === request) {
        this.message('榜单暂不可用，刷新后可重试。');
        this.onSnapshot([], []);
      }
      throw error;
    }).finally(() => {
      if (this.pending === request) this.pending = null;
    });
    this.pending = request;
    return request;
  }

  async read(category, generation) {
    const query = new URLSearchParams(category ? { category } : {});
    const data = await this.api.request(treeApi.getTreeRanking({}, query));
    const ids = data.entries.map(rank => rank.postId);
    const posts = ids.length ? (await this.api.request(treeApi.getTreePosts({}, new URLSearchParams({
      limit: '10', ids: ids.join(','), ...(category ? { category } : {}),
    })))).posts : [];
    if (generation !== this.generation) return;
    const postsById = new Map(posts.map(post => [post.id, post]));
    const ranks = data.entries.filter(rank => postsById.has(rank.postId));
    this.updated.textContent = data.settledAt ? `更新于 ${formatShortDateTime(data.settledAt)}` : '等待结算';
    const signature = JSON.stringify([ranks, posts]);
    if (this.signature === signature) return;
    this.signature = signature;
    this.list.replaceChildren();
    if (!ranks.length) this.message('近七日没有可上榜的故事。');
    for (const [index, rank] of ranks.entries()) {
      const post = postsById.get(rank.postId);
      const story = storyText(post);
      const row = document.createElement('li');
      row.className = index < 3 ? 'rank-featured' : 'rank-row';
      const open = document.createElement('button');
      open.type = 'button';
      open.dataset.postId = rank.postId;
      const position = document.createElement('strong');
      position.textContent = String(index + 1).padStart(2, '0');
      position.className = 'rank-number';
      const summary = document.createElement('span');
      summary.className = 'rank-story';
      const caption = document.createElement('strong');
      caption.textContent = story.title;
      const author = document.createElement('small');
      author.textContent = `${post.authorName} · ${formatShortDateTime(post.createdAt)}`;
      const engagement = document.createElement('span');
      engagement.className = 'rank-engagement';
      engagement.textContent = `${post.likes ?? 0} 点赞 · ${post.repliers ?? 0} 人讨论`;
      summary.append(caption, author, engagement);
      if (story.example) {
        const badge = document.createElement('small');
        badge.className = 'story-example';
        badge.textContent = '站内示例';
        summary.append(badge);
      }
      const score = document.createElement('span');
      score.className = 'rank-score';
      score.textContent = `热度 ${Number(rank.score).toLocaleString('zh-CN', {maximumFractionDigits:3})}`;
      open.append(position, summary, score);
      open.onclick = () => this.onOpen(rank.postId);
      row.append(open);
      this.list.append(row);
    }
    this.onSnapshot(ranks, posts);
  }

  message(text) {
    this.signature = '';
    const row = document.createElement('li');
    row.className = 'rank-message';
    row.textContent = text;
    this.list.replaceChildren(row);
  }
}
