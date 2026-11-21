/* Load after local-runtime.js, workspace-core.js and the native task/review modules. */
(() => {
    'use strict';
    if (window.ObsidianBridge?.version === 1) return;

    const el = id => document.getElementById(id);
    const records = collection => typeof state === 'undefined' ? [] : state[collection] || [];
    const find = (collection, id) => records(collection).find(item => item.id === id);
    const linked = item => item?.source === 'obsidian';
    const participates = item => linked(item) || Boolean(item?.bridgeRef) || item?.obsidianExport === true || item?.bridgeReadonly === true;
    const mobile = () => Boolean(navigator.userAgentData?.mobile || /Android|iPhone|iPad|iPod/i.test(navigator.userAgent) || navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
    const today = () => window.PrepLocal?.dateText(PrepLocal.nowDate()) || new Date().toLocaleDateString('sv-SE');
    const toast = message => window.PrepLocal?.toast(message);
    const wrapped = new WeakSet(), locks = new WeakMap(), signatures = new WeakMap(), pendingEdits = new Set();
    const temporal = ['done', 'taskStatus', 'date', 'startDate', 'deadline', 'originalDate', 'carried'];
    const provenance = ['bridgeRef', 'source', 'bridgeReadonly', 'bridgeStatus'];
    let panel, privateArea, configForm, previewArea, conflictArea, statusArea, issueArea, messageArea, observationArea;
    const statusCells = new Map();
    let status = null, config = null, templateHeadings = [], preview = null, previewSerial = 0, previewBasis = null;
    let local = false, dirty = false, busy = false, initialized = false, epoch = 0, refreshing = null;
    let decorateQueued = false, refreshTimer, lastRefresh = 0, saveGuard = null;
    let connectionLost = false, configHash = null, configOutdated = false;

    function node(tag, className, text) {
        const value = document.createElement(tag);
        if (className) value.className = className;
        if (text !== undefined) value.textContent = text;
        return value;
    }
    function button(label, icon, action, iconOnly = false) {
        const value = node('button', 'btn btn-ghost' + (iconOnly ? ' btn-icon' : ''));
        value.type = 'button';
        const glyph = node('i', 'fas fa-' + icon);
        glyph.setAttribute('aria-hidden', 'true');
        value.append(glyph);
        if (!iconOnly) value.append(node('span', '', label));
        value.title = label;
        value.setAttribute('aria-label', label);
        value.onclick = event => {
            event.preventDefault();
            event.stopPropagation();
            Promise.resolve().then(action).catch(reportError);
        };
        return value;
    }
    function reportError(error) {
        const text = local ? String(error?.message || '操作未完成') : 'Obsidian 桥接暂不可用，请在电脑端检查';
        if (messageArea) showMessage(text, 'error');
        else toast(text);
    }
    function showMessage(text, kind = 'success', connection = false) {
        if (!messageArea) return;
        messageArea.textContent = text;
        messageArea.dataset.kind = kind;
        messageArea.dataset.connectionError = String(connection);
    }
    const api = (path, body) => PrepLocal.jsonRequest('/api/obsidian/' + path, body);
    function requirePC() {
        if (!local || mobile()) throw new Error('请在电脑本机管理 Obsidian');
    }
    function field(label, name, type = 'text') {
        const row = node('label');
        row.append(node('span', '', label));
        const input = node('input');
        input.name = name;
        input.type = type;
        row.append(input);
        return row;
    }
    function checkbox(label, name) {
        const row = field(label, name, 'checkbox');
        row.className = 'ob-checkbox';
        row.prepend(row.lastElementChild);
        return row;
    }
    function definition(target, label, value) {
        const row = node('div');
        row.append(node('dt', '', label), node('dd', '', value));
        target.append(row);
    }
    function number(value) { return Number.isFinite(Number(value)) ? Math.max(0, Number(value)) : 0; }
    function stamp(value) {
        if (!value) return '未记录';
        const date = new Date(typeof value === 'number' && value < 1e12 ? value * 1000 : value);
        return Number.isNaN(date.getTime()) ? '未记录' : date.toLocaleString('zh-CN', {hour12: false});
    }
    function showIssues(target, issues, privateMessages = local) {
        target.replaceChildren();
        if (!Array.isArray(issues) || !issues.length) return;
        if (!privateMessages) {
            target.append(node('li', '', number(issues.length) + ' 项问题待电脑端处理'));
            return;
        }
        for (const issue of issues) target.append(node('li', '', String(issue?.message || '未说明的问题')));
    }
    function renderStatus() {
        if (!statusArea) return;
        const label = connectionLost ? '连接中断' : !status ? '正在连接' : !status.enabled ? '未启用' : status.paused ? '已暂停' : '已启用';
        const values = [['桥接状态', label], ['待写入 Obsidian', status ? number(status.pending) + ' 项' : '--'],
            ['冲突', status ? number(status.conflicts) + ' 项' : '--'],
            ['已关联', status ? number(status.linkedTasks) + ' 个任务 · ' + number(status.linkedReviews) + ' 篇复盘' : '--'],
            ['最近检查', stamp(status?.lastScan)], ['最近写入', stamp(status?.lastSync)]];
        for (const [title, value] of values) {
            if (!statusCells.has(title)) {
                definition(statusArea, title, value);
                statusCells.set(title, statusArea.lastElementChild.lastElementChild);
            }
            const cell = statusCells.get(title);
            if (cell.textContent !== value) cell.textContent = value;
        }
        observationArea.textContent = local && !connectionLost && status?.enabled && !status?.paused ? ({
            missing: '今日日记尚未创建', unavailable: '今日日记暂不可用',
            found: status.observation?.sharedTasks || status.observation?.sharedReviews ? '已检测到授权内容' : '当前日记范围内没有已授权内容'
        }[status.observation?.today] || '') : '';
        showIssues(issueArea, status?.issues);
        updateControls();
    }
    function invalidatePreview() {
        preview = null;
        previewBasis = null;
        previewSerial++;
        previewArea?.replaceChildren();
        updateControls();
    }
    function clearPrivate() {
        local = false;
        config = null;
        configHash = null;
        configOutdated = false;
        dirty = false;
        templateHeadings = [];
        invalidatePreview();
        privateArea?.remove();
        privateArea = configForm = previewArea = conflictArea = null;
        showMessage('');
        issueArea?.replaceChildren();
        scheduleDecorate();
    }
    function updateControls() {
        if (!privateArea) return;
        for (const control of privateArea.querySelectorAll('button,input,select')) {
            const action = control.dataset.obAction;
            control.disabled = busy || !config;
            if (control.tagName === 'BUTTON' && connectionLost) control.disabled = true;
            if (action === 'save') control.disabled ||= !dirty || configOutdated;
            if (action === 'undo') control.disabled ||= !dirty && !configOutdated;
            if (action === 'preview') control.disabled ||= dirty || configOutdated || !config?.vaultPath;
            if (action === 'apply') control.disabled ||= dirty || !preview?.token || !el('obEnableConsent')?.checked;
            if (action === 'scan') control.disabled ||= !status?.enabled || status?.paused;
            if (action === 'pause') control.disabled ||= !status?.enabled;
            if (action === 'conflicts') control.disabled ||= !number(status?.conflicts);
        }
        const pause = privateArea.querySelector('[data-ob-action=pause]');
        if (pause && pause.dataset.paused !== String(Boolean(status?.paused))) {
            pause.dataset.paused = String(Boolean(status?.paused));
            pause.replaceChildren(node('i', 'fas fa-' + (status?.paused ? 'play' : 'pause')), node('span', '', status?.paused ? '继续桥接' : '暂停桥接'));
            pause.setAttribute('aria-label', status?.paused ? '继续桥接' : '暂停桥接');
            pause.title = pause.getAttribute('aria-label');
        }
    }
    async function operation(run, kind) {
        requirePC();
        if (busy) return;
        const currentEpoch = epoch;
        busy = true;
        showMessage(({save: '正在保存配置', preview: '正在读取共享预览', apply: '正在建立联动', scan: '正在检查日记', conflicts: '正在读取冲突', undo: '正在恢复已保存配置', resolve: '正在处理冲突'}[kind] || '正在处理'), 'progress');
        updateControls();
        try { await run(currentEpoch); }
        catch (error) { if (currentEpoch === epoch) reportError(error); }
        finally { if (currentEpoch === epoch) { busy = false; updateControls(); } }
    }
    function actionButton(label, icon, action, key) {
        const value = button(label, icon, () => operation(action, key));
        value.dataset.obAction = key;
        return value;
    }
    function headingField(label, name) {
        const row = node('label');
        row.append(node('span', '', label));
        const input = node('input');
        input.name = name;
        input.setAttribute('aria-label', label);
        input.value = config?.[name] || '';
        input.maxLength = 160;
        input.placeholder = '不导入';
        input.setAttribute('list', 'ob-headings-' + name);
        const choices = node('datalist');
        choices.id = 'ob-headings-' + name;
        for (const heading of templateHeadings) choices.append(new Option(heading, heading));
        row.append(input, choices);
        return row;
    }
    function mountPrivate(suggestion) {
        requirePC();
        privateArea?.remove();
        privateArea = node('div', 'ob-private');
        configForm = node('form', 'ob-config');
        configForm.id = 'obConfigForm';
        configForm.autocomplete = 'off';
        const root = field('Obsidian 库根目录', 'vaultPath');
        root.className = 'ob-wide';
        root.lastElementChild.required = true;
        root.lastElementChild.spellcheck = false;
        root.lastElementChild.value = config.vaultPath || suggestion || '';
        const folder = field('日记目录', 'diaryFolder');
        folder.lastElementChild.value = '日记';
        folder.lastElementChild.readOnly = true;
        const start = field('开始日期', 'startDate', 'date');
        start.lastElementChild.required = true;
        start.lastElementChild.value = config.startDate || today();
        configForm.append(root, folder, start, headingField('心得标题（可选）', 'insightHeading'), headingField('计划标题（可选）', 'planHeading'));
        const stats = checkbox('写入研途统计', 'statsEnabled');
        stats.firstElementChild.checked = config.statsEnabled === true;
        configForm.append(stats);
        const tools = node('div', 'ob-actions ob-wide');
        if (suggestion) tools.append(button('使用建议库', 'folder-open', () => {
            requirePC();
            configForm.elements.vaultPath.value = suggestion;
            changedConfig();
        }));
        const save = node('button', 'btn btn-primary');
        save.type = 'submit';
        save.dataset.obAction = 'save';
        save.append(node('i', 'fas fa-save'), node('span', '', '保存配置（保持禁用）'));
        tools.append(save);
        tools.append(actionButton('撤回修改', 'arrow-rotate-left', async current => {
            invalidatePreview();
            await loadConfig(current);
            if (current === epoch) showMessage('已恢复已保存配置');
        }, 'undo'));
        configForm.append(tools);
        configForm.addEventListener('input', changedConfig);
        configForm.addEventListener('change', changedConfig);
        configForm.onsubmit = event => { event.preventDefault(); operation(saveConfig, 'save'); };
        const warnings = node('ul', 'ob-settings-notices');
        warnings.append(node('li', '', '私有写前备份：保留 30 天，上限 100 MiB。'), node('li', '', '共享提醒：已导入的任务与复盘会同步到已配对手机。'));
        const actions = node('div', 'ob-actions');
        actions.append(
            actionButton('扫描一次', 'rotate', scan, 'scan'),
            actionButton('暂停桥接', 'pause', () => configureAction(status?.paused ? 'resume' : 'pause'), 'pause'),
            actionButton('处理冲突', 'code-compare', loadConflicts, 'conflicts'),
            actionButton('断开关联', 'link-slash', () => configureAction('disconnect'), 'disconnect')
        );
        const range = node('div', 'ob-preview-range');
        const from = field('预览起日', 'from', 'date'), to = field('预览止日', 'to', 'date');
        const history = checkbox('包含所选历史日期', 'historyConsent');
        history.firstElementChild.id = 'obHistoryConsent';
        from.lastElementChild.id = 'obPreviewFrom';
        from.lastElementChild.value = config.startDate || today();
        to.lastElementChild.id = 'obPreviewTo';
        to.lastElementChild.value = today();
        for (const input of [from.lastElementChild, to.lastElementChild]) input.addEventListener('input', () => {
            history.hidden = !from.lastElementChild.value || from.lastElementChild.value >= config.startDate;
            history.firstElementChild.checked = false;
            invalidatePreview();
        });
        history.hidden = true;
        history.firstElementChild.addEventListener('change', invalidatePreview);
        range.append(from, to, actionButton('预览变更', 'eye', makePreview, 'preview'));
        previewArea = node('div', 'ob-preview');
        conflictArea = node('div', 'ob-conflicts');
        privateArea.append(configForm, warnings, actions, range, history, previewArea, conflictArea);
        panel.insertBefore(privateArea, messageArea);
        dirty = Boolean(!config.vaultPath && suggestion || !config.startDate || config.diaryFolder !== '日记');
        updateControls();
    }
    function changedConfig() { dirty = true; invalidatePreview(); }
    function readConfig() {
        const fields = configForm.elements;
        if (fields.insightHeading.value && fields.insightHeading.value === fields.planHeading.value) throw new Error('心得与计划不能使用同一个标题');
        if (!configForm.reportValidity()) return null;
        return {
            vaultPath: fields.vaultPath.value.trim(), diaryFolder: '日记', startDate: fields.startDate.value,
            insightHeading: fields.insightHeading.value, planHeading: fields.planHeading.value,
            statsEnabled: fields.statsEnabled.checked, enabled: false, paused: false
        };
    }
    async function loadConfig(currentEpoch) {
        requirePC();
        const result = await api('config');
        if (currentEpoch !== epoch) return;
        if (result.local !== true || mobile()) { clearPrivate(); return; }
        config = result.config || {};
        configHash = result.configHash || null;
        configOutdated = false;
        templateHeadings = [...new Set((result.templateHeadings || []).filter(value => typeof value === 'string' && value.length))];
        mountPrivate(typeof result.suggestedVaultPath === 'string' ? result.suggestedVaultPath : '');
    }
    async function saveConfig(currentEpoch) {
        const next = readConfig();
        if (!next) return;
        if (!next.vaultPath) throw new Error('请填写库根目录');
        invalidatePreview();
        const saved = await api('config', {config: next, ...(configHash ? {expectedConfigHash: configHash} : {})});
        if (currentEpoch !== epoch) return;
        config = saved.config || next;
        configHash = saved.configHash || null;
        configOutdated = false;
        dirty = false;
        showMessage('配置已保存 · 桥接未启用');
        await refresh(true);
    }
    async function configureAction(action) {
        const currentEpoch = epoch;
        if (action === 'disconnect' && !confirm('断开 Obsidian 关联？')) return;
        if (action === 'resume' && !status?.enabled) throw new Error('请先预览并确认启用');
        invalidatePreview();
        const changed = await api('config', {action});
        configHash = changed.configHash || configHash;
        if (currentEpoch !== epoch) return;
        conflictArea?.replaceChildren();
        await refresh(true);
        if (currentEpoch === epoch) showMessage(({pause: '桥接已暂停', resume: '桥接已继续', disconnect: '关联已断开'}[action]));
    }
    async function settleNative() {
        await PrepLocal.flush();
        if (PrepLocal.hasPending()) throw new Error('研途仍有待同步内容，请稍后再试');
    }
    const recordBasis = () => JSON.stringify([records('tasks'), records('reviews')]);
    async function makePreview(currentEpoch) {
        if (dirty) throw new Error('请先保存配置');
        const from = el('obPreviewFrom').value, to = el('obPreviewTo').value;
        if (from && to && from > to) throw new Error('预览起日不能晚于止日');
        const history = Boolean(from && config.startDate && from < config.startDate);
        if (history && !el('obHistoryConsent')?.checked) throw new Error('请确认包含所选历史日期');
        invalidatePreview();
        const serial = previewSerial;
        await settleNative();
        if (currentEpoch !== epoch) return;
        const basis = recordBasis();
        const result = await api('preview', {...(from ? {from} : {}), ...(to ? {to} : {}), history});
        if (currentEpoch !== epoch || serial !== previewSerial || dirty) return;
        if (basis !== recordBasis()) throw new Error('研途记录已变化，请重新预览');
        if (typeof result.token !== 'string' || !result.token) throw new Error('预览缺少确认凭据，请重试');
        preview = result;
        previewBasis = basis;
        renderPreview();
        showMessage('预览已就绪');
    }
    function renderPreview() {
        previewArea.replaceChildren();
        const summary = node('dl', 'ob-summary');
        definition(summary, '日记文件', number(preview.files));
        definition(summary, '任务', number(preview.tasks));
        definition(summary, '复盘', number(preview.reviews));
        previewArea.append(summary);
        const issues = node('ul', 'ob-issues');
        showIssues(issues, preview.issues, true);
        previewArea.append(issues);
        const list = node('div', 'ob-table-scroll');
        const table = node('table');
        const header = node('tr');
        for (const title of ['类型', '日期', '标题', '变更']) header.append(node('th', '', title));
        const head = node('thead');
        head.append(header);
        const body = node('tbody');
        const kinds = {task: '任务', tasks: '任务', review: '复盘', reviews: '复盘', stats: '统计', file: '日记'};
        const actions = {create: '新增', update: '更新', import: '导入', export: '写入', skip: '跳过', unlink: '解除关联', conflict: '冲突', delete: '删除'};
        for (const change of preview.changes || []) {
            const row = node('tr');
            for (const value of [kinds[change.kind] || change.kind, change.date, change.title, actions[change.action] || change.action]) row.append(node('td', '', String(value ?? '')));
            body.append(row);
        }
        table.append(head, body);
        list.append(table);
        previewArea.append(list);
        if (!(preview.changes || []).length) previewArea.append(node('p', 'ob-muted', '无变更'));
        if (typeof preview.templatePreview === 'string' && preview.templatePreview) {
            const details = node('details', 'ob-template');
            details.append(node('summary', '', '模板预览（不覆盖原模板）'), node('pre', '', preview.templatePreview));
            const tools = node('div', 'ob-actions');
            tools.append(button('复制模板预览', 'copy', copyTemplate, true), button('下载模板预览', 'download', downloadTemplate, true));
            details.append(tools);
            previewArea.append(details);
        }
        const consent = checkbox('确认上述变更并启用', 'enableConsent');
        consent.firstElementChild.id = 'obEnableConsent';
        consent.firstElementChild.addEventListener('change', updateControls);
        const apply = actionButton('确认并启用', 'check', applyPreview, 'apply');
        apply.classList.add('btn-primary');
        const controls = node('div', 'ob-actions');
        controls.append(button('返回设置', 'arrow-left', () => { invalidatePreview(); showMessage('已返回设置'); }), apply);
        previewArea.append(consent, controls);
        updateControls();
    }
    async function copyTemplate() {
        requirePC();
        if (!preview?.templatePreview) return;
        if (!navigator.clipboard?.writeText) throw new Error('当前环境无法复制，请下载模板预览');
        await navigator.clipboard.writeText(preview.templatePreview);
        toast('模板预览已复制');
    }
    function downloadTemplate() {
        requirePC();
        if (!preview?.templatePreview) return;
        return downloadBlob('研途日记模板-预览.md', new Blob([preview.templatePreview], {type: 'text/markdown;charset=utf-8'}));
    }
    async function applyPreview(currentEpoch) {
        if (dirty || !preview?.token || !el('obEnableConsent')?.checked) throw new Error('请重新预览并确认变更');
        const token = preview.token, serial = previewSerial;
        await settleNative();
        if (currentEpoch !== epoch || serial !== previewSerial) return;
        if (previewBasis !== recordBasis()) { invalidatePreview(); throw new Error('研途记录已变化，请重新预览'); }
        try {
            await api('apply', {token, confirmation: 'ENABLE_OBSIDIAN'});
        } finally {
            if (currentEpoch === epoch) invalidatePreview();
        }
        if (currentEpoch !== epoch) return;
        await PrepLocal.pull();
        if (currentEpoch !== epoch) return;
        await refresh(true);
        if (currentEpoch === epoch) showMessage('桥接已启用');
    }
    async function scan(currentEpoch) {
        invalidatePreview();
        await settleNative();
        if (currentEpoch !== epoch) return;
        await api('scan', {});
        if (currentEpoch !== epoch) return;
        await PrepLocal.pull();
        if (currentEpoch !== epoch) return;
        await refresh(true);
        if (currentEpoch === epoch) showMessage('扫描已完成');
    }
    async function loadConflicts(currentEpoch = epoch) {
        requirePC();
        const result = await api('conflicts');
        if (currentEpoch !== epoch || !local) return;
        conflictArea.replaceChildren(node('h4', '', '冲突选择'));
        for (const conflict of result.conflicts || []) {
            const row = node('article', 'ob-conflict');
            row.append(node('h5', '', ({task: '任务', review: '复盘'}[conflict.kind] || '关联记录')), node('p', '', String(conflict.message || '内容不一致')));
            const compare = node('div', 'ob-compare');
            const labels = conflict.kind === 'fields' ? ['Obsidian 当前文件值', '研途记录值'] : ['当前版本', '传入版本'];
            for (const [label, value] of [[labels[0], conflict.current], [labels[1], conflict.incoming]]) {
                if (value === undefined) continue;
                const side = node('section');
                side.append(node('h5', '', label), node('pre', '', typeof value === 'string' ? value : JSON.stringify(value ?? null, null, 2)));
                compare.append(side);
            }
            const actions = node('div', 'ob-actions');
            const repairRequired = ['duplicate', 'missing-record'].includes(conflict.kind);
            const choices = repairRequired ? [] : conflict.kind === 'file' ? [['obsidian', '保留 Obsidian 原文']] : conflict.kind === 'fields' ? [['obsidian', '保留 Obsidian'], ['yantu', '保留研途']] : [];
            for (const [choice, label] of choices) actions.append(actionButton(label, 'check', async current => {
                invalidatePreview();
                await settleNative();
                if (current !== epoch) return;
                await api('resolve', {id: conflict.id, choice});
                if (current !== epoch) return;
                await PrepLocal.pull();
                if (current !== epoch) return;
                await refresh(true);
                if (current === epoch && local) await loadConflicts(current);
            }, 'resolve'));
            if (repairRequired || !choices.length) {
                row.append(node('p', 'ob-muted', '来源待修复'));
                actions.append(actionButton('修复后重新扫描', 'rotate', async current => {
                    await scan(current);
                    if (current === epoch && local) await loadConflicts(current);
                }, 'scan'));
            }
            row.append(compare, actions);
            conflictArea.append(row);
        }
        if (!result.conflicts?.length) conflictArea.append(node('p', 'ob-muted', '暂无冲突'));
        showMessage(result.conflicts?.length ? '冲突已载入' : '暂无冲突');
        updateControls();
    }
    async function refresh(force = false) {
        if (!panel || !window.PrepLocal) return;
        if (refreshing) { await refreshing; if (!force) return; }
        const currentEpoch = epoch;
        const request = (async () => {
            try {
                const info = await (typeof PrepLocal.info === 'function' ? PrepLocal.info() : PrepLocal.info || PrepLocal.jsonRequest('/api/info'));
                if (currentEpoch !== epoch) return;
                if (info?.local !== true || mobile()) clearPrivate();
                const result = await api('status');
                if (currentEpoch !== epoch) return;
                status = result;
                connectionLost = false;
                const isPC = info?.local === true && result.local === true && !mobile();
                if (!isPC) clearPrivate();
                local = isPC;
                renderStatus();
                if (local && !config) await loadConfig(currentEpoch);
                else if (local && result.configHash && configHash && result.configHash !== configHash) {
                    if (dirty) {
                        configOutdated = true;
                        invalidatePreview();
                        showMessage('设置已在其他页面修改，请先撤回未保存修改', 'warning');
                    } else if (!busy) await loadConfig(currentEpoch);
                }
                lastRefresh = Date.now();
                if (messageArea.dataset.connectionError === 'true') showMessage('连接已恢复');
                scheduleDecorate();
            } catch (error) {
                if (currentEpoch !== epoch) return;
                if (!local || !config || [401, 403].includes(error.status)) { clearPrivate(); status = null; }
                connectionLost = true;
                renderStatus();
                showMessage(error.status === 404 ? 'Obsidian 桥接服务尚未接入' : '电脑连接暂不可用，保留上次同步状态', 'warning', true);
            }
        })();
        refreshing = request;
        try { await request; } finally { if (refreshing === request) refreshing = null; }
    }
    async function openSource(item) {
        requirePC();
        const ref = item?.bridgeRef;
        if (typeof ref !== 'string' || !ref) throw new Error('来源尚未关联');
        const currentEpoch = epoch;
        const result = await api('source?id=' + encodeURIComponent(ref));
        if (currentEpoch !== epoch || !local) return;
        const url = new URL(result.url);
        if (url.protocol !== 'obsidian:' || /[\r\n]/.test(result.url)) throw new Error('来源链接无效');
        window.location.assign(url.href);
    }

    function describe(item) {
        if (pendingEdits.has(item?.id)) return {source: linked(item) ? 'Obsidian' : '研途', text: '已存研途 · 等待 Obsidian 写入确认', readonly: item?.bridgeReadonly === true};
        const text = {
            synced: '已写入 Obsidian', pending: '已存研途 · 待写入 Obsidian',
            conflict: '已存研途 · 冲突待处理', 'waiting-file': '已存研途 · 等待日记文件',
            unlinked: '已存研途 · 未关联'
        }[item?.bridgeStatus] || '已存研途 · 待关联';
        return {source: linked(item) ? 'Obsidian' : '研途', text, readonly: item?.bridgeReadonly === true};
    }
    function metadata(item) {
        const area = node('span', 'ob-record-meta'), info = describe(item);
        const badge = node('span', 'ob-source-badge');
        badge.append(node('i', 'fas fa-book-open'), node('span', '', info.source));
        const sync = node('span', 'ob-sync-state', info.text);
        sync.dataset.status = pendingEdits.has(item.id) ? 'pending' : ['synced', 'pending', 'conflict', 'waiting-file', 'unlinked'].includes(item.bridgeStatus) ? item.bridgeStatus : 'pending';
        area.append(badge, sync);
        if (item.taskStatus) area.append(node('span', 'ob-task-status', ({todo: '待办', in_progress: '进行中', done: '完成', cancelled: '已取消'}[item.taskStatus] || '状态待确认')));
        if (info.readonly) area.append(node('span', 'ob-readonly', '只读'));
        if (local && linked(item) && item.bridgeRef) area.append(button('在 Obsidian 打开', 'arrow-up-right-from-square', () => openSource(item), true));
        return area;
    }
    function setLocked(control, shouldLock, readonly = false) {
        if (!control) return;
        const property = readonly ? 'readOnly' : 'disabled';
        let saved = locks.get(control);
        if (shouldLock) {
            if (!saved) { saved = {}; locks.set(control, saved); }
            if (!(property in saved)) saved[property] = control[property];
            control[property] = true;
        } else if (saved && property in saved) {
            control[property] = saved[property];
            delete saved[property];
        }
    }
    function decorateRow(row, item, allowEdit = false) {
        if (!row || !item) return;
        const signature = JSON.stringify([item.id, item.bridgeRef, item.source, item.obsidianExport, item.bridgeStatus, item.taskStatus, item.bridgeReadonly, pendingEdits.has(item.id), local]);
        const checkbox = row.querySelector('input[type=checkbox]');
        setLocked(checkbox, item.bridgeReadonly === true);
        if (checkbox && item.bridgeReadonly) checkbox.checked = Boolean(item.done);
        if (signatures.get(row) === signature) return;
        signatures.set(row, signature);
        row.querySelectorAll('.ob-record-meta,.ob-task-edit').forEach(value => value.remove());
        if (!participates(item)) return;
        const target = row.querySelector('.task-meta,.journal-actions') || row;
        target.append(metadata(item));
        if (allowEdit && !item.bridgeReadonly) {
            const edit = button('编辑任务', 'pen', () => window.openTaskDialog(item.id), true);
            edit.classList.add('ob-task-edit');
            row.append(edit);
        }
    }
    function inlineId(row, name) {
        const expression = new RegExp('\\b' + name + '\\(\\s*[\'"]([^\'"]+)[\'"]');
        for (const control of row.querySelectorAll('[onclick]')) {
            const match = expression.exec(control.getAttribute('onclick'));
            if (match) return match[1];
        }
        return row.dataset.recordId || row.dataset.taskId || row.dataset.reviewId || row.dataset.obsidianTask || row.dataset.obsidianReview || row.dataset.id;
    }
    function lockTaskForm(form, item) {
        const linkedTitle = linked(item);
        setLocked(form.elements.namedItem('title'), linkedTitle, true);
        for (const name of [...temporal, 'useCurve']) {
            const control = form.elements.namedItem(name);
            if (!control) continue;
            setLocked(control, item?.bridgeReadonly === true || name === 'useCurve' && linkedTitle, control.tagName === 'INPUT' && !['checkbox', 'radio', 'hidden'].includes(control.type));
        }
        const exportToggle = form.elements.namedItem('obsidianExport');
        if (exportToggle) {
            (exportToggle.closest('label') || exportToggle).hidden = linkedTitle;
            setLocked(exportToggle, linkedTitle);
        }
        const signature = JSON.stringify([item?.id, item?.bridgeStatus, item?.bridgeRef, item?.bridgeReadonly, pendingEdits.has(item?.id), local]);
        let area = form.querySelector('.ob-form-source');
        if (signatures.get(form) !== signature) {
            signatures.set(form, signature);
            area?.remove();
            if (participates(item)) {
                area = node('div', 'ob-form-source');
                area.append(metadata(item));
                form.prepend(area);
            }
        }
    }
    function decorate() {
        for (const row of document.querySelectorAll('#taskList .task-item,[data-obsidian-task]')) decorateRow(row, find('tasks', inlineId(row, 'toggleTask')), true);
        for (const row of document.querySelectorAll('#reviewList .journal-record,[data-obsidian-review]')) decorateRow(row, find('reviews', inlineId(row, 'editReview')));
        document.querySelectorAll('#homeMustDoItems .home-task-check').forEach(row => decorateRow(row, find('tasks', row.dataset.recordId)));
        const form = el('taskForm');
        if (form) lockTaskForm(form, find('tasks', form.elements.namedItem('id')?.value));
        for (const dialog of document.querySelectorAll('dialog[id^="planningTask-"]')) {
            const form = dialog.querySelector('form');
            if (form) lockTaskForm(form, find('tasks', dialog.id.slice('planningTask-'.length)));
        }
        const reviewForm = el('reviewForm'), review = find('reviews', reviewForm?.elements.namedItem('id')?.value);
        if (reviewForm) {
            for (const name of ['insight', 'plan', 'date', 'status']) setLocked(reviewForm.elements.namedItem(name), linked(review), true);
            setLocked(reviewForm.querySelector('[type=submit]'), linked(review));
            const signature = JSON.stringify([review?.id, review?.bridgeRef, review?.bridgeStatus, pendingEdits.has(review?.id), local]);
            if (signatures.get(reviewForm) !== signature) {
                signatures.set(reviewForm, signature);
                reviewForm.querySelector('.ob-form-source')?.remove();
                if (participates(review)) {
                    const area = node('div', 'ob-form-source');
                    area.append(metadata(review));
                    reviewForm.prepend(area);
                }
            }
        }
    }
    function scheduleDecorate() {
        if (decorateQueued) return;
        decorateQueued = true;
        queueMicrotask(() => { decorateQueued = false; decorate(); });
    }
    function copyFields(target, source, fields) {
        for (const key of fields) {
            if (Object.prototype.hasOwnProperty.call(source, key)) target[key] = source[key];
            else delete target[key];
        }
    }
    // The current native handlers save synchronously. Normalize before their capture, not after it.
    function guardedSave(prepare, run) {
        const previous = saveGuard;
        saveGuard = () => { previous?.(); prepare(); };
        try { return run(); } finally { saveGuard = previous; }
    }
    function wrap(name, factory) {
        const previous = window[name];
        if (typeof previous !== 'function' || wrapped.has(previous)) return;
        const next = factory(previous);
        wrapped.add(next);
        window[name] = next;
    }
    function setupForm(collection, id) {
        const form = el(id);
        if (!form || typeof form.onsubmit !== 'function' || wrapped.has(form.onsubmit)) return;
        if (collection === 'tasks' && !form.elements.namedItem('obsidianExport')) {
            const choice = checkbox('写入 Obsidian 日记', 'obsidianExport');
            choice.firstElementChild.value = '1';
            choice.firstElementChild.defaultChecked = false;
            form.querySelector('[type=submit]')?.parentElement.before(choice);
        }
        const previous = form.onsubmit;
        form.onsubmit = function (event) {
            const id = form.elements.namedItem('id')?.value, old = find(collection, id);
            const beforeIds = new Set(records(collection).map(item => item.id));
            const exportValue = form.elements.namedItem('obsidianExport')?.checked === true;
            let savedItem;
            const result = guardedSave(() => {
                let item = id ? find(collection, id) : records(collection).find(item => !beforeIds.has(item.id));
                if (!item) return;
                if (old) {
                    const merged = {...old, ...item};
                    copyFields(merged, old, provenance);
                    if (collection === 'reviews' && linked(old)) copyFields(merged, old, ['insight', 'plan', 'date', 'status']);
                    if (collection === 'tasks') {
                        if (linked(old)) { merged.title = old.title; copyFields(merged, old, ['useCurve', 'obsidianExport']); }
                        if (old.bridgeReadonly) copyFields(merged, old, [...temporal, 'useCurve']);
                        else if (!form.elements.namedItem('done') && !form.elements.namedItem('taskStatus')) copyFields(merged, old, ['done']);
                    }
                    const index = records(collection).indexOf(item);
                    records(collection)[index] = merged;
                    item = merged;
                }
                if (collection === 'tasks' && !linked(old)) item.obsidianExport = exportValue;
                if (participates(item) && JSON.stringify(old) !== JSON.stringify(item)) pendingEdits.add(item.id);
                savedItem = item;
            }, () => previous.call(this, event));
            scheduleDecorate();
            if (participates(savedItem)) toast(savedItem.bridgeStatus === 'synced' ? '已存研途 · 等待 Obsidian 写入确认' : describe(savedItem).text);
            return result;
        };
        wrapped.add(form.onsubmit);
    }
    function installHooks() {
        wrap('saveState', previous => function (...args) { saveGuard?.(); return previous.apply(this, args); });
        setupForm('tasks', 'taskForm');
        setupForm('reviews', 'reviewForm');
        for (const name of ['renderTasks', 'renderTaskList', 'renderOverview', 'renderReview', 'editReview', 'resetReviewForm']) wrap(name, previous => function (...args) {
            const result = previous.apply(this, args);
            decorate();
            return result;
        });
        wrap('openTaskDialog', previous => function (...args) {
            const result = previous.apply(this, args);
            const form = el('taskForm'), item = find('tasks', form?.elements.namedItem('id')?.value);
            const choice = form?.elements.namedItem('obsidianExport');
            const heading = el('taskDialog')?.querySelector('.dialog-header h3');
            if (heading) heading.textContent = item ? '编辑任务' : '添加任务';
            if (choice) choice.checked = item?.obsidianExport === true;
            decorate();
            return result;
        });
        wrap('toggleTask', previous => function (id, ...args) {
            const item = find('tasks', id);
            if (item?.bridgeReadonly === true) { toast('此 Obsidian 任务只读'); scheduleDecorate(); return; }
            if (participates(item)) pendingEdits.add(id);
            try { return previous.call(this, id, ...args); } finally { scheduleDecorate(); }
        });
        wrap('autoRefreshTasks', previous => function (...args) {
            const protectedItems = records('tasks').filter(item => item.bridgeReadonly === true).map(item => [item.id, {...item}]);
            const before = new Map(records('tasks').filter(participates).map(item => [item.id, JSON.stringify(item)]));
            return guardedSave(() => {
                for (const [id, old] of protectedItems) {
                    const current = find('tasks', id);
                    if (current) copyFields(current, old, temporal);
                }
                for (const item of records('tasks')) if (before.has(item.id) && before.get(item.id) !== JSON.stringify(item)) pendingEdits.add(item.id);
            }, () => previous.apply(this, args));
        });
        wrap('renderView', previous => function (id, ...args) {
            const result = previous.call(this, id, ...args);
            scheduleDecorate();
            if (id === 'connection') refresh().catch(reportError);
            return result;
        });
        const W = window.Workspace;
        if (W && !wrapped.has(W.put)) {
            const previous = W.put;
            W.put = async function (collection, item, ...args) {
                const old = find(collection, item.id);
                if (collection === 'tasks' && old) {
                    if (linked(old) && old.title !== item.title) throw new Error('Obsidian 任务标题只读');
                    if (old.bridgeReadonly && temporal.some(key => JSON.stringify(old[key] ?? '') !== JSON.stringify(item[key] ?? ''))) throw new Error('此 Obsidian 任务的状态与日期只读');
                }
                if (collection === 'reviews' && linked(old) && ['insight', 'plan'].some(key => old[key] !== item[key])) throw new Error('Obsidian 复盘正文只读');
                if (participates(item) && JSON.stringify(old) !== JSON.stringify(item)) pendingEdits.add(item.id);
                return previous.call(this, collection, item, ...args);
            };
            wrapped.add(W.put);
        }
    }
    function init() {
        if (initialized || !window.PrepLocal || !el('view-connection')) return;
        initialized = true;
        panel = node('section', 'obsidian-bridge');
        panel.id = 'obsidianBridge';
        panel.setAttribute('aria-labelledby', 'obBridgeHeading');
        const header = node('header', 'ob-header'), heading = node('h3', '', 'Obsidian 日记');
        heading.id = 'obBridgeHeading';
        header.append(heading, button('刷新 Obsidian 状态', 'rotate', () => refresh(true), true));
        statusArea = node('dl', 'ob-summary');
        issueArea = node('ul', 'ob-issues');
        observationArea = node('p', 'ob-observation');
        messageArea = node('p', 'ob-message');
        messageArea.setAttribute('role', 'status');
        messageArea.setAttribute('aria-live', 'polite');
        panel.append(header, statusArea, observationArea, issueArea, messageArea);
        el('view-connection').append(panel);
        installHooks();
        decorate();
        renderStatus();
        refresh().catch(reportError);
        const observer = new MutationObserver(scheduleDecorate);
        for (const id of ['taskList', 'reviewList', 'homeMustDoItems', 'taskForm', 'reviewForm']) if (el(id)) observer.observe(el(id), {childList: true, subtree: true});
        observer.observe(document.body, {childList: true});
        document.addEventListener('submit', event => {
            const dialog = event.target.closest?.('dialog[id^="planningTask-"]');
            if (!dialog) return;
            const item = find('tasks', dialog.id.slice('planningTask-'.length));
            if (item?.bridgeReadonly) {
                event.preventDefault();
                event.stopImmediatePropagation();
                toast('此 Obsidian 任务的状态与日期只读');
            }
        }, true);
        refreshTimer = setInterval(() => {
            if (!document.hidden && el('view-connection')?.classList.contains('active') && !busy) refresh().catch(reportError);
        }, 5000);
        addEventListener('pagehide', () => clearInterval(refreshTimer), {once: true});
    }
    addEventListener('workspace-ready', () => { init(); installHooks(); scheduleDecorate(); if (!status || connectionLost) refresh().catch(reportError); });
    for (const name of ['workspace-change', 'yantu-sync']) addEventListener(name, () => {
        if (name === 'yantu-sync' && !window.PrepLocal?.hasPending()) pendingEdits.clear();
        if (preview && previewBasis !== recordBasis()) invalidatePreview();
        scheduleDecorate();
        if (!document.hidden && el('view-connection')?.classList.contains('active') && Date.now() - lastRefresh > 5000 && !busy) refresh().catch(reportError);
    });
    document.addEventListener('visibilitychange', () => {
        if (!document.hidden && el('view-connection')?.classList.contains('active') && !busy) refresh().catch(reportError);
    });
    addEventListener('online', () => { if (!busy) refresh().catch(reportError); });
    addEventListener('pc-host-change', () => {
        epoch++;
        busy = false;
        pendingEdits.clear();
        status = null;
        clearPrivate();
        renderStatus();
        refresh(true).catch(reportError);
    });
    window.ObsidianBridge = Object.freeze({version: 1, init, refresh, decorate, openSource, describe});
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init, {once: true});
    else init();
})();
