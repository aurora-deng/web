(() => {
    'use strict';
    const W = window.Workspace;
    if (!W) return;
    const PREFIX = 'subject-tools-draft-';
    const {el, clone} = W;
    const button = (label, name, action, kind) => W.button(label, name, () => Promise.resolve().then(action), kind);
    const icon = (label, name, action) => W.icon(label, name, () => Promise.resolve().then(action));
    const configs = {
        vocabulary: {label: '语境生词', icon: 'language', fields: [
            ['word', '单词 / 词组', 'input', true, 300], ['sentence', '原句', 'text', true],
            ['meaning', '语境中的词义', 'text', true], ['example', '自己的例句', 'text']]},
        readingAnalyses: {label: '阅读复盘', icon: 'book-open', fields: [
            ['passage', '原文 / 长句', 'text', true], ['ownAnswer', '独立作答与理解', 'text'],
            ['structure', '长句结构', 'text'], ['paragraphRoles', '段落作用与行文关系', 'text']]},
        writingProjects: {label: '写作与翻译', icon: 'pen-nib', fields: [
            ['kind', '类型', [['writing', '写作'], ['translation', '翻译']]],
            ['prompt', '题目 / 翻译材料', 'text'], ['original', '原稿', 'text', true],
            ['patterns', '句型积累', 'text'], ['commonErrors', '常见错误', 'text']]},
        studyTemplates: {label: '数学与专业课', icon: 'square-root-variable', fields: [
            ['kind', '类型', [['math', '数学'], ['cs', '计算机 / 专业课']]],
            ['question', '题目 / 自测问题', 'text', true], ['conditions', '适用条件', 'text'],
            ['steps', '关键步骤', 'text'], ['counterexamples', '反例与边界', 'text'],
            ['proof', '证明结构', 'text'], ['complexity', '时间 / 空间复杂度', 'text'],
            ['formula', '待回忆公式 / 结论', 'text'], ['codeLanguage', '代码语言', 'input', false, 80],
            ['code', '代码原文（仅文本）', 'text']]},
        memoryTopics: {label: '记忆专题', icon: 'brain', fields: [
            ['keywords', '关键词', 'text', true], ['material', '材料依据', 'text'],
            ['answerLayers', '答题层次', 'text'], ['comparisons', '易混淆对比', 'text'],
            ['applicableYear', '适用年份', 'year'], ['checkedOn', '核验日期', 'date'],
            ['currentness', '核验状态', [['unverified', '尚未核验'], ['checked', '已人工核验'], ['outdated', '已知过时']]]]},
        schoolArchives: {label: '院校与复试', icon: 'university', fields: [
            ['schoolId', '关联原院校记录', 'school', true], ['applicableYear', '招生适用年份', 'year'],
            ['checkedOn', '核验日期', 'date'],
            ['currentness', '核验状态', [['unverified', '尚未核验'], ['checked', '已人工核验'], ['outdated', '已知过时']]],
            ['requirements', '公告要求 / 核对记录', 'text'], ['selfIntroduction', '复试自述', 'text'],
            ['interviewQA', '复试问答', 'text'], ['interviewMaterials', '复试材料与出处', 'text']]}
    };
    const commonFields = [['sourceTitle', '资料 / 公告名称', 'input', false, 1000],
        ['sourceUrl', '来源链接', 'url', false, 2000], ['sourcePosition', '页码 / 段落 / 原文位置', 'input', false, 1000],
        ['sourceId', '来源原记录', 'source'], ['anchorId', '精确引用', 'anchor'], ['linkedIds', '其他关联记录', 'links']];
    let collection = 'vocabulary', section, editor, detail, form, draft, base, dirty = false, saving = false;
    let draftTail = Promise.resolve(), openGeneration = 0, renderGeneration = 0, detailGeneration = 0, initialized = false, page = 0;
    let conversion, detailCollection, detailId;
    const searches = {}, pageSize = 30, conversions = new Set();
    const now = () => (window.PrepLocal?.nowDate?.() || new Date()).toISOString();
    const today = () => now().slice(0, 10);
    const notify = message => window.PrepLocal?.toast(message);
    const safeId = value => typeof value === 'string' && /^[A-Za-z0-9_.-]{1,100}$/.test(value);
    const rows = key => state[key] || [];
    const get = (key, id) => rows(key).find(value => value.id === id);
    const same = (a, b) => JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
    const keyFor = value => PREFIX + value.collection + '-' + (value.mode === 'reading-answer' ? 'answer-' : '') + value.record.id;
    const run = action => Promise.resolve().then(action).catch(error => notify(error.message));
    function node(tag, text, className) {
        const value = document.createElement(tag);
        if (text !== undefined) value.textContent = text;
        if (className) value.className = className;
        return value;
    }
    function safeUrl(value) {
        if (!value) return '';
        if (/\s|\\/.test(value)) throw new Error('来源链接不能含空白或反斜杠');
        const url = new URL(value);
        if (!['https:', 'http:'].includes(url.protocol) || url.username || url.password) throw new Error('来源链接仅支持不含账号密码的 HTTP / HTTPS 地址');
        return url.href;
    }
    function warnings(value, key, date = today()) {
        if (!['memoryTopics', 'schoolArchives'].includes(key)) return [];
        const result = [], year = Number(date.slice(0, 4));
        if (!value.sourceTitle && !value.sourceUrl && !value.sourceId) result.push('来源尚未填写，材料依据待核对');
        if (!value.applicableYear) result.push('尚未标注适用年份');
        else if (Number(value.applicableYear) !== year) result.push(`适用年份为 ${value.applicableYear}，与当前日历年份 ${year} 不同，请核对目标考试年份`);
        if (value.currentness === 'outdated') result.push('已标记过时，不宜直接使用');
        else if (value.currentness !== 'checked' || !value.checkedOn) result.push('尚未完成有日期的人工核验');
        if (value.checkedOn && value.checkedOn > date) result.push('核验日期在未来，请核对');
        else if (value.checkedOn && Date.parse(date) - Date.parse(value.checkedOn) > 180 * 86400000) result.push('距离上次核验已超过 180 天，请重新核对');
        if (key === 'schoolArchives') result.push('招生与复试要求以对应年份的官方公告为准；档案不代表实时要求');
        return result;
    }
    function validate(key, value) {
        if (!configs[key] || !safeId(value.id) || !value.title?.trim() || value.title.length > 300) throw new Error('标题需要 1 至 300 字，记录编号必须有效');
        for (const [name, label, type, required, limit] of [...configs[key].fields, ...commonFields]) {
            const text = value[name];
            if (required && (typeof text !== 'string' || !text.trim())) throw new Error('请填写' + label);
            if (typeof text === 'string' && text.length > (limit || 100000)) throw new Error(label + '超出长度限制');
            if (type === 'year' && text && !/^(19\d{2}|20\d{2}|21\d{2}|2200)$/.test(String(text))) throw new Error('年份应为 1900 至 2200 的整数');
            if (type === 'date' && text && (!/^\d{4}-\d{2}-\d{2}$/.test(text) || new Date(text).toISOString().slice(0, 10) !== text)) throw new Error('日期无效');
            if (Array.isArray(type) && !type.some(([id]) => id === text)) throw new Error(label + '无效');
        }
        safeUrl(value.sourceUrl);
        for (const name of ['sourceId', 'anchorId', 'schoolId', 'cardId']) if (value[name] && !safeId(value[name])) throw new Error('关联编号无效');
        if (!Array.isArray(value.linkedIds) || value.linkedIds.length > 200 || value.linkedIds.some(id => !safeId(id))) throw new Error('关联记录无效或超过 200 条');
        for (const [name, max] of [['options', 50], ['checklist', 100], ['deadlines', 100], ['versions', 200]]) {
            if (value[name] !== undefined && (!Array.isArray(value[name]) || value[name].length > max)) throw new Error('条目数量超限');
            const ids = new Set();
            for (const item of value[name] || []) {
                if (!safeId(item.id) || ids.has(item.id)) throw new Error('条目编号无效或重复');
                ids.add(item.id);
                for (const text of Object.values(item)) if (typeof text === 'string' && text.length > 100000) throw new Error('条目文字超限');
                if (name === 'checklist' && (!item.text?.trim() || typeof item.done !== 'boolean')) throw new Error('请填写清单事项');
                if (name === 'deadlines' && (!item.label?.trim() || !/^\d{4}-\d{2}-\d{2}$/.test(item.date || '') || !Number.isFinite(Date.parse(item.date)) || new Date(item.date).toISOString().slice(0, 10) !== item.date)) throw new Error('截止事项及日期不能为空');
                if (name === 'versions' && (!item.text?.trim() || !item.createdAt)) throw new Error('修订稿不能为空');
            }
        }
        if (key === 'writingProjects' && !['writing', 'translation'].includes(value.kind)) throw new Error('写作类型无效');
        if (JSON.stringify(value).length > 2000000) throw new Error('单条记录超过 200 万字符，请拆分保存');
        return value;
    }
    function optionsFor(control, items, selected, multiple = false) {
        const ids = multiple ? selected || [] : [selected || ''];
        if (!multiple) control.append(new Option('未选择', ''));
        const known = new Set();
        for (const item of items) {
            control.append(new Option(item.label, item.id, false, ids.includes(item.id)));
            known.add(item.id);
        }
        for (const id of ids.filter(Boolean)) if (!known.has(id)) control.append(new Option('原关联已移除 · ' + id, id, false, true));
    }
    function field(spec, host, value = draft.record) {
        const [name, text, type, required, maximum] = spec;
        const label = node('label'), caption = node('span', text), control = document.createElement(type === 'text' ? 'textarea' : Array.isArray(type) || ['school', 'source', 'links', 'anchor'].includes(type) ? 'select' : 'input');
        control.name = name; control.id = 'subject-field-' + name; control.required = Boolean(required);
        if (type === 'text') { control.rows = name === 'code' || name === 'original' ? 7 : 4; control.maxLength = maximum || 100000; }
        else if (Array.isArray(type)) for (const [id, label] of type) control.append(new Option(label, id));
        else if (['school', 'source', 'links', 'anchor'].includes(type)) {
            let items;
            if (type === 'school') items = rows('schools').map(item => ({id: item.id, label: item.name || item.id}));
            else if (type === 'anchor') items = rows('sourceAnchors').filter(item => !value.sourceId || item.sourceId === value.sourceId).map(item => ({id: item.id, label: (W.record(item.sourceId)?.record.title || item.sourceId) + ' · 第 ' + item.page + ' 页'}));
            else items = W.allRecords().filter(item => ['notes', 'resources', 'mistakes', 'papers', 'schools', ...Object.keys(configs)].includes(item.collection) && item.record.id !== value.id).map(item => ({id: item.record.id, label: (configs[item.collection]?.label || ({notes: '笔记', resources: '资料', mistakes: '错题', papers: '试卷', schools: '院校'}[item.collection])) + ' · ' + (item.record.title || item.record.name || item.record.word || item.record.id)}));
            control.multiple = type === 'links'; if (control.multiple) control.size = 4;
            optionsFor(control, items, value[name], control.multiple);
        } else { control.type = type === 'year' ? 'number' : type === 'input' ? 'text' : type; control.maxLength = maximum || 1000; if (type === 'year') { control.min = 1900; control.max = 2200; control.step = 1; } }
        if (!['school', 'source', 'links', 'anchor'].includes(type)) control.value = value[name] ?? (Array.isArray(type) ? type[0][0] : '');
        if (name === 'original' && base?.original) control.readOnly = true;
        label.htmlFor = control.id; label.append(caption, control); host.append(label);
        return control;
    }
    function repeated(name, title, fields, max) {
        const area = node('section', undefined, 'subject-repeat'), heading = node('h4', title), list = node('div');
        area.dataset.repeat = name;
        const add = item => {
            const row = node('fieldset', undefined, 'subject-repeat-row'); row.dataset.id = item.id || uid();
            row.append(node('legend', title));
            for (const [key, caption, type = 'text'] of fields) {
                const label = node('label'), input = document.createElement(type === 'textarea' ? 'textarea' : 'input');
                input.dataset.key = key; input.setAttribute('aria-label', caption); if (type !== 'textarea') input.type = type;
                if (type === 'checkbox') input.checked = Boolean(item[key]); else { input.value = item[key] || ''; input.maxLength = 20000; }
                if (type === 'textarea') input.rows = 3;
                label.append(node('span', caption), input); row.append(label);
            }
            row.append(icon('移除此项', 'minus', () => { row.remove(); changed(); })); list.append(row);
        };
        for (const item of draft.record[name] || []) add(item);
        area.append(heading, list, button('添加' + title, 'plus', () => { if (list.children.length >= max) throw new Error('最多 ' + max + ' 项'); add({}); changed(); }));
        area._values = () => [...list.children].map(row => ({id: row.dataset.id, ...Object.fromEntries([...row.querySelectorAll('[data-key]')].map(input => [input.dataset.key, input.type === 'checkbox' ? input.checked : input.value]))}));
        return area;
    }
    function readForm() {
        if (!draft || !form) return;
        for (const control of form.querySelectorAll('[name]')) draft.record[control.name] = control.multiple ? [...control.selectedOptions].map(option => option.value) : control.value;
        for (const area of form.querySelectorAll('[data-repeat]')) draft.record[area.dataset.repeat] = area._values();
    }
    function cacheDraft() {
        if (!draft || !dirty) return draftTail;
        readForm(); draft.base = clone(base); draft.updatedAt = now();
        const value = clone(draft), key = keyFor(value);
        draftTail = draftTail.catch(() => {}).then(() => DeviceStore.put(key, value));
        draftTail.then(() => { if (draft?.record.id === value.record.id) el('subjectDraftStatus').textContent = '本机草稿已保留 · 尚未归档'; }).catch(error => { if (draft?.record.id === value.record.id) el('subjectDraftStatus').textContent = '草稿写入失败，请勿关闭页面：' + error.message; });
        return draftTail;
    }
    function changed() { if (saving) return; dirty = true; run(cacheDraft); }
    async function closeEditor() {
        if (saving) return;
        if (draft && dirty) await cacheDraft();
        editor._nativeClose(); draft = null; dirty = false; ++openGeneration; await render();
    }
    async function saveDraftCopy() {
        if (!draft || saving) return;
        readForm(); const oldKey = keyFor(draft), originalId = draft.record.id;
        draft = {...clone(draft), base: null, mode: undefined, record: {...clone(draft.record), id: uid(), title: draft.record.title.slice(0, 290) + '（草稿副本）', createdAt: now(), linkedIds: [...new Set([...draft.record.linkedIds || [], originalId])]}};
        delete draft.record.cardId; base = null; dirty = true;
        buildEditor(); await cacheDraft(); await DeviceStore.put(oldKey, undefined);
    }
    function buildEditor() {
        const key = draft.collection, value = draft.record;
        form.replaceChildren(); form.append(node('h4', configs[key].label));
        field(['title', '标题', 'input', true, 300], form);
        let analysis;
        for (const spec of configs[key].fields) {
            if (key === 'readingAnalyses' && spec[0] === 'structure') {
                analysis = node('section'); analysis.id = 'subjectEditorAnalysis'; analysis.hidden = true;
                const reveal = button('展开人工复盘', 'eye', () => {
                    if (!form.elements.ownAnswer.value.trim()) throw new Error('请先保留自己的作答与理解');
                    draft.record.analysisViewedAt ||= now(); analysis.hidden = false; reveal.hidden = true; changed();
                });
                form.append(reveal, analysis);
            }
            field(spec, analysis || form);
        }
        if (key === 'readingAnalyses') analysis.append(repeated('options', '选项复盘', [['label', '选项'], ['evidence', '原文证据', 'textarea'], ['position', '证据位置'], ['distractorReason', '干扰项原因 / 排除依据', 'textarea']], 50));
        if (key === 'writingProjects') {
            const revision = node('section', undefined, 'subject-revision-fields'); revision.append(node('h4', '追加修订版本'));
            for (const spec of [['revisionText', '本次修订稿', 'text'], ['revisionComments', '人工批注', 'text'], ['revisionSelfReview', '自评（非正式阅卷）', 'text'], ['revisionModelSuggestions', '模型建议（外部已有文本，未核验，非阅卷结论）', 'text']]) field(spec, revision);
            form.append(revision, node('p', '已有 ' + (value.versions || []).length + ' 个修订版本；历史原稿和修订不被覆盖。', 'subject-meta'));
        }
        if (key === 'schoolArchives') {
            form.append(repeated('checklist', '个人材料', [['text', '材料事项'], ['done', '已备齐', 'checkbox']], 100), repeated('deadlines', '截止事项', [['label', '事项'], ['date', '日期', 'date'], ['source', '该日期的依据 / 公告位置']], 100));
            if (!rows('schools').length) form.append(button('返回目标院校建档', 'university', async () => { await closeEditor(); window.PrepPush?.openView('schools'); }));
        }
        const sources = node('section', undefined, 'subject-source-fields'); sources.append(node('h4', '来源与关联'));
        for (const spec of commonFields) field(spec, sources); form.append(sources);
        form.elements.sourceId.onchange = () => {
            const anchor = rows('sourceAnchors').find(item => item.id === form.elements.anchorId.value);
            if (anchor && anchor.sourceId !== form.elements.sourceId.value) form.elements.anchorId.value = '';
            readForm(); const old = form.elements.anchorId.closest('label'); const host = old.parentElement; old.remove(); field(commonFields.find(item => item[0] === 'anchorId'), host); changed();
        };
        form.append(node('p', '', 'subject-meta')); form.lastChild.id = 'subjectDraftStatus'; form.lastChild.setAttribute('role', 'status');
        const conflict = node('section', undefined, 'subject-warning'); conflict.id = 'subjectDraftConflict'; conflict.hidden = true;
        conflict.append(node('p', '已归档记录与草稿基线不同。保留两份内容后再人工核对。'), button('将草稿另存为新记录', 'copy', saveDraftCopy)); form.append(conflict);
        const actions = node('div', undefined, 'subject-actions subject-editor-actions');
        actions.append(button('返回并保留草稿', 'arrow-left', closeEditor));
        const save = button('保存归档', 'floppy-disk', () => {} , 'btn-primary'); save.type = 'submit'; save.id = 'subjectSave'; actions.append(save); form.append(actions);
        form.oninput = changed; form.onchange = changed; form.onsubmit = event => { event.preventDefault(); run(saveEditor); };
    }
    async function edit(key = collection, id, sourceId, fromDraft = false) {
        if (!initialized || !configs[key] || saving) return;
        if (editor.open) await closeEditor();
        await draftTail.catch(() => {});
        const generation = ++openGeneration, current = id ? get(key, id) : null;
        const fresh = {version: 1, collection: key, record: current ? clone(current) : {id: id || uid(), title: '', linkedIds: [], createdAt: now(), sourceId: sourceId || '', ...(key === 'schoolArchives' && W.record(sourceId)?.collection === 'schools' ? {schoolId: sourceId} : {})}, base: clone(current || null)};
        const local = await DeviceStore.get(keyFor(typeof fromDraft === 'object' ? fromDraft : fresh)); if (generation !== openGeneration) return;
        if (local && (!local.record || local.collection !== key || local.record.id !== fresh.record.id)) throw new Error('草稿结构异常，未覆盖本机内容');
        if (local && (fromDraft || confirm('发现未归档的本机草稿，继续编辑？取消则保留草稿并查看已归档记录。'))) {
            draft = clone(local); base = clone(local.base); dirty = true;
        } else if (local) { if (current) await showDetail(key, current.id); return; }
        else { draft = fresh; base = clone(current || null); dirty = !current; }
        buildEditor(); editor.showModal();
        el('subjectDraftConflict').hidden = same(current, base);
        if (dirty) await cacheDraft();
        form.elements.title.focus();
    }
    async function saveEditor() {
        if (saving || !draft) return;
        readForm(); dirty = true; await cacheDraft();
        const key = draft.collection, value = clone(draft.record), current = get(key, value.id);
        if (!same(current, base)) { el('subjectDraftConflict').hidden = false; throw new Error('记录已在其他位置更新。本机草稿仍保留，请返回后对照已归档记录；未覆盖其他修改'); }
        value.title = value.title.trim(); value.linkedIds ||= [];
        const anchor = value.anchorId && get('sourceAnchors', value.anchorId);
        if (anchor && value.sourceId && anchor.sourceId !== value.sourceId) throw new Error('精确引用与来源记录不一致');
        if (key === 'writingProjects') {
            if (base?.original && value.original !== base.original) throw new Error('已归档原稿不可覆盖，请追加修订版本');
            value.versions = clone(base?.versions || value.versions || []);
            if ([value.revisionComments, value.revisionSelfReview, value.revisionModelSuggestions].some(text => text?.trim()) && !value.revisionText?.trim()) throw new Error('请先填写本次修订稿，再添加批注与建议');
            if (value.revisionText?.trim()) value.versions.push({id: uid(), text: value.revisionText, manualComments: value.revisionComments || '', selfReview: value.revisionSelfReview || '', modelSuggestions: value.revisionModelSuggestions || '', createdAt: now()});
            for (const name of ['revisionText', 'revisionComments', 'revisionSelfReview', 'revisionModelSuggestions']) delete value[name];
        }
        if (key === 'readingAnalyses' && value.ownAnswer?.trim()) value.attemptedAt ||= now();
        if (key === 'vocabulary') { value.content = value.sentence; value.solution = value.meaning; }
        if (key === 'studyTemplates') { value.content = value.question; value.solution = [value.formula, value.steps, value.proof, value.complexity, value.counterexamples].filter(Boolean).join('\n\n'); }
        value.updatedAt = now(); validate(key, value);
        saving = true;
        const controls = [...form.querySelectorAll('input,select,textarea,button')].map(control => [control, control.disabled]);
        controls.forEach(([control]) => { control.disabled = true; });
        const acceptLocalWrite = () => {
            base = clone(value); draft.record = clone(value);
            for (const name of ['revisionText', 'revisionComments', 'revisionSelfReview', 'revisionModelSuggestions']) if (form.elements[name]) form.elements[name].value = '';
        };
        try {
            await draftTail; await W.put(key, value); acceptLocalWrite(); await DeviceStore.put(keyFor(draft), undefined);
            dirty = false; draft = null; editor._nativeClose(); await render(); await showDetail(key, value.id);
        } catch (error) {
            if (same(get(key, value.id), value)) acceptLocalWrite();
            draft.base = clone(base); await cacheDraft().catch(() => {}); throw error;
        } finally { saving = false; controls.forEach(([control, disabled]) => { control.disabled = disabled; }); }
    }
    function textBlock(host, title, text) {
        const area = node('section', undefined, 'subject-block'); area.append(node('h4', title), node('div', text || '尚未填写', 'subject-text')); host.append(area); return area;
    }
    function showLinks(host, value) {
        const area = node('section', undefined, 'subject-block'); area.append(node('h4', '来源与关联'));
        const ids = [...new Set([value.schoolId, value.sourceId, ...value.linkedIds || [], ...rows('flashcards').filter(card => card.sourceId === value.id).map(card => card.id)].filter(Boolean))];
        if (value.sourceTitle || value.sourcePosition) area.append(node('p', [value.sourceTitle, value.sourcePosition].filter(Boolean).join(' · '), 'subject-text'));
        if (value.sourceUrl) {
            try { const a = node('a', value.sourceUrl); a.href = safeUrl(value.sourceUrl); a.target = '_blank'; a.rel = 'noopener noreferrer'; area.append(a); } catch (_) { area.append(node('p', '来源链接格式无效，未开放跳转', 'subject-warning')); }
        }
        for (const id of ids) {
            const found = W.record(id);
            if (!found) { area.append(node('p', '原关联已移除 · ' + id, 'subject-warning')); continue; }
            area.append(button(found.record.title || found.record.name || found.record.word || id, 'arrow-up-right-from-square', () => { detail.close(); W.openRecord(id); }));
        }
        if (value.anchorId) area.append(button('查看原文位置', 'location-dot', () => {
            if (!get('sourceAnchors', value.anchorId)) throw new Error('原文引用已移除');
            if (!window.ReaderWorkspace?.openAnchor) throw new Error('阅读工作区尚未就绪');
            detail.close(); return ReaderWorkspace.openAnchor(value.anchorId);
        }));
        if (!ids.length && !value.sourceTitle && !value.sourceUrl) area.append(node('p', '来源尚未填写', 'subject-meta'));
        host.append(area);
    }
    function writingDetail(host, value) {
        const controls = node('div', undefined, 'subject-compare-controls'), baseline = document.createElement('select'), revision = document.createElement('select');
        baseline.setAttribute('aria-label', '对照基线'); revision.setAttribute('aria-label', '修订版本');
        baseline.append(new Option('原稿', 'original'));
        for (const [index, version] of (value.versions || []).entries()) { const label = '版本 ' + (index + 1) + ' · ' + version.createdAt.slice(0, 16).replace('T', ' '); baseline.append(new Option(label, version.id)); revision.append(new Option(label, version.id)); }
        controls.append(node('span', '对照基线'), baseline, node('span', '修订版本'), revision); host.append(controls);
        const layout = node('div', undefined, 'subject-writing-layout'), comparison = node('div', undefined, 'subject-comparison'), comments = node('aside'); layout.append(comparison, comments); host.append(layout);
        const draw = () => {
            comparison.replaceChildren(); comments.replaceChildren();
            const left = baseline.value === 'original' ? value.original : value.versions.find(item => item.id === baseline.value)?.text;
            const right = value.versions?.find(item => item.id === revision.value);
            textBlock(comparison, baseline.value === 'original' ? '原稿' : '历史修订', left);
            textBlock(comparison, '选定修订稿', right?.text || '尚无修订版本');
            textBlock(comments, '人工批注', right?.manualComments); textBlock(comments, '自评（非正式阅卷）', right?.selfReview);
            textBlock(comments, '模型建议 · 未核验，非阅卷结论', right?.modelSuggestions).classList.add('subject-model-notes');
        };
        if (value.versions?.length) revision.value = value.versions.at(-1).id; else revision.disabled = true;
        baseline.onchange = revision.onchange = draw; draw();
        textBlock(host, '题目 / 翻译材料', value.prompt); textBlock(host, '句型积累', value.patterns); textBlock(host, '常见错误', value.commonErrors);
    }
    async function readingDetail(host, value) {
        textBlock(host, '原文 / 长句', value.passage);
        const localKey = keyFor({collection: 'readingAnalyses', mode: 'reading-answer', record: value}), local = await DeviceStore.get(localKey);
        const answerForm = node('form', undefined, 'subject-reading-answer'), label = node('label'), answer = document.createElement('textarea');
        answer.name = 'ownAnswer'; answer.rows = 4; answer.maxLength = 100000; answer.value = local?.record.ownAnswer ?? value.ownAnswer ?? ''; label.append(node('span', '独立作答与理解'), answer); answerForm.append(label);
        if (local) answerForm.append(node('p', '已恢复本机作答草稿，尚未归档', 'subject-meta'));
        const reveal = button('保存作答并展开复盘', 'eye', () => {}, 'btn-primary'); reveal.type = 'submit'; answerForm.append(reveal); host.append(answerForm);
        const analysis = node('section'); analysis.id = 'subjectReadingAnalysis'; analysis.hidden = true;
        textBlock(analysis, '长句结构', value.structure); textBlock(analysis, '段落作用与行文关系', value.paragraphRoles);
        for (const item of value.options || []) {
            const row = node('section', undefined, 'subject-option'); row.append(node('h4', '选项 ' + (item.label || '未标注')));
            textBlock(row, '原文证据', item.evidence); textBlock(row, '证据位置', item.position); textBlock(row, '干扰项原因 / 排除依据', item.distractorReason); analysis.append(row);
        }
        host.append(analysis);
        let answerTail = Promise.resolve(), revealBase = clone(local?.base || value);
        const cache = () => {
            const local = {version: 1, collection: 'readingAnalyses', mode: 'reading-answer', record: {...clone(revealBase), ownAnswer: answer.value}, base: clone(revealBase), updatedAt: now()};
            answerTail = answerTail.catch(() => {}).then(() => DeviceStore.put(keyFor(local), local)); return answerTail;
        };
        answer.oninput = () => { analysis.hidden = true; run(cache); };
        answerForm.onsubmit = event => { event.preventDefault(); run(async () => {
            if (!answer.value.trim()) throw new Error('请先填写自己的作答与理解');
            reveal.disabled = true;
            try {
                const existingDraft = await DeviceStore.get(PREFIX + 'readingAnalyses-' + value.id);
                if (existingDraft) throw new Error('此记录另有编辑草稿，请先从目录继续草稿，未覆盖草稿');
                await cache(); await answerTail;
                if (!same(get('readingAnalyses', value.id), revealBase)) throw new Error('阅读记录已变化，作答草稿仍保留，请重新打开');
                const saved = {...clone(revealBase), ownAnswer: answer.value, attemptedAt: revealBase.attemptedAt || now(), analysisViewedAt: now(), updatedAt: now()};
                validate('readingAnalyses', saved); await W.put('readingAnalyses', saved); revealBase = clone(saved);
                await DeviceStore.put(localKey, undefined); analysis.hidden = false;
            } finally { reveal.disabled = false; }
        }); };
    }
    async function showDetail(key, id) {
        const value = get(key, id); if (!value) throw new Error('记录已移除');
        const generation = ++detailGeneration, changedRecord = detailCollection !== key || detailId !== id;
        const host = node('div');
        host.append(node('h3', value.title, 'subject-detail-title'));
        const actions = node('div', undefined, 'subject-actions'); actions.append(button('编辑', 'pen-to-square', async () => { detail.close(); await edit(key, id); }, 'btn-primary'));
        if (['vocabulary', 'studyTemplates'].includes(key)) actions.append(button(value.cardId ? '查看 / 核对背诵卡' : '转为背诵卡', 'layer-group', () => previewCard(key, id)));
        host.append(actions);
        for (const warning of warnings(value, key)) host.append(node('p', warning, 'subject-warning'));
        if (key === 'writingProjects') writingDetail(host, value);
        else if (key === 'readingAnalyses') await readingDetail(host, value);
        else {
            for (const [name, label, type] of configs[key].fields) {
                if (name === 'schoolId' || name === 'codeLanguage') continue;
                if (name === 'formula') { const hidden = node('details', undefined, 'subject-selftest'); hidden.append(node('summary', '核对公式 / 结论')); textBlock(hidden, label, value[name]); host.append(hidden); }
                else if (name === 'code') { const area = node('section', undefined, 'subject-block'); area.append(node('h4', '代码原文 · ' + (value.codeLanguage || '未标注语言')), node('pre', value.code || '尚未填写', 'subject-code')); host.append(area); }
                else textBlock(host, label, Array.isArray(type) ? type.find(([id]) => id === value[name])?.[1] : value[name]);
            }
            if (key === 'schoolArchives') {
                const checklist = node('section', undefined, 'subject-block'); checklist.append(node('h4', '个人材料清单'));
                for (const item of value.checklist || []) {
                    const label = node('label', undefined, 'subject-check'), check = document.createElement('input'); check.type = 'checkbox'; check.checked = item.done; label.append(check, node('span', item.text)); checklist.append(label);
                    check.onchange = () => run(async () => {
                        check.disabled = true;
                        try { const latest = get(key, id); if (!same(latest, value)) throw new Error('档案已变化，请重新打开再核对清单'); const updated = clone(value); updated.checklist.find(row => row.id === item.id).done = check.checked; updated.updatedAt = now(); await W.put(key, updated); await showDetail(key, id); }
                        catch (error) { check.checked = item.done; throw error; } finally { check.disabled = false; }
                    });
                }
                if (!value.checklist?.length) checklist.append(node('p', '尚未录入材料事项', 'subject-meta')); host.append(checklist);
                const deadlines = node('section', undefined, 'subject-block'); deadlines.append(node('h4', '截止事项'));
                for (const item of [...value.deadlines || []].sort((a, b) => a.date.localeCompare(b.date))) deadlines.append(node('p', `${item.date} · ${item.label}${item.date < today() ? ' · 所填日期已过' : item.date === today() ? ' · 所填日期为今天' : ''}\n依据：${item.source || '尚未填写，待核对'}`, 'subject-text'));
                if (!value.deadlines?.length) deadlines.append(node('p', '尚未录入截止事项', 'subject-meta')); host.append(deadlines);
            }
        }
        showLinks(host, value);
        if (generation !== detailGeneration) return;
        detailCollection = key; detailId = id; el('subjectDetailBody').replaceChildren(...host.childNodes);
        if (!detail.open) detail.showModal();
        if (changedRecord) detail.scrollTop = 0;
    }
    async function makeCard(key, value) {
        if (!['vocabulary', 'studyTemplates'].includes(key)) throw new Error('该类型没有建卡操作');
        const digest = [...new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(key + ':' + value.id)))].map(byte => byte.toString(16).padStart(2, '0')).join('');
        return {id: 'subject-card-' + digest, title: value.title, type: 'qa', paused: false,
            front: key === 'vocabulary' ? value.word + '\n\n' + value.sentence : value.question + (value.conditions ? '\n\n条件：' + value.conditions : ''),
            back: key === 'vocabulary' ? value.meaning + (value.example ? '\n\n例句：' + value.example : '') : [value.formula, value.steps, value.proof, value.complexity, value.counterexamples].filter(Boolean).join('\n\n'),
            sourceId: value.id, sourceCollection: key, knowledgeIds: clone(value.knowledgeIds || []),
            sourceSnapshot: {content: value.content || '', solution: value.solution || '', updatedAt: value.updatedAt || ''},
            subjectSnapshot: clone(value), createdAt: now(), updatedAt: now(), files: []};
    }
    async function previewCard(key, id) {
        const value = get(key, id); if (!value) throw new Error('原记录已移除');
        const card = await makeCard(key, value), existing = get('flashcards', value.cardId) || get('flashcards', card.id) || rows('flashcards').find(item => item.sourceId === id && item.sourceCollection === key);
        if (!card.front?.trim() || !card.back?.trim()) throw new Error('请先补充题目与要回忆的内容，再预览建卡');
        conversion = {collection: key, source: clone(value), card, existingId: existing?.id};
        el('subjectCardPreview').replaceChildren(); textBlock(el('subjectCardPreview'), '正面', existing?.front || card.front); textBlock(el('subjectCardPreview'), '背面', existing?.back || card.back);
        el('subjectCardPreviewStatus').textContent = existing ? '已存在关联卡片；确认只补齐关联，不覆盖卡片编辑或复习安排。' : '确认后新增 1 张卡片；不生成评分，不记录复习反馈。';
        el('subjectCardConfirm').textContent = existing ? '确认关联并打开' : '确认建卡'; el('subjectCardDialog').showModal();
    }
    async function confirmCard() {
        const proposal = conversion; if (!proposal) return;
        const lock = proposal.collection + ':' + proposal.source.id; if (conversions.has(lock)) return; conversions.add(lock); el('subjectCardConfirm').disabled = true;
        try {
            const value = get(proposal.collection, proposal.source.id);
            if (!same(value, proposal.source)) throw new Error('原记录已变化，请返回重新预览');
            let existing = get('flashcards', proposal.existingId) || get('flashcards', proposal.card.id);
            if (existing && existing.sourceId !== value.id) throw new Error('卡片编号与其他来源冲突，未覆盖');
            if (!existing) { existing = proposal.card; (state.flashcards ||= []).push(existing); }
            value.cardId = existing.id;
            try { await W.persist(); }
            catch (error) { if (same(value, {...proposal.source, cardId: existing.id})) { proposal.source = clone(value); proposal.existingId = existing.id; } throw error; }
            conversion = null; el('subjectCardDialog').close(); detail.close(); W.openRecord(existing.id); await render();
        } finally { conversions.delete(lock); el('subjectCardConfirm').disabled = false; }
    }
    async function render() {
        if (!initialized) return;
        const generation = ++renderGeneration, key = collection, query = (searches[key] || '').toLocaleLowerCase();
        const drafts = (await DeviceStore.entries(PREFIX)).map(([, value]) => value).filter(value => value?.collection === key && value.record && safeId(value.record.id));
        if (generation !== renderGeneration) return;
        const items = rows(key).filter(value => !query || JSON.stringify(value).toLocaleLowerCase().includes(query)).slice().sort((a, b) => (b.updatedAt || '').localeCompare(a.updatedAt || ''));
        const host = el('subjectCatalog'); host.replaceChildren();
        for (const local of drafts.filter(value => !query || JSON.stringify(value.record).toLocaleLowerCase().includes(query))) {
            const row = node('div', undefined, 'subject-record subject-draft-row');
            row.append(button((local.record.title || '未命名') + (local.mode === 'reading-answer' ? ' · 本机作答草稿' : ' · 本机草稿'), 'pen-to-square', () => edit(key, local.record.id, undefined, local)), node('small', local.updatedAt?.slice(0, 16).replace('T', ' ') || '', 'subject-meta'));
            if (get(key, local.record.id)) row.append(icon('对照已归档记录', 'columns', () => showDetail(key, local.record.id)));
            row.append(icon('丢弃此本机草稿', 'trash', async () => { if (confirm('仅丢弃这份未归档草稿？已归档记录和原资料不变。')) { await DeviceStore.put(keyFor(local), undefined); await render(); } })); host.append(row);
        }
        page = Math.min(page, Math.max(0, Math.ceil(items.length / pageSize) - 1));
        for (const value of items.slice(page * pageSize, (page + 1) * pageSize)) {
            const row = node('div', undefined, 'subject-record');
            row.append(button(value.title || value.word || value.id, configs[key].icon, () => showDetail(key, value.id)), node('small', [value.sourceTitle, value.sourcePosition, value.applicableYear ? value.applicableYear + ' 年' : ''].filter(Boolean).join(' · '), 'subject-meta'));
            if (warnings(value, key).length) row.append(node('span', '待核对', 'subject-warning-label')); host.append(row);
        }
        if (!host.children.length) host.append(node('p', '暂无' + configs[key].label + (query ? '匹配记录' : '记录'), 'subject-meta'));
        el('subjectCount').textContent = items.length + ' 条归档 · ' + drafts.length + ' 份本机草稿';
        el('subjectPageStatus').textContent = (page + 1) + ' / ' + Math.max(1, Math.ceil(items.length / pageSize));
        el('subjectPrev').disabled = page === 0; el('subjectNext').disabled = (page + 1) * pageSize >= items.length;
        el('subjectCatalog').setAttribute('aria-labelledby', 'subject-tab-' + key);
        for (const tab of el('subjectTabs').children) { const active = tab.dataset.collection === key; tab.setAttribute('aria-selected', String(active)); tab.tabIndex = active ? 0 : -1; }
    }
    function selectCollection(key) { if (!configs[key]) return; collection = key; page = 0; el('subjectSearch').value = searches[key] || ''; run(render); }
    function setup() {
        if (initialized || !W.ready()) return; initialized = true;
        for (const key of Object.keys(configs)) state[key] ||= [];
        section = W.view('subjectTools', '科目工具', 'book', () => run(render));
        section.querySelector('.workspace-actions').append(button('新增记录', 'plus', () => edit(), 'btn-primary'));
        const host = section.querySelector('.workspace-content');
        const tabs = node('div', undefined, 'subject-tabs'); tabs.id = 'subjectTabs'; tabs.setAttribute('role', 'tablist'); tabs.setAttribute('aria-label', '科目工具类别');
        for (const [key, config] of Object.entries(configs)) { const tab = button(config.label, config.icon, () => selectCollection(key)); tab.dataset.collection = key; tab.id = 'subject-tab-' + key; tab.setAttribute('role', 'tab'); tab.setAttribute('aria-controls', 'subjectCatalog'); tabs.append(tab); }
        tabs.onkeydown = event => { if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return; event.preventDefault(); const keys = Object.keys(configs), current = keys.indexOf(collection); const index = event.key === 'Home' ? 0 : event.key === 'End' ? keys.length - 1 : (current + (event.key === 'ArrowRight' ? 1 : -1) + keys.length) % keys.length; selectCollection(keys[index]); tabs.children[index].focus(); };
        const toolbar = node('div', undefined, 'subject-toolbar'), search = document.createElement('input'); search.id = 'subjectSearch'; search.type = 'search'; search.placeholder = '搜索当前类别'; search.setAttribute('aria-label', '搜索当前类别'); search.oninput = () => { searches[collection] = search.value; page = 0; run(render); };
        const count = node('span', '', 'subject-meta'); count.id = 'subjectCount'; count.setAttribute('role', 'status'); toolbar.append(search, count);
        const list = node('div'); list.id = 'subjectCatalog'; list.setAttribute('role', 'tabpanel');
        const pagination = node('nav', undefined, 'subject-actions'); pagination.setAttribute('aria-label', '科目记录分页');
        const previous = icon('上一页', 'chevron-left', () => { page = Math.max(0, page - 1); return render(); }); previous.id = 'subjectPrev'; const next = icon('下一页', 'chevron-right', () => { page++; return render(); }); next.id = 'subjectNext'; const status = node('span', ''); status.id = 'subjectPageStatus'; pagination.append(previous, status, next);
        host.append(tabs, toolbar, list, pagination);
        editor = W.dialog('subjectEditor', '编辑学习记录', '<form id="subjectForm"></form>'); editor.classList.add('subject-dialog'); form = el('subjectForm');
        editor._nativeClose = editor.close.bind(editor); editor.close = () => run(closeEditor); editor.oncancel = event => { event.preventDefault(); run(closeEditor); };
        detail = W.dialog('subjectDetail', '学习记录', '<div id="subjectDetailBody"></div>'); detail.classList.add('subject-dialog');
        const card = W.dialog('subjectCardDialog', '确认背诵卡', '<p id="subjectCardPreviewStatus"></p><div id="subjectCardPreview"></div><div class="subject-actions"><button id="subjectCardCancel" type="button" class="btn btn-ghost">返回</button><button id="subjectCardConfirm" type="button" class="btn btn-primary">确认建卡</button></div>'); card.classList.add('subject-dialog');
        el('subjectCardCancel').onclick = () => { conversion = null; card.close(); }; el('subjectCardConfirm').onclick = () => run(confirmCard);
        addEventListener('workspace-open-record', event => {
            if (configs[event.detail.collection]) run(() => showDetail(event.detail.collection, event.detail.record.id));
            else if (event.detail.collection === 'schools') { detail.close(); if (typeof openSchoolDialog === 'function') openSchoolDialog(event.detail.record.id); else window.PrepPush?.openView('schools'); }
        });
        addEventListener('study-detail', event => {
            if (!['notes', 'resources', 'mistakes', 'papers'].includes(event.detail.col)) return;
            const connections = el('studyConnections'); if (!connections) return;
            for (const key of ['vocabulary', 'readingAnalyses', 'studyTemplates']) connections.append(button('关联' + configs[key].label, configs[key].icon, () => edit(key, undefined, event.detail.id)));
            for (const key of Object.keys(configs)) for (const value of rows(key).filter(item => item.sourceId === event.detail.id || item.linkedIds?.includes(event.detail.id))) connections.append(button(value.title, 'link', () => W.openRecord(value.id)));
        });
        const schoolDialog = el('schoolDialog');
        if (schoolDialog) {
            const linked = node('section', undefined, 'subject-block'); linked.id = 'subjectSchoolLinks'; schoolDialog.querySelector('.dialog-body').append(linked);
            schoolDialog.addEventListener('toggle', () => {
                if (!schoolDialog.open) return;
                const id = el('schoolForm')?.elements.id.value; linked.replaceChildren(); if (!id) return;
                linked.append(node('h4', '院校资料档案'), button('新增此校资料档案', 'plus', () => edit('schoolArchives', undefined, id)));
                for (const archive of rows('schoolArchives').filter(value => value.schoolId === id)) linked.append(button(archive.title, 'folder-open', () => W.openRecord(archive.id)));
            });
        }
        addEventListener('visibilitychange', () => { if (document.hidden) run(cacheDraft); });
        addEventListener('pagehide', () => { cacheDraft().catch(() => {}); });
        run(render);
    }
    addEventListener('workspace-ready', setup, {once: true}); if (W.ready()) setup();
    window.SubjectTools = {edit, open: showDetail, select: selectCollection, render, validate, warnings, makeCard, previewCard, confirmCard, close: closeEditor, save: saveEditor, draftPrefix: PREFIX, ready: () => initialized};
})();
