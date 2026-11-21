(() => {
    'use strict';
    if (window.WorkbenchExperience) return;
    const el = id => document.getElementById(id);
    const prefKey = 'workbench-preferences-v1', cacheKey = 'yantu-workbench-experience-v1';
    const draftKey = 'workbench-draft-stage-summary';
    const defaults = {fontSize:16, lineHeight:1.8, theme:'system', reducedMotion:false, showPet:true, celebrations:true};
    const primary = [
        ['today','今日','house',['overview']], ['review','复习','layer-group',['studyReview','mistakes']],
        ['library','资料','book-open',['resources']], ['plan','计划','calendar-check',['planning','timeline']],
        ['feedback','反馈','chart-column',['review']]
    ];
    const contexts = new Map(), dialogs = new WeakMap(), adapters = new Map();
    let prefs = {...defaults}, initialized = false, current = 'overview', generation = 0, refreshQueued = false;
    let lastInteraction, draftTail = Promise.resolve(), prefTail = Promise.resolve(), transfer = null, editingSummary = false;
    let baseline = null, noticeTimer, keyboardFrame, preferenceRevision = 0;
    const data = () => typeof state === 'object' ? state : {};
    const viewRoot = id => el('view-' + id);
    const date = () => window.PrepLocal?.dateText(PrepLocal.nowDate()) || new Date().toISOString().slice(0,10);
    const toast = message => window.PrepLocal?.toast(message);
    const safe = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const copy = value => JSON.parse(JSON.stringify(value));
    function normalize(value = {}) {
        return {
            fontSize:[14,16,18,20,22,24].includes(Number(value.fontSize)) ? Number(value.fontSize) : defaults.fontSize,
            lineHeight:[1.5,1.8,2,2.2].includes(Number(value.lineHeight)) ? Number(value.lineHeight) : defaults.lineHeight,
            theme:['system','light','dark'].includes(value.theme) ? value.theme : 'system',
            ...Object.fromEntries(['reducedMotion','showPet','celebrations'].map(key => [key,typeof value[key] === 'boolean' ? value[key] : defaults[key]]))
        };
    }
    try { prefs = normalize(JSON.parse(YantuEnvironment.storage.getItem(cacheKey) || '{}')); } catch (_) {}
    const motion = matchMedia('(prefers-reduced-motion: reduce)'), dark = matchMedia('(prefers-color-scheme: dark)');
    function applyPreferences() {
        const root = document.documentElement;
        root.style.setProperty('--wx-reading-size', prefs.fontSize + 'px');
        root.style.setProperty('--wx-reading-leading', String(prefs.lineHeight));
        root.dataset.wxTheme = prefs.theme === 'system' ? dark.matches ? 'dark' : 'light' : prefs.theme;
        root.dataset.wxMotion = prefs.reducedMotion || motion.matches ? 'reduce' : 'normal';
        root.dataset.wxPet = String(prefs.showPet);
        if (!prefs.celebrations) dismissCelebration();
    }
    applyPreferences();
    dark.addEventListener('change', applyPreferences); motion.addEventListener('change', applyPreferences);
    async function updatePreferences(patch) {
        prefs = normalize({...prefs,...patch}); preferenceRevision++;
        applyPreferences(); syncPreferenceForm();
        const snapshot = copy(prefs);
        prefTail = prefTail.catch(() => {}).then(async () => {
            let saved = false;
            try { YantuEnvironment.storage.setItem(cacheKey,JSON.stringify(snapshot)); saved = true; } catch (_) {}
            if (window.DeviceStore) {
                try { await DeviceStore.put(prefKey,snapshot); saved = true; }
                catch (_) { toast('阅读偏好未写入设备存储，紧急导出可能不包含本次设置'); }
            }
            if (!saved) toast('无法保存阅读偏好，本次仅在当前页面生效');
        });
        await prefTail;
        dispatchEvent(new CustomEvent('workbench-preferences',{detail:copy(prefs)}));
    }
    function button(label, name, action, iconOnly = false) {
        const value = document.createElement('button'); value.type = 'button';
        value.className = 'btn btn-ghost' + (iconOnly ? ' btn-icon' : '');
        value.innerHTML = `<i class="fas fa-${name}" aria-hidden="true"></i>${iconOnly ? '' : `<span>${safe(label)}</span>`}`;
        value.title = label; value.setAttribute('aria-label',label);
        value.addEventListener('click',() => Promise.resolve().then(action).catch(error => toast(error.message)));
        return value;
    }
    function makeDialog(id, title, content) {
        const dialog = document.createElement('dialog'); dialog.id = id; dialog.className = 'workspace-dialog wx-dialog';
        dialog.setAttribute('aria-labelledby',id + 'Title');
        dialog.innerHTML = `<header class="dialog-header"><h3 id="${id}Title">${safe(title)}</h3></header><div class="dialog-body">${content}</div>`;
        dialog.querySelector('header').prepend(button('返回','arrow-left',() => dialog.close(),true));
        document.body.append(dialog); return dialog;
    }
    function syncPreferenceForm() {
        const form = el('workbenchPreferencesForm'); if (!form) return;
        for (const [key,value] of Object.entries(prefs)) {
            const field = form.elements.namedItem(key); if (!field) continue;
            if (field.type === 'checkbox') field.checked = value; else field.value = String(value);
        }
    }
    function setupPreferences() {
        makeDialog('workbenchPreferences','阅读与陪伴',`<form id="workbenchPreferencesForm">
            <label>阅读字号<select name="fontSize">${[14,16,18,20,22,24].map(n => `<option value="${n}">${n} px</option>`).join('')}</select></label>
            <label>行距<select name="lineHeight"><option value="1.5">紧凑 · 1.5</option><option value="1.8">舒适 · 1.8</option><option value="2">宽松 · 2.0</option><option value="2.2">宽松 · 2.2</option></select></label>
            <label>外观<select name="theme"><option value="system">跟随系统</option><option value="light">浅色</option><option value="dark">暗色</option></select></label>
            <label class="wx-toggle"><input name="reducedMotion" type="checkbox" />减少动画</label>
            <label class="wx-toggle"><input name="showPet" type="checkbox" />显示桌宠</label>
            <label class="wx-toggle"><input name="celebrations" type="checkbox" />温和里程碑</label>
            <div class="dialog-actions"><button type="button" class="btn btn-ghost" id="workbenchPreferencesReset">恢复默认</button><button class="btn btn-primary" type="submit">完成</button></div></form>`);
        syncPreferenceForm();
        el('workbenchPreferencesForm').addEventListener('change',event => {
            const field = event.target;
            if (field.name in defaults) updatePreferences({[field.name]:field.type === 'checkbox' ? field.checked : field.value});
        });
        el('workbenchPreferencesForm').addEventListener('submit',event => {event.preventDefault(); el('workbenchPreferences').close();});
        el('workbenchPreferencesReset').onclick = () => updatePreferences(defaults);
        document.querySelector('.workspace-header-actions')?.append(button('阅读与陪伴','sliders',settings,true));
    }
    function settings() { syncPreferenceForm(); el('workbenchPreferences')?.showModal(); }
    function destination(key) {
        const item = primary.find(item => item[0] === key);
        return item ? item[3].find(id => viewRoot(id)) : viewRoot(key) ? key : undefined;
    }
    function navigate(key) {
        const id = destination(key); if (!id) return false;
        const nav = [...document.querySelectorAll('.sidebar .nav-item[data-view]')].find(item => item.dataset.view === id);
        if (nav) nav.click(); else if (window.Workspace) Workspace.navigate(id); else return false;
        return true;
    }
    function mountPrimary() {
        const host = document.querySelector('.workspace-header'); if (!host) return;
        const nav = document.createElement('nav'); nav.id = 'workbenchPrimary'; nav.className = 'wx-primary'; nav.setAttribute('aria-label','常用功能');
        for (const [key,label,icon] of primary) {
            const value = button(label,icon,() => navigate(key)); value.dataset.wxNav = key; nav.append(value);
        }
        const more = button('更多','bars',() => {
            const sidebar = el('sidebar'); if (!sidebar) return;
            sidebar.classList.toggle('open'); el('sidebarOverlay')?.classList.toggle('active',sidebar.classList.contains('open'));
            if (innerWidth > 768) document.querySelector('.sidebar nav')?.scrollIntoView({block:'nearest'});
            if (sidebar.classList.contains('open') || innerWidth > 768) sidebar.querySelector('.nav-item')?.focus({preventScroll:true});
            else el('workbenchMore')?.focus({preventScroll:true});
            markNavigation();
        },true);
        more.id = 'workbenchMore'; more.setAttribute('aria-controls','sidebar'); nav.append(more); host.after(nav);
        document.querySelector('.mobile-tabs')?.classList.add('wx-legacy-mobile');
        const actions = document.querySelector('.workspace-header-actions');
        const bell = actions?.querySelector('[onclick="showReminderSettings()"]') || button('提醒通知','bell',() => navigate('notifications'),true);
        bell.id = 'workbenchNotification';bell.removeAttribute('onclick');bell.onclick = () => navigate('notifications');
        bell.title = '提醒通知';bell.setAttribute('aria-label','提醒通知');
        if (actions && !bell.parentElement) actions.prepend(bell);
        const skip = document.createElement('a'); skip.className = 'wx-skip'; skip.href = '#mainContent'; skip.textContent = '跳到内容';
        skip.onclick = () => {el('mainContent').tabIndex = -1; el('mainContent').focus();}; document.body.prepend(skip);
    }
    function markNavigation() {
        document.querySelectorAll('[data-wx-nav]').forEach(value => {
            const item = primary.find(item => item[0] === value.dataset.wxNav);
            const active = item[3].includes(current); value.classList.toggle('active',active);
            if (active) value.setAttribute('aria-current','page'); else value.removeAttribute('aria-current');
        });
        const more = el('workbenchMore'); if (more) more.setAttribute('aria-expanded',String(el('sidebar')?.classList.contains('open') || innerWidth > 768));
    }
    function compactHome() {
        const home = el('view-overview'); if (!home) return;
        const redundant = el('homeMustDoItems')?.parentElement;
        const actualFocus = el('homeFocusActual')?.parentElement;
        if (redundant?.contains(actualFocus)) {
            actualFocus.classList.add('wx-focus-actual'); home.querySelector('.focus-tool')?.append(actualFocus);
        }
        if (redundant && el('taskList')) redundant.classList.add('wx-duplicate-today');
        home.querySelector('.countdown-banner')?.classList.add('wx-countdown');
        const actions = home.querySelector('.quick-actions');
        if (actions && !actions.closest('details')) {
            const details = document.createElement('details'); details.className = 'wx-home-tools';
            const summary = document.createElement('summary'); summary.textContent = '备考与数据操作';
            actions.before(details); details.append(summary,actions);
            const heading = details.previousElementSibling;
            if (heading?.classList.contains('card-header') && heading.textContent.trim() === '快捷操作') heading.classList.add('wx-duplicate-today');
        }
        if (!el('workbenchBacklog')) {
            const row = button('','calendar-check',() => navigate('plan')); row.id = 'workbenchBacklog'; row.hidden = true;
            el('taskList')?.before(row);
        }
        const day = date(), overdue = (data().tasks || []).filter(task => !task.done && task.date && task.date < day).length;
        const row = el('workbenchBacklog');
        if (row) {row.hidden = !overdue; row.querySelector('span').textContent = `${overdue} 项较早安排 · 查看计划`;}
    }
    function focusKey(node) {
        if (!node || node === document.body || node.closest('dialog')) return null;
        if (node.id) return {id:node.id};
        const keys = ['recordSelect','wxRecord','field','action','view'];
        const shell = node.closest('.library-shell');
        for (const key of keys) if (node.dataset[key]) return {key,value:node.dataset[key],col:shell?.dataset.col};
        if (node.classList.contains('library-record-title')) return {record:node.closest('[data-record-id]')?.dataset.recordId,title:node.textContent,col:shell?.dataset.col};
        return null;
    }
    function findFocus(descriptor, root) {
        if (!descriptor) return null;
        if (descriptor.id) return el(descriptor.id);
        const scope = descriptor.col ? [...root.querySelectorAll('.library-shell')].find(node => node.dataset.col === descriptor.col) : root;
        if (!scope) return null;
        if (descriptor.record) return [...scope.querySelectorAll('[data-record-id]')].find(node => node.dataset.recordId === descriptor.record)?.querySelector('.library-record-title');
        if (descriptor.title) return [...scope.querySelectorAll('.library-record-title')].find(node => node.textContent === descriptor.title);
        return [...scope.querySelectorAll('[data-' + descriptor.key.replace(/[A-Z]/g,c => '-' + c.toLowerCase()) + ']')].find(node => node.dataset[descriptor.key] === descriptor.value);
    }
    function capture(id = current) {
        const root = viewRoot(id); if (!root) return null;
        const value = {x:scrollX,y:scrollY,focus:focusKey(document.activeElement),scrollers:[],custom:undefined};
        for (const node of root.querySelectorAll('.library-folders,[data-workbench-scroll],[id]')) {
            if (node.scrollHeight > node.clientHeight && node.clientHeight && getComputedStyle(node).overflowY.match(/auto|scroll/)) {
                value.scrollers.push({id:node.id,col:node.closest('.library-shell')?.dataset.col,folders:node.classList.contains('library-folders'),top:node.scrollTop,left:node.scrollLeft});
            }
        }
        try {value.custom = adapters.get(id)?.capture();} catch (_) {}
        contexts.set(id,value); return value;
    }
    function restore(id = current, value = contexts.get(id)) {
        const request = ++generation;
        requestAnimationFrame(() => requestAnimationFrame(() => {
            if (request !== generation || id !== current || document.querySelector('dialog[open]') || !value) return;
            const root = viewRoot(id); if (!root) return;
            try {adapters.get(id)?.restore(value.custom);} catch (_) {}
            const node = findFocus(value.focus,root); if (node?.getClientRects().length) node.focus({preventScroll:true});
            for (const item of value.scrollers) {
                const target = item.id ? el(item.id) : [...root.querySelectorAll('.library-folders')].find(node => node.closest('.library-shell')?.dataset.col === item.col);
                if (target) {target.scrollTop = item.top; target.scrollLeft = item.left;}
            }
            scrollTo({left:value.x,top:value.y,behavior:'instant'});
        }));
    }
    function setupContext() {
        // Keep the existing rendering/navigation chain intact, including module-owned list state.
        if (typeof renderView === 'function') {
            const previous = renderView;
            renderView = function(id, ...args) {
                const changed = current !== id;
                if (changed && !lastInteraction?.navigating) capture(current);
                const result = previous.call(this,id,...args); current = id;
                refresh(); if (changed) restore(id); return result;
            };
        }
        document.addEventListener('click',event => {
            const node = event.target.closest?.('button,a,input,summary'); if (!node || node.closest('dialog')) return;
            lastInteraction = {view:current,value:capture(),time:performance.now(),navigating:Boolean(node.closest('.nav-item,[data-wx-nav],[data-mobile-view]'))};
            // A stale capture must not suppress later programmatic navigation.
            queueMicrotask(() => { if (lastInteraction) lastInteraction.navigating = false; });
        },true);
        addEventListener('study-detail',() => {
            const dialog = el('studyDetail'); if (dialog && !dialog.open) dialogs.set(dialog,{view:current,value:lastInteraction?.view === current && performance.now()-lastInteraction.time<500 ? lastInteraction.value : capture()});
        });
        const observer = new MutationObserver(records => {
            let inherited;
            for (const record of records) {
                const dialog = record.target;
                if (dialog.tagName !== 'DIALOG' || record.attributeName !== 'open') continue;
                if (!dialog.open && dialogs.has(dialog)) inherited = dialogs.get(dialog);
                if (dialog.open && !dialogs.has(dialog)) dialogs.set(dialog,inherited?.view === current ? inherited : {view:current,value:lastInteraction?.view === current && performance.now()-lastInteraction.time<500 ? lastInteraction.value : capture()});
            }
        });
        observer.observe(document.body,{subtree:true,attributes:true,attributeFilter:['open']});
        document.addEventListener('close',event => {
            const dialog = event.target, context = dialogs.get(dialog); dialogs.delete(dialog);
            if (context?.view === current) restore(current,context.value);
            if (dialog.id === 'moodDialog' && transfer) completeTransfer();
        },true);
    }
    function setupDetailActions() {
        const dialog = el('studyDetail'); if (!dialog) return;
        if (el('studyDetailTitle')) el('studyDetailTitle').tabIndex = 0;
        const footer = document.createElement('nav'); footer.id = 'workbenchDetailActions'; footer.setAttribute('aria-label','详情操作');
        for (const [id,label,icon] of [['studyDetailPrevious','上一条','chevron-left'],['studyDetailEdit','编辑','pen'],['studyDetailNext','下一条','chevron-right']]) {
            const value = button(label,icon,() => el(id)?.click(),id !== 'studyDetailEdit'); value.dataset.wxDelegate = id; footer.append(value);
        }
        dialog.append(footer);
        const update = () => {for (const value of footer.children) value.disabled = Boolean(el(value.dataset.wxDelegate)?.disabled);};
        addEventListener('study-detail',update); update();
    }
    function setupSummary() {
        makeDialog('workbenchSummary','阶段小结',`<form id="workbenchSummaryForm">
            <label>阶段<input name="title" maxlength="120" required /></label><div class="wx-date-pair"><label>开始日期<input name="start" type="date" required /></label><label>结束日期<input name="end" type="date" required /></label></div>
            <label>这段时间的收获<textarea name="gains" rows="4" maxlength="6000" required></textarea></label>
            <label>下一小步<textarea name="next" rows="3" maxlength="3000"></textarea></label>
            <label>留给休息与生活<textarea name="rest" rows="2" maxlength="2000"></textarea></label>
            <p id="workbenchSummaryStatus" role="status"></p><div class="dialog-actions"><button type="button" class="btn btn-ghost" id="workbenchSummaryBack">稍后继续</button><button type="submit" class="btn btn-primary"><i class="fas fa-note-sticky" aria-hidden="true"></i> 生成便签</button></div></form>`);
        const form = el('workbenchSummaryForm');
        form.addEventListener('input',() => cacheSummary());
        el('workbenchSummaryBack').onclick = () => el('workbenchSummary').close();
        el('workbenchSummary').addEventListener('close',() => {editingSummary = false; cacheSummary();});
        form.addEventListener('submit',event => {
            event.preventDefault();
            if (form.elements.end.value < form.elements.start.value) {el('workbenchSummaryStatus').textContent = '结束日期不能早于开始日期';return;}
            if (!window.MoodWall || !el('moodForm')) {toast('心情墙尚未就绪，小结草稿保留在本机');return;}
            const v = Object.fromEntries(new FormData(form));
            const text = `阶段小结：${v.title.trim()}\n${v.start} 至 ${v.end}\n\n收获\n${v.gains.trim()}${v.next.trim() ? '\n\n下一小步\n' + v.next.trim() : ''}${v.rest.trim() ? '\n\n休息与生活\n' + v.rest.trim() : ''}`;
            if (text.length > (el('moodForm').elements.text.maxLength > 0 ? el('moodForm').elements.text.maxLength : Infinity)) {el('workbenchSummaryStatus').textContent = '小结超出便签字数限制，请精简后继续';return;}
            cacheSummary(); el('workbenchSummary').close(); MoodWall.create();
            el('moodForm').elements.text.value = text;
            transfer = {text,createdAt:el('moodForm').elements.createdAt.value,ids:new Set((data().moodNotes || []).map(note => note.id))};
        });
        document.addEventListener('submit',event => {if (event.target.id === 'moodForm' && transfer) transfer.text = event.target.elements.text.value.trim();},true);
        const toolbar = document.querySelector('.mood-toolbar-actions'); if (toolbar) toolbar.append(button('阶段小结','pen-nib',summary));
    }
    function cacheSummary() {
        const form = el('workbenchSummaryForm'); if (!form) return Promise.resolve();
        const fields = Object.fromEntries(new FormData(form));
        draftTail = draftTail.catch(() => {}).then(async () => {
            if (!window.DeviceStore) {el('workbenchSummaryStatus').textContent = '设备存储不可用，关闭页面会丢失草稿';return;}
            await DeviceStore.put(draftKey,{version:1,fields,updatedAt:new Date().toISOString()});
            el('workbenchSummaryStatus').textContent = '本机草稿已保留';
        }).catch(() => {el('workbenchSummaryStatus').textContent = '草稿保存失败，请保持页面打开';});
        return draftTail;
    }
    async function summary() {
        if (editingSummary) return; editingSummary = true;
        const dialog = el('workbenchSummary'), form = el('workbenchSummaryForm');
        try {
            await draftTail;
            const saved = await window.DeviceStore?.get(draftKey);
            form.reset(); form.elements.start.value = date(); form.elements.end.value = date();
            for (const [key,value] of Object.entries(saved?.fields || {})) if (form.elements.namedItem(key) && typeof value === 'string') form.elements.namedItem(key).value = value;
            el('workbenchSummaryStatus').textContent = saved ? '已恢复本机草稿' : '';
            dialog.showModal();
        } catch (_) {editingSummary = false;toast('无法读取小结草稿，请稍后重试');}
    }
    function completeTransfer() {
        const item = transfer; transfer = null;
        if (!(data().moodNotes || []).some(note => !item.ids.has(note.id) && note.text === item.text && note.createdAt === item.createdAt)) return;
        draftTail = draftTail.catch(() => {}).then(() => DeviceStore.put(draftKey,undefined)).catch(() => toast('小结已保存，旧草稿未清除'));
        celebrate({id:'summary-saved',text:'这一段的收获，已经留下来了。'});
    }
    function dismissCelebration() {clearTimeout(noticeTimer); const value = el('workbenchMilestone'); if (value) value.hidden = true;}
    function celebrate(item) {
        if (!prefs.celebrations || !item?.text || document.hidden) return false;
        const notice = el('workbenchMilestone'); if (!notice) return false;
        notice.querySelector('span').textContent = String(item.text).slice(0,240); notice.hidden = false;
        clearTimeout(noticeTimer); noticeTimer = setTimeout(dismissCelebration,6500); return true;
    }
    function milestones() {
        const now = {tasks:new Set((data().tasks || []).filter(item => item.done).map(item => item.id)), attempts:new Set((data().reviewAttempts || []).map(item => item.id))};
        if (baseline) {
            if ([...now.attempts].some(id => !baseline.attempts.has(id))) celebrate({text:'又完成了一次复习，感谢自己的认真。'});
            else if ([...now.tasks].some(id => !baseline.tasks.has(id))) celebrate({text:'完成了一件事，也可以安心歇一会儿。'});
        }
        // Never subtract growth or use missed days as a trigger.
        baseline = now;
    }
    function keyboard() {
        cancelAnimationFrame(keyboardFrame);
        keyboardFrame = requestAnimationFrame(() => {
            const viewport = window.visualViewport;
            document.documentElement.style.setProperty('--wx-viewport-height',(viewport?.height || innerHeight) + 'px');
            const field = document.activeElement;
            const open = viewport && viewport.scale === 1 && innerHeight - viewport.height > 140 && field?.matches('input,textarea,[contenteditable=true]');
            document.body.classList.toggle('wx-keyboard',Boolean(open));
            if (open) field.scrollIntoView({block:'nearest',behavior:'instant'});
        });
    }
    function refresh() {
        if (refreshQueued || !initialized) return; refreshQueued = true;
        queueMicrotask(() => {refreshQueued = false;markNavigation();compactHome();milestones();});
    }
    function init() {
        if (initialized || !el('mainContent')) return; initialized = true;
        current = document.body.dataset.view || 'overview'; document.body.classList.add('wx-experience');
        mountPrimary(); setupPreferences(); setupContext(); setupDetailActions(); setupSummary();
        const notice = document.createElement('aside'); notice.id = 'workbenchMilestone'; notice.hidden = true; notice.setAttribute('role','status'); notice.setAttribute('aria-live','polite');
        notice.innerHTML = '<i class="fas fa-check" aria-hidden="true"></i><span></span>';
        notice.append(button('关闭庆祝','xmark',dismissCelebration,true)); document.body.append(notice);
        const revision = preferenceRevision;
        if (window.DeviceStore) DeviceStore.get(prefKey).then(saved => {
            if (saved && revision === preferenceRevision) {prefs = normalize(saved);applyPreferences();syncPreferenceForm();}
        }).catch(() => {});
        window.visualViewport?.addEventListener('resize',keyboard); addEventListener('resize',() => {keyboard();markNavigation();});
        addEventListener('workspace-ready',refresh); addEventListener('workspace-change',refresh); addEventListener('yantu-sync',refresh);
        document.addEventListener('change',() => setTimeout(refresh,0));
        document.addEventListener('click',event => {if (event.target.closest?.('#toggleSidebar,#sidebarOverlay')) queueMicrotask(markNavigation);});
        keyboard(); refresh();
        dispatchEvent(new CustomEvent('workbench-ready'));
    }
    window.WorkbenchExperience = {
        version:1,init,navigate,settings,summary,capture,restore,updatePreferences,preferences:() => copy(prefs),celebrate,dismissCelebration,
        registerContextAdapter:(id,adapter) => {adapters.set(id,adapter);return () => adapters.delete(id);},
        integration:Object.freeze({draftKeys:[draftKey],draftPattern:'^workbench-draft-[A-Za-z0-9_.-]{1,150}$',deviceSettingKeys:[prefKey],localStorageKeys:[cacheKey],sharedCollections:[],sharedSettings:[]}),
        settled:() => Promise.all([prefTail,draftTail])
    };
    if (window.PrepLocal?.ready) {const previous = PrepLocal.ready;PrepLocal.ready = function(...args) {const result = previous.apply(this,args);init();return result;};}
    if (document.body.classList.contains('app-ready')) init();
})();
