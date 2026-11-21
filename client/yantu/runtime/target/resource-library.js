(() => {
    const W = Workspace;
    addEventListener('study-detail', event => {
        const record = W.record(event.detail.id)?.record;
        if (!record) return;
        const target = document.getElementById('studyConnections');
        if (!target) return;
        const section = document.createElement('section');
        if (record.importBatch) {
            const status = document.createElement('p');
            status.textContent = ({'not-started':'未开始', reading:'阅读中', completed:'已读'}[record.studyStatus] || '整理资料，尚无学习记录') +
                (record.verification ? ' · ' + ({content:'局部内容核验', outline:'目录级整理', 'name-only':'名称待核验', unreadable:'读取受阻'}[record.verification] || '核验范围待确认') : '');
            section.append(status);
        }
        for (const id of record.boardIds || []) {
            const board = W.record(id)?.record;
            section.append(W.button(board?.title || '关联导图', 'diagram-project', () => KnowledgeBoard.open(id)));
        }
        const related = record.relatedResources || (record.linkedIds || []).map(id => ({id, title:W.record(id)?.record.title, verified:true}));
        if (related.length) {
            const details = document.createElement('details'), summary = document.createElement('summary');
            summary.textContent = '关联资料 · ' + related.length; details.append(summary);
            for (const item of related) {
                const source = W.record(item.id)?.record;
                if (!source) continue;
                const answer = item.role === 'solution' || source.role === 'solution';
                details.append(W.button((answer ? '答案解析 · ' : '') + (item.title || source.title) + (item.verified === false ? '（配套待核对）' : ''), answer ? 'eye' : 'link', () => {
                    if (!answer || confirm('现在打开答案解析？未作答时可以选择取消。')) W.openRecord(item.id);
                }));
            }
            section.append(details);
        }
        if (record.sourceAliases?.length) {
            const details = document.createElement('details'), summary = document.createElement('summary'), text = document.createElement('p');
            summary.textContent = '原文件名称 · ' + record.sourceAliases.length; text.textContent = record.sourceAliases.join('\n'); details.append(summary, text); section.append(details);
        }
        target.append(section);
    });
})();
