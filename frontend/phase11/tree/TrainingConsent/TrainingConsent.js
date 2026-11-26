import { treeApi } from '../TreeApi/TreeApi.js?v=0613976363093f95';

export function trainingConsentControl(api, post, status) {
  if (!post.treeChannel || !post.body?.trim() || String(post.authorId) !== String(api.user?.id)) return null;
  const button = document.createElement('button');
  button.type = 'button';
  button.textContent = 'AI 训练授权';
  button.onclick = async () => {
    button.disabled = true;
    try {
      const { allowed } = await api.request(treeApi.getTreePostsPostTrainingConsent({ post: post.id }));
      const message = allowed ? '已授权。是否撤回此帖的训练授权？撤回后它会退出后续导出。'
        : '允许审核员脱敏后将此帖的公开文字加入训练候选？授权可以撤回。';
      if (!confirm(message)) return;
      const result = await api.request(treeApi.patchTreePostsPostTrainingConsent({ post: post.id }), {
        data: { allowed: String(!allowed) },
      });
      status(result.allowed ? '授权已登记，等待脱敏审核。' : '此帖当前未授权训练。');
    } catch (error) { status(error.message); }
    finally { button.disabled = false; }
  };
  return button;
}
