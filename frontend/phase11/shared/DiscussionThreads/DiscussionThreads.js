export function groupDiscussion(rows) {
  const byId = new Map(rows.map(reply => [String(reply.id), reply]));
  const groups = new Map();
  const rootFor = new Map();
  for (const reply of rows) {
    let root = String(reply.id);
    const walked = new Set();
    while (byId.has(root) && !walked.has(root)) {
      if (rootFor.has(root)) { root = rootFor.get(root); break; }
      walked.add(root);
      const parent = String(byId.get(root).parentReplyId || '0');
      if (parent === '0') break;
      root = parent;
    }
    for (const id of walked) rootFor.set(id, root);
    if (!groups.has(root)) groups.set(root, { root: byId.get(root), replies: [] });
    if (root !== String(reply.id)) groups.get(root).replies.push(reply);
  }
  return [...groups.values()];
}

export function paintDiscussion(host, rows, render) {
  const open = new Set([...host.querySelectorAll('details[open]')].map(details => details.dataset.root));
  host.replaceChildren();
  for (const thread of groupDiscussion(rows)) {
    const group = document.createElement('section');
    group.className = 'discussion-thread';
    if (thread.root) group.append(render(thread.root));
    if (thread.replies.length) {
      const details = document.createElement('details');
      details.className = 'discussion-children';
      details.dataset.root = String(thread.root?.id || thread.replies[0].parentReplyId);
      details.open = open.has(details.dataset.root);
      const summary = document.createElement('summary');
      summary.textContent = thread.root ? `楼中回复 · ${thread.replies.length}`
        : `更早楼层的回复 · ${thread.replies.length}`;
      details.append(summary);
      for (const reply of [...thread.replies].reverse()) details.append(render(reply));
      group.append(details);
    }
    host.append(group);
  }
}
