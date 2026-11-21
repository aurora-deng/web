/* A01/A02 independent module. No auto-mount, shared overrides or import-time IO.
 * Parent contract: assistant_drafts.py module docstring. Configure request/accept,
 * then mount(host), or install() after workspace-ready. Never use StudyAI.send or
 * DeviceAI.send here: their retrieval/memory scope is wider than selected sources.
 * DeviceStore rescue prefix: assistant-local-. Copies are per mounted session,
 * so resuming in another tab cannot overwrite an earlier offline draft.
 */
(() => {
    'use strict';
    const collections = ['notes', 'mistakes', 'resources', 'papers', 'mindmaps'];
    const kinds = {card: '背诵卡', question: '练习题', tags: '标签建议', review: '复习安排', week: '周计划'};
    const modes = {hint: '只提示一步', errors: '指出错误', explain: '完整讲解'};
    const bases = {quote: '原文引用', inference: '依据原文推断', common: '常识补充', unread: '未读到'};
    const clone = value => structuredClone(value);
    const id = () => crypto.randomUUID();
    const canonical = value => JSON.stringify(value, (_, item) => item && typeof item === 'object' && !Array.isArray(item) ? Object.fromEntries(Object.keys(item).sort().map(key => [key, item[key]])) : item);
    const fail = message => { throw new Error(message); };
    const object = (value, keys) => {
        if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).sort().join('|') !== [...keys].sort().join('|')) fail('输出字段不符合固定格式');
    };
    const string = (value, max, blank = false) => {
        if (typeof value !== 'string' || value.length > max || (!blank && !value.trim())) fail('文字为空或超限');
    };
    function validate(output, selected, purpose, mode) {
        const source = new Map(selected.map(item => [item.key, item]));
        if (purpose === 'tutor') {
            object(output, ['mode', 'claims']);
            if (output.mode !== mode || !Array.isArray(output.claims) || output.claims.length < 1 || output.claims.length > (mode === 'hint' ? 1 : 16)) fail('辅导层级或分点数量不符');
            for (const claim of output.claims) {
                object(claim, ['basis', 'text', 'sourceKey', 'quote']);
                string(claim.text, mode === 'hint' ? 1200 : 4000);
                if (['quote', 'inference'].includes(claim.basis)) {
                    string(claim.quote, 2000);
                    if (!source.get(claim.sourceKey)?.excerpt.includes(claim.quote)) fail('引用不在所选原文中');
                } else if (!['common', 'unread'].includes(claim.basis) || claim.sourceKey !== '' || claim.quote !== '') fail('结论依据或来源无效');
            }
        } else {
            object(output, ['items']);
            if (!kinds[purpose] || !Array.isArray(output.items) || !output.items.length || output.items.length > 20) fail('草稿数量或类型无效');
            const fields = {card: ['title', 'front', 'back'], question: ['title', 'content', 'solution', 'hints'], tags: ['tags'], review: ['title', 'date', 'startTime', 'durationMinutes'], week: ['title', 'date', 'startTime', 'durationMinutes']}[purpose];
            for (const item of output.items) {
                object(item, ['kind', 'sourceKey', ...fields]);
                if (item.kind !== purpose || !source.has(item.sourceKey)) fail('草稿指向未选定来源');
                for (const field of fields) {
                    const value = item[field];
                    if (field === 'hints' || field === 'tags') {
                        if (!Array.isArray(value) || value.length > (field === 'hints' ? 8 : 20) || (field === 'tags' && !value.length) || new Set(value).size !== value.length) fail('提示或标签无效');
                        value.forEach(entry => string(entry, field === 'hints' ? 600 : 80));
                    } else if (field === 'durationMinutes') {
                        if (!Number.isInteger(value) || value < 1 || value > 1440) fail('分钟数无效');
                    } else if (field === 'date') {
                        if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}$/.test(value) || !Number.isFinite(Date.parse(value)) || new Date(value).toISOString().slice(0, 10) !== value) fail('日期无效');
                    } else if (field === 'startTime') {
                        if (typeof value !== 'string' || (value && !/^([01]\d|2[0-3]):[0-5]\d$/.test(value))) fail('开始时间无效');
                    } else string(value, field === 'title' ? 300 : 6000);
                }
                if (item.startTime) {
                    const [hour, minute] = item.startTime.split(':').map(Number);
                    if (hour * 60 + minute + item.durationMinutes > 1440) fail('时间块不能跨午夜');
                }
            }
        }
        return clone(output);
    }
    let settings = {}, installed;
    function configure(value) { settings = {...settings, ...value}; }
    function element(tag, text, className) {
        const node = document.createElement(tag);
        if (text !== undefined) node.textContent = text;
        if (className) node.className = className;
        return node;
    }
    function currentState() { const value=settings.state ? settings.state() : typeof state !== 'undefined' ? state : {};return window.YantuActivity?YantuActivity.aiSafeState(value):value; }
    function recordExcerpt(record) { return ['title', 'content', 'text', 'solution', 'front', 'back'].filter(key => typeof record[key] === 'string' && record[key].trim()).map(key => record[key]).join('\n\n'); }
    function filesOf(record) { return window.StudyLibrary ? StudyLibrary.files(record) : record.files || []; }
    function snapshot(collection, record, excerpt = recordExcerpt(record), page = {}) {
        if (!collections.includes(collection)) fail('资料类型不允许');
        string(excerpt, 6000);
        return {key: id(), collection, record: clone(record), excerpt, fileId: page.fileId || '', page: page.page ?? null, fingerprint: page.fingerprint || '', method: page.method || 'record'};
    }
    function mount(host) {
        if (!host || !window.DeviceStore || !window.Workspace) fail('需要资料工作区与本机存储');
        const W = window.Workspace;
        const localKey = 'assistant-local-' + id();
        let session = {version: 1, id: id(), sources: [], thought: '', request: '', purpose: 'tutor', mode: 'hint', output: null, draft: null, skipped: [], receipts: {}}, preview = null;
        let tail = Promise.resolve(), busy = false, controller, generation = 0, destroyed = false;
        host.classList.add('assistant-workspace');
        host.innerHTML = `<div class="aw-toolbar"><h2>资料辅导与草稿</h2><div class="aw-actions" data-aw="actions"></div></div>
<p class="aw-status" role="status" aria-live="polite" data-aw="status"></p>
<div class="aw-layout"><section class="aw-source-pane"><h3>本次资料</h3><div class="aw-picker" data-aw="picker"></div><div class="aw-actions" data-aw="source-actions"></div><ol class="aw-source-list" data-aw="sources"></ol></section>
<section class="aw-main"><form data-aw="form"><fieldset data-aw="fields"><div class="aw-mode" role="group" aria-label="辅导层级" data-aw="modes"></div>
<label>我的思考过程<textarea name="thought" rows="5" maxlength="4000"></textarea></label>
<label>本次问题或草稿要求<textarea name="request" rows="3" maxlength="2000" required></textarea></label>
<div class="aw-options"><label>内容<select name="purpose"><option value="tutor">分层辅导</option>${Object.entries(kinds).map(([key, name]) => `<option value="${key}">${name}</option>`).join('')}</select></label>
<label>模型<select name="provider"><option value="ollama">本机 Ollama</option><option value="api">已授权 API</option></select></label></div>
<label class="aw-consent" hidden data-aw="consent"><input name="cloud" type="checkbox">本次允许将所选片段、问题和思考发送至已配置 API</label></fieldset>
<div class="aw-actions"><button type="submit" class="btn btn-primary" data-aw="generate"><i class="fas fa-paper-plane" aria-hidden="true"></i> 生成</button><button type="button" class="btn btn-ghost" data-aw="stop" hidden><i class="fas fa-stop" aria-hidden="true"></i> 停止</button></div></form>
<div data-aw="output" class="aw-output"></div></section></div><section class="aw-saved"><h3>本机草稿</h3><div data-aw="saved"></div></section>`;
        const at = name => host.querySelector(`[data-aw="${name}"]`);
        const form = at('form');
        const status = message => { if (!destroyed) at('status').textContent = message; };
        const safe = action => async () => { try { await action(); } catch (error) { status(error.message); } };
        const button = (label, icon, action) => W.button(label, icon, safe(action));
        function read() { session.thought = form.elements.thought.value; session.request = form.elements.request.value; session.purpose = form.elements.purpose.value; }
        function persist() {
            const saved = clone(session);
            tail = tail.catch(() => {}).then(() => DeviceStore.put(localKey, saved));
            return tail.catch(() => { status('本机保存失败，请保持页面打开并导出草稿'); fail('本机保存失败，请立即导出草稿'); });
        }
        async function request(kind, body, signal) {
            if (!settings.request) fail('受限对话/逐条确认接口尚未接线，未调用模型或写入学习数据');
            return settings.request(kind, clone(body), {signal});
        }
        function setBusy(value) {
            busy = value;
            at('fields').disabled = value;
            at('generate').disabled = value;
            at('stop').hidden = !value;
            host.querySelectorAll('[data-mutating]').forEach(node => { node.disabled = value; });
        }
        function invalidate() { preview = null; }
        function renderModes() {
            at('modes').replaceChildren();
            for (const [key, label] of Object.entries(modes)) {
                const control = button(label, key === 'hint' ? 'lightbulb' : key === 'errors' ? 'magnifying-glass' : 'book-open', async () => {
                    session.mode = key; await persist(); renderModes();
                });
                control.setAttribute('aria-pressed', String(session.mode === key));
                control.dataset.mutating = '';
                control.disabled = busy;
                at('modes').append(control);
            }
        }
        function renderPicker() {
            const picker = at('picker'); picker.replaceChildren();
            for (const collection of collections) for (const record of currentState()[collection] || []) {
                const label = element('label'), input = element('input'); input.type = 'checkbox'; input.value = collection + ':' + record.id;
                input.dataset.collection = collection; input.dataset.id = record.id; input.dataset.mutating = ''; input.disabled = busy;
                label.append(input, element('span', record.title || record.id)); picker.append(label);
            }
            if (!picker.children.length) picker.textContent = '暂无可选资料';
        }
        function citation(source) {
            const details = element('details', undefined, 'aw-citation');
            details.append(element('summary', (source.record.title || source.record.id) + (source.page ? ' · 第 ' + source.page + ' 页' : ' · 正文') + (source.method.includes('ocr') ? ' · OCR 待校对' : '')));
            details.append(element('pre', source.excerpt));
            if (source.fingerprint) details.append(element('small', '版本 SHA-256 ' + source.fingerprint));
            const current = (currentState()[source.collection] || []).find(item => item.id === source.record.id);
            if (canonical(current || null) !== canonical(source.record)) details.append(element('p', '当前来源已变化或缺失；此处保留生成时快照', 'aw-warning'));
            if (source.fileId) details.append(button('定位原页', 'location-dot', async () => {
                if (settings.openSource) return settings.openSource(clone(source));
                const file = current && filesOf(current).find(file => file.id === source.fileId);
                if (!file || !window.ReaderWorkspace) fail('原页不可用，快照仍可核对');
                let fingerprint = file.sha256;
                if (!fingerprint && typeof getFile === 'function') {
                    const blob = await getFile(file.id);
                    if (blob instanceof Blob) fingerprint = await hashBlob(blob);
                }
                if (fingerprint !== source.fingerprint) fail('无法确认当前文件版本，未打开可能已替换的原页');
                ReaderWorkspace.open(current.id, filesOf(current).findIndex(entry => entry.id === file.id), source.page);
            }));
            return details;
        }
        function renderSources() {
            at('sources').replaceChildren();
            for (const source of session.sources) {
                const row = element('li'); row.append(citation(source));
                const remove = W.icon('移除此来源', 'xmark', safe(async () => {
                    if (session.output) fail('已有输出，请先新建草稿再更换来源');
                    session.sources = session.sources.filter(item => item.key !== source.key); invalidate(); await persist(); renderSources();
                }));
                remove.dataset.mutating = ''; remove.disabled = busy; row.append(remove); at('sources').append(row);
            }
        }
        function draftForOutput() { return {id: session.id, sources: clone(session.sources), kind: session.purpose, output: clone(session.output)}; }
        function readable(record, kind) {
            if (record === null) return element('p', '无（新增记录）');
            const list = element('dl', undefined, 'aw-values');
            let fields;
            if (kind === 'tags') fields = [['标签', (record.tags || []).join('、') || '无标签']];
            else if (kind === 'card') fields = [['标题', record.title], ['正面', record.front], ['背面', record.back]];
            else if (kind === 'question') {
                const question = record.items?.[0] || record;
                fields = [['标题', record.title], ['题干', question.content], ['解析', question.solution], ['提示', (question.hints || []).join('\n')]];
            } else fields = [['名称', record.title], ['日期', record.date], ['开始', record.startTime || '未固定'], ['时长', record.durationMinutes + ' 分钟']];
            for (const [label, value] of fields) { list.append(element('dt', label), element('dd', value || '无')); }
            return list;
        }
        function renderOutput() {
            const area = at('output'); area.replaceChildren();
            if (!session.output) return;
            if (!session.draft) {
                const list = element('ol');
                for (const claim of session.output.claims) {
                    const row = element('li'); row.append(element('strong', bases[claim.basis]), element('p', claim.text));
                    if (claim.sourceKey) {
                        row.append(element('blockquote', claim.quote), citation(session.sources.find(source => source.key === claim.sourceKey)));
                    }
                    list.append(row);
                }
                area.append(list); return;
            }
            area.append(element('h3', '逐条变更预览'));
            const previewButton = button('核对入库变更', 'list-check', async () => {
                const draft = clone(session.draft), signature = canonical(draft);
                const result = await request('preview', {draft});
                if (signature !== canonical(session.draft)) return;
                if (!result || typeof result.digest !== 'string' || !Array.isArray(result.rows) || result.rows.length !== draft.output.items.length) fail('预览返回无效');
                preview = result;
                renderOutput(); status('变更已核对，尚未写入学习数据');
            });
            previewButton.disabled = busy; area.append(previewButton);
            const cancel = button('取消未确认草稿', 'xmark', async () => {
                if (busy) return;
                session.skipped = session.draft.output.items.map((_, index) => String(index)).filter(key => !session.receipts[key]);
                invalidate(); await persist(); renderOutput(); status('未确认草稿已取消，没有写入学习目标');
            });
            cancel.disabled = busy; area.append(cancel);
            session.draft.output.items.forEach((item, index) => {
                const rowId = String(index), receipt = session.receipts[rowId], skipped = session.skipped.includes(rowId), article = element('article', undefined, 'aw-change');
                article.append(element('h4', (index + 1) + '. ' + kinds[item.kind] + ' · ' + (receipt === 'applied' ? '已确认' : receipt === 'undone' ? '已撤销' : skipped ? '已退回' : '待确认')));
                const entry = preview?.rows.find(row => row.rowId === rowId);
                if (entry) {
                    const source = session.draft.sources.find(source => source.key === item.sourceKey);
                    const effect = item.kind === 'tags' ? '修改「' + (source.record.title || source.record.id) + '」的标签' : '新增一条' + kinds[item.kind];
                    article.append(element('p', '影响：' + effect));
                    const diff = element('div', undefined, 'aw-diff');
                    for (const [label, value] of [['变更前', entry.before], ['变更后', entry.after]]) {
                        const section = element('section'); section.append(element('h5', label), readable(value, item.kind)); diff.append(section);
                    }
                    article.append(diff);
                } else article.append(readable(item, item.kind));
                article.append(citation(session.draft.sources.find(source => source.key === item.sourceKey)));
                const actions = element('div', undefined, 'aw-actions');
                const acknowledge = element('label', undefined, 'aw-consent'), checkbox = element('input'); checkbox.type = 'checkbox'; checkbox.setAttribute('aria-label', '确认第 ' + (index + 1) + ' 条变更');
                acknowledge.append(checkbox, element('span', '已核对本条变更'));
                const apply = button('确认本条', 'check', async () => {
                    if (!checkbox.checked || !preview || skipped || receipt) fail('请先核对并明确勾选本条');
                    await mutate('confirm', {draft: session.draft, rowId, digest: preview.digest, confirmation: 'CONFIRM_ONE_ASSISTANT_CHANGE'}, rowId);
                });
                apply.disabled = true;
                checkbox.disabled = busy || !entry || skipped || Boolean(receipt);
                checkbox.onchange = () => { apply.disabled = !checkbox.checked || busy; };
                const skip = button(skipped ? '恢复待确认' : '退回本条', 'rotate-left', async () => {
                    if (receipt) fail('已确认条目请使用撤销');
                    session.skipped = skipped ? session.skipped.filter(value => value !== rowId) : [...session.skipped, rowId]; await persist(); renderOutput();
                });
                skip.disabled = busy || Boolean(receipt);
                actions.append(acknowledge, apply, skip);
                if (receipt === 'applied') {
                    const undoCheck = element('input'); undoCheck.type = 'checkbox'; undoCheck.setAttribute('aria-label', '确认撤销第 ' + (index + 1) + ' 条');
                    const label = element('label', undefined, 'aw-consent'); label.append(undoCheck, element('span', '撤销本条'));
                    const undo = button('撤销', 'rotate-left', async () => { if (!undoCheck.checked) fail('请明确勾选撤销'); await mutate('undo', {draftId: session.draft.id, rowId, confirmation: 'UNDO_ONE_ASSISTANT_CHANGE'}, rowId); });
                    undo.disabled = true; undoCheck.onchange = () => { undo.disabled = !undoCheck.checked || busy; }; actions.append(label, undo);
                }
                article.append(actions); area.append(article);
            });
        }
        async function mutate(kind, body, rowId) {
            if (busy) return;
            setBusy(true);
            try {
                await persist();
                const result = await request(kind, body);
                if (!result?.snapshot?.state || !['applied', 'undone'].includes(result.status)) fail('确认响应无效，保留原草稿供幂等重试');
                if (settings.accept) settings.accept(result.snapshot);
                else if (window.PrepLocal?.accept) PrepLocal.accept(result.snapshot);
                session.receipts[rowId] = result.status;
                await persist(); status(result.status === 'undone' ? '本条已撤销' : '本条已确认；其他条目仍待确认');
            } finally { setBusy(false); renderOutput(); }
        }
        async function generate() {
            if (busy) return;
            read();
            if (session.output) fail('请先新建草稿；已有输出和思考过程保持不变');
            if (!session.sources.length) fail('请先明确选定本次资料');
            string(session.request, 2000);
            if (session.mode === 'errors' && session.purpose === 'tutor' && !session.thought.trim()) fail('请先写下你的思考过程');
            const provider = form.elements.provider.value, cloudConfirmed = form.elements.cloud.checked;
            if (provider === 'api' && !cloudConfirmed) fail('本次尚未同意向已配置 API 发送所选内容');
            await persist();
            const token = ++generation;
            controller = new AbortController(); setBusy(true);
            try {
                const result = await request('generate', {purpose: session.purpose, mode: session.mode, sources: session.sources, thought: session.thought, request: session.request, provider, cloudConfirmed}, controller.signal);
                if (token !== generation || controller.signal.aborted) return;
                session.output = validate(result.output, session.sources, session.purpose, session.mode);
                session.draft = session.purpose === 'tutor' ? null : draftForOutput();
                await persist(); renderOutput(); status('已保存本机草稿，尚未写入学习目标或作答结果');
            } catch (error) {
                if (token !== generation || controller?.signal.aborted) return;
                throw error;
            } finally { if (token === generation) { setBusy(false); controller = null; renderOutput(); } }
        }
        async function stop() {
            ++generation; controller?.abort(); controller = null; setBusy(false);
            await persist(); status('已停止；思考与资料快照已保留，未接纳迟到回复');
        }
        async function newDraft() {
            if (busy) fail('请先停止生成');
            read(); await persist();
            const previousKey = localKey + '-saved-' + session.id;
            await DeviceStore.put(previousKey, clone(session));
            session = {...clone(session), id: id(), output: null, draft: null, skipped: [], receipts: {}};
            form.elements.cloud.checked = false; invalidate(); await persist(); renderOutput(); await renderSaved(); status('新草稿已就绪，上一份仍在本机');
        }
        async function renderSaved() {
            const list = at('saved'); list.replaceChildren();
            const saved = await DeviceStore.entries('assistant-local-');
            const seen = new Set();
            for (const [key, value] of saved.reverse()) {
                if (!value || value.version !== 1 || seen.has(value.id)) continue;
                seen.add(value.id);
                list.append(button((value.draft ? kinds[value.draft.kind] : '辅导') + ' · ' + (value.request || value.id).slice(0, 80), 'folder-open', async () => {
                    if (busy) fail('请先停止生成');
                    read(); await persist();
                    const stored = await DeviceStore.get(key);
                    if (!stored || stored.version !== 1 || !Array.isArray(stored.sources) || stored.sources.length > 8) fail('本机草稿格式无效');
                    if (stored.output) validate(stored.output, stored.sources, stored.draft?.kind || 'tutor', stored.output.mode || stored.mode);
                    // Preserve the currently open envelope before changing this session key.
                    await DeviceStore.put(localKey + '-saved-' + session.id, clone(session));
                    session = clone(stored); invalidate(); fill(); await persist(); status('已恢复本机草稿；联网不会自动入库');
                }));
            }
        }
        function fill() {
            form.elements.thought.value = session.thought;
            form.elements.request.value = session.request;
            form.elements.purpose.value = session.draft?.kind || session.purpose;
            form.elements.provider.value = 'ollama'; form.elements.cloud.checked = false; at('consent').hidden = true;
            renderModes(); renderSources(); renderOutput();
        }
        async function selectSources(selected) {
            if (busy || session.output) fail('已有生成过程或输出，请先新建草稿');
            if (!Array.isArray(selected) || !selected.length || selected.length > 8 || canonical(selected).length > 200000) fail('请选择 1 至 8 份有限大小的资料片段');
            session.sources = clone(selected); invalidate(); await persist(); renderSources(); status('本次来源快照已保存');
        }
        async function addPage() {
            if (busy || session.output) fail('请先新建草稿再更换来源');
            const selected = [...at('picker').querySelectorAll('input:checked')];
            if (selected.length !== 1) fail('请勾选一份包含附件的资料');
            const input = selected[0], record = currentState()[input.dataset.collection].find(item => item.id === input.dataset.id);
            const files = filesOf(record);
            const dialog = W.dialog('aw-page-' + id(), '选取原文页', '<form><label>附件<select name="file"></select></label><label>页码<input name="page" type="number" min="1" max="10000" value="1" required></label><div class="aw-actions"><button type="submit" class="btn btn-primary">读取本页</button></div><p role="status"></p></form>');
            dialog.classList.add('assistant-workspace');
            const pageForm = dialog.querySelector('form');
            for (const file of files) { const option = element('option', file.name); option.value = file.id; pageForm.elements.file.append(option); }
            pageForm.onsubmit = async event => {
                event.preventDefault(); const submit = pageForm.querySelector('[type=submit]'); submit.disabled = true;
                try {
                    const file = files.find(item => item.id === pageForm.elements.file.value), page = Number(pageForm.elements.page.value);
                    if (!file || !Number.isInteger(page) || page < 1 || page > 10000) fail('附件或页码无效');
                    let fingerprint = file.sha256;
                    if (!fingerprint && typeof getFile === 'function') { const blob = await getFile(file.id); if (blob instanceof Blob) fingerprint = await hashBlob(blob); }
                    if (!/^[a-f0-9]{64}$/.test(fingerprint || '')) fail('缺少可核对的文件版本，请先将原件缓存到本机');
                    let result;
                    if (settings.readSource) result = await settings.readSource({fileId: file.id, page});
                    else if (window.PrepLocal?.jsonRequest) result = await PrepLocal.jsonRequest('/api/assistant/attachment?id=' + encodeURIComponent(file.id) + '&page=' + page);
                    else fail('原文读取接口尚未接线');
                    const chunk = result.pagesRead?.find(entry => entry.page === page) || (result.page === page && !result.pagesRead?.length ? result : null);
                    if (!chunk?.text) fail('本页未读到文字，未虚构引用；可改选已核对正文');
                    const value = snapshot(input.dataset.collection, record, chunk.text, {fileId: file.id, page, fingerprint, method: chunk.method || result.method || 'extracted'});
                    await selectSources([...session.sources, value]); dialog.close();
                } catch (error) { pageForm.querySelector('[role=status]').textContent = error.message; }
                finally { submit.disabled = false; }
            };
            dialog.addEventListener('close', () => dialog.remove(), {once: true}); dialog.showModal();
        }
        function exportDraft() {
            read();
            const blob = new Blob([JSON.stringify({format: 'yantu-assistant-local-v1', session}, null, 2)], {type: 'application/json'});
            if (window.downloadBlob) downloadBlob('assistant-' + session.id + '.json', blob);
            else { const url = URL.createObjectURL(blob), link = element('a'); link.href = url; link.download = 'assistant-' + session.id + '.json'; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); }
        }
        at('actions').append(button('新建草稿', 'plus', newDraft), button('导出草稿', 'download', exportDraft), button('刷新本机草稿', 'rotate', renderSaved));
        at('source-actions').append(button('锁定所选正文', 'check', async () => {
            const selected = [...at('picker').querySelectorAll('input:checked')].map(input => snapshot(input.dataset.collection, currentState()[input.dataset.collection].find(item => item.id === input.dataset.id)));
            await selectSources(selected);
        }), button('选取附件页', 'file-lines', addPage), button('刷新资料', 'rotate', renderPicker));
        form.onsubmit = event => { event.preventDefault(); safe(generate)(); };
        form.oninput = () => { read(); persist().catch(() => {}); };
        form.elements.provider.onchange = () => { form.elements.cloud.checked = false; at('consent').hidden = form.elements.provider.value !== 'api'; };
        at('stop').onclick = safe(stop);
        const changed = () => { renderPicker(); renderSources(); };
        window.addEventListener('workspace-change', changed);
        renderPicker(); fill(); renderSaved().catch(error => status(error.message));
        status(settings.request ? '本机 Ollama · 尚未发送资料' : '受限接口尚未接线 · 离线草稿可用');
        return {selectSources, newDraft, exportDraft, refresh: renderSaved, inspect: () => clone(session), saved: () => tail,
            destroy: () => { destroyed = true; ++generation; controller?.abort(); window.removeEventListener('workspace-change', changed); host.replaceChildren(); }};
    }
    async function hashBlob(blob) { return [...new Uint8Array(await crypto.subtle.digest('SHA-256', await blob.arrayBuffer()))].map(byte => byte.toString(16).padStart(2, '0')).join(''); }
    function install() {
        if (installed) return installed;
        const section = Workspace.view('assistant-workspace', '资料辅导与草稿', 'lightbulb', () => {});
        installed = mount(section.querySelector('.workspace-content'));
        return installed;
    }
    window.AssistantWorkspace = Object.freeze({configure, mount, install, validate, snapshot});
})();
