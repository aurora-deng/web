(() => {
    const el = id => document.getElementById(id);
    const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
    const rooms = {profile: '个人信息', goals: '备考目标', preferences: '学习偏好', learning: '学习情况'};
    let config, conversation = YantuEnvironment.storage.getItem('yantu-assistant-conversation') || '', messages = [], job = null, pollTimer;
    let memories = [], memoryRoom = 'all', sending = false, refreshing = false, composeError = '';
    let providerChosen = false;
    const statusNames = {queued: '等待模型', running: '正在思考与查阅', cancelled: '已停止', failed: '回复未完成', done: ''};
    function deviceMode() { return el('assistantProvider').value === 'api-device'; }
    async function api(path, body, timeoutMs = 15000) {
        const cacheable = /\/(conversations|messages|memories|memory|forget|delete-conversation)(\?|$)/.test(path);
        if (deviceMode() && cacheable) return DeviceAI.route(path, body);
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), timeoutMs);
        try {
            const response = await PrepTransport.fetch(path, {credentials: 'same-origin', cache: 'no-store', signal: controller.signal, ...(body === undefined ? {} : {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)})});
            const result = await response.json().catch(() => ({error: response.status >= 500 ? '电脑主机已下线或远程连接中断，消息草稿已保留' : '连接返回了无效内容，请重新连接电脑'}));
            if (!response.ok) throw new Error(result.error || `连接失败 (${response.status})`);
            return result;
        } catch (error) {
            if (error.name === 'AbortError' || error instanceof TypeError) {
                if (cacheable) return DeviceAI.route(path, body);
                throw new Error('电脑主机已下线或连接中断，消息草稿已保留');
            }
            throw error;
        } finally { clearTimeout(timeout); }
    }
    function open() { PrepPush.openView('assistant'); }
    function connectionText(text) {
        el('assistantConnection').textContent = text;
        el('petConnection').textContent = text;
    }
    async function status() {
        const device = await DeviceAI.config();
        const selected = YantuEnvironment.storage.getItem('yantu-assistant-provider');
        if (!providerChosen && selected === 'api-device') { el('assistantProvider').value = selected; providerChosen = true; }
        if (deviceMode()) connectionText(`${device.hasKey && device.cloudConsent ? '本机 API · ' + device.apiModel : '本机 API 尚未配置'}${device.pending ? ' · 待同步 ' + device.pending + ' 项' : ''}`);
        try {
            config = await api('/api/assistant/status');
            if (!providerChosen) { el('assistantProvider').value = config.provider === 'api' && config.apiReady ? 'api' : 'ollama'; providerChosen = true; }
            const provider = el('assistantProvider').value;
            const text = provider === 'api' ? (config.apiReady ? 'API 已配置 · 电脑转发' : 'API 尚未配置') : config.ollama.online ? `电脑在线 · ${config.ollamaModel || '未选择模型'}` : '电脑在线 · Ollama 已下线';
            if (!deviceMode()) connectionText(text);
            el('assistantProvider').querySelector('[value=api]').disabled = !config.apiReady;
            el('ollamaModels').innerHTML = config.ollama.models.map(model => `<option value="${escape(model)}"></option>`).join('');
        } catch (error) { if (!deviceMode()) connectionText(error.message.includes('配对') || error.message.includes('连接此电脑') ? '请重新配对电脑' : '电脑主机已下线'); }
    }
    async function conversationList() {
        const result = await api('/api/assistant/conversations');
        el('assistantConversations').innerHTML = result.conversations.map(item => `<div class="conversation-item ${item.id === conversation ? 'active' : ''}"><button onclick="StudyAI.select('${item.id}')" title="${escape(item.title)}">${escape(item.title)}</button><button class="conversation-delete" onclick="StudyAI.removeConversation('${item.id}')" title="删除对话" aria-label="删除对话"><i class="fas fa-trash"></i></button></div>`).join('');
    }
    function setConversation(id) {
        conversation = id;
        if (id) YantuEnvironment.storage.setItem('yantu-assistant-conversation', id);
        else YantuEnvironment.storage.removeItem('yantu-assistant-conversation');
        el('assistantInput').value = YantuEnvironment.storage.getItem('yantu-chat-draft-' + (id || 'new')) || '';
    }
    async function select(id) {
        clearTimeout(pollTimer);
        setConversation(id); job = null; composeError = '';
        await loadMessages();
        await conversationList();
    }
    function newChat() { clearTimeout(pollTimer); setConversation(''); messages = []; job = null; composeError = ''; renderChat(); conversationList().catch(() => {}); }
    async function removeConversation(id) {
        if (!confirm('删除这段对话？已确认的长期记忆会保留。')) return;
        try { await api('/api/assistant/delete-conversation', {id}); if (conversation === id) newChat(); await conversationList(); }
        catch (error) { PrepLocal.toast(error.message); }
    }
    function bubble(role, content, extra = '') {
        return `<article class="assistant-message ${role}"><div class="message-label"><strong>${role === 'user' ? '我' : '小研'}</strong>${extra}</div><div class="message-content">${escape(content)}</div></article>`;
    }
    function renderChat(scroll = false) {
        const transcript = el('assistantTranscript');
        const bottom = transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 100;
        transcript.innerHTML = messages.map(message => bubble(message.role, message.content, `<button class="message-copy" onclick="StudyAI.copyMessage('${message.id}')" title="复制" aria-label="复制"><i class="fas fa-copy"></i></button>`)).join('');
        if (job && !messages.some(message => message.jobId === job.id || (job.status === 'done' && message.role === 'assistant' && message.content === job.content))) transcript.insertAdjacentHTML('beforeend', bubble('assistant', job.content || job.error || (job.status === 'running' ? '正在查阅资料…' : statusNames[job.status]), job.status === 'failed' ? '<button class="btn btn-ghost btn-sm" onclick="StudyAI.retry()">重试</button>' : ''));
        if (!messages.length && !job) transcript.innerHTML = '<div class="assistant-welcome"><img src="icons/study-pet.png" alt="小研" width="110" height="126" /><h4>今天想聊些什么？</h4></div>';
        const busy = sending || ['queued', 'running'].includes(job?.status);
        el('assistantSend').disabled = busy;
        el('assistantStop').hidden = !['queued', 'running'].includes(job?.status);
        el('assistantJobStatus').textContent = composeError || statusNames[job?.status] || '';
        el('petCompanion').classList.toggle('is-thinking', busy);
        if (scroll || bottom) transcript.scrollTop = transcript.scrollHeight;
        el('assistantSources').innerHTML = (job?.sources || []).filter((source, index, list) => list.findIndex(other => (other.id || other.fileId) === (source.id || source.fileId)&&other.page===source.page) === index).map((source, index) => `<button class="btn btn-ghost btn-sm" title="${escape(source.excerpt||source.title)}" onclick="StudyAI.source(${index})"><i class="fas fa-bookmark"></i> ${escape(source.title)}${source.page?' · 第 '+source.page+' 页':''}${source.method==='windows-ocr'?' · OCR 待校对':''}</button>`).join('');
    }
    async function loadMessages() {
        if (!conversation) { renderChat(); return; }
        try {
            const result = await api('/api/assistant/messages?conversation=' + encodeURIComponent(conversation));
            messages = result.messages;
            job = result.jobs.find(item => ['queued', 'running'].includes(item.status)) || result.lastJob || null;
            renderChat();
            if (['queued', 'running'].includes(job?.status)) schedulePoll();
        } catch (error) { connectionText('电脑主机已下线'); el('assistantJobStatus').textContent = error.message; }
    }
    function schedulePoll() { clearTimeout(pollTimer); pollTimer = setTimeout(poll, 700); }
    async function poll() {
        if (!job || !['queued', 'running'].includes(job.status)) return;
        const id = job.id, cid = conversation;
        try {
            const result = await api('/api/assistant/job?id=' + encodeURIComponent(id));
            if (conversation !== cid || job?.id !== id) return;
            job = result; renderChat();
            if (['queued', 'running'].includes(job.status)) schedulePoll();
            else {
                if (job.status === 'done') {
                    messages.push({id: 'reply-' + job.id, role: 'assistant', content: job.content, jobId: job.id});
                    renderChat(true);
                }
                conversationList().catch(() => {});
            }
        } catch (error) {
            connectionText('电脑主机已下线'); el('assistantJobStatus').textContent = error.message;
            pollTimer = setTimeout(poll, 4000);
        }
    }
    async function send(textOverride) {
        const input = el('assistantInput');
        const text = (typeof textOverride === 'string' ? textOverride : input.value).trim();
        if (!text || sending || ['queued', 'running'].includes(job?.status)) return;
        sending = true; composeError = ''; renderChat();
        let pending;
        try { pending = JSON.parse(YantuEnvironment.storage.getItem('yantu-assistant-submission') || 'null'); } catch (_) { pending = null; }
        const provider = el('assistantProvider').value;
        const nonce = pending?.text === text && pending?.conversation === conversation && pending?.provider === provider ? pending.nonce : uid();
        try {
            if (provider === 'api-device') {
                YantuEnvironment.storage.removeItem('yantu-chat-draft-' + (conversation || 'new'));
                const result = await DeviceAI.send({text, conversation, nonce}, update => {
                    if (!conversation) setConversation(update.conversation);
                    if (conversation !== update.conversation) return;
                    job = update; input.value = '';
                    DeviceAI.route('/api/assistant/messages?conversation=' + encodeURIComponent(conversation)).then(snapshot => { messages = snapshot.messages; renderChat(true); });
                });
                if (conversation === result.conversation) { job = result; await loadMessages(); }
                await conversationList();
                return;
            }
            await PrepLocal.flush();
            if (PrepLocal.hasPending()) throw new Error('学习资料还未同步，请等待电脑连接恢复后再发送');
            YantuEnvironment.storage.setItem('yantu-assistant-submission', JSON.stringify({text, provider, conversation, nonce}));
            const result = await api('/api/assistant/chat', {text, provider, conversation, nonce});
            YantuEnvironment.storage.removeItem('yantu-assistant-submission');
            YantuEnvironment.storage.removeItem('yantu-chat-draft-' + (conversation || 'new'));
            setConversation(result.conversation);
            input.value = '';
            messages.push({id: nonce, role: 'user', content: text});
            job = result;
            renderChat(true); schedulePoll(); await conversationList();
        } catch (error) {
            input.value = text;
            YantuEnvironment.storage.setItem('yantu-chat-draft-' + (conversation || 'new'), text);
            el('assistantJobStatus').textContent = error.message;
            composeError = error.message;
            if (error.message.includes('已下线') || error.message.includes('连接中断')) connectionText(error.message);
            PrepLocal.toast(error.message);
        } finally { sending = false; renderChat(); }
    }
    async function stop() {
        if (!job) return;
        if (job.provider === 'api-device') { DeviceAI.stop(); return; }
        try { job = await api('/api/assistant/cancel', {id: job.id}); clearTimeout(pollTimer); renderChat(); }
        catch (error) { PrepLocal.toast(error.message); }
    }
    function retry() { const text = [...messages].reverse().find(message => message.role === 'user')?.content; if (text) { job = null; send(text); } }
    async function copyMessage(id) {
        const content = messages.find(message => message.id === id)?.content;
        if (!content) return;
        try { await navigator.clipboard.writeText(content); PrepLocal.toast('已复制'); }
        catch (_) { PrepLocal.toast('当前浏览器不允许复制'); }
    }
    function source(index) {
        const unique = (job?.sources || []).filter((source, i, list) => list.findIndex(other => (other.id || other.fileId) === (source.id || source.fileId)&&other.page===source.page) === i);
        const reference = unique[index];
        if (!reference) return;
        const record = reference.id ? (state[reference.collection] || []).find(item => item.id === reference.id) : ['notes','mistakes','resources','mindmaps','papers','moodNotes'].flatMap(col => state[col] || []).find(item => item.fileId === reference.fileId || item.files?.some(file => file.id === reference.fileId));
        if(reference.page&&record&&window.Workspace?.ready()&&window.ReaderWorkspace){const index=StudyLibrary.files(record).findIndex(file=>file.id===reference.fileId);if(index>=0){ReaderWorkspace.open(record.id,index,reference.page);return;}}
        if (record?.files?.length || record?.fileId) { openViewer(record.id); return; }
        if (reference.collection === 'moodNotes') { MoodWall.create(reference.id); return; }
        const views = {tasks: 'overview', timelines: 'timeline', timelineItems: 'timeline', notes: 'resources', resources: 'resources', calendarEvents: 'exams'};
        PrepPush.openView(views[reference.collection] || reference.collection || 'resources');
    }
    async function settings() {
        await status();
        if (YantuEnvironment.hosted || deviceMode() || !config?.local) { await deviceSettings(); return; }
        const form = el('assistantSettingsForm');
        for (const key of ['provider', 'ollamaUrl', 'ollamaModel', 'contextTokens', 'apiUrl', 'apiModel']) form.elements[key].value = config[key] || '';
        form.elements.apiKey.value = '';
        form.elements.apiKey.placeholder = config.hasKey ? '已安全保存，留空不更换' : '填写密钥';
        form.elements.clearKey.checked = false;
        form.elements.cloudConsent.checked = config.cloudConsent;
        el('assistantSettings').showModal();
    }
    async function deviceSettings() {
        const settings = await DeviceAI.config(), form = el('deviceSettingsForm');
        form.elements.apiUrl.value = settings.apiUrl;
        form.elements.apiModel.value = settings.apiModel;
        form.elements.apiKey.value = '';
        form.elements.apiKey.placeholder = settings.hasKey ? '已保存在本机，留空不更换' : '填写本机密钥';
        form.elements.clearKey.checked = false;
        form.elements.cloudConsent.checked = settings.cloudConsent;
        form.elements.pcUrl.value = PrepTransport.base() || location.origin;
        form.elements.pcCode.value = '';
        for (const name of ['pcUrl', 'pcCode']) {
            if (YantuEnvironment.hosted) form.elements[name].closest('label')?.setAttribute('hidden', '');
        }
        if (YantuEnvironment.hosted) el('deviceConnectPC').hidden = true;
        el('deviceSettings').showModal();
    }
    async function saveDeviceSettings(event) {
        event.preventDefault();
        const form = event.target, button = form.querySelector('[type=submit]');
        button.disabled = true;
        try {
            const body = Object.fromEntries(new FormData(form));
            body.cloudConsent = form.elements.cloudConsent.checked;
            body.clearKey = form.elements.clearKey.checked;
            await DeviceAI.configure(body);
            form.elements.apiKey.value = '';
            el('assistantProvider').value = 'api-device'; providerChosen = true;
            YantuEnvironment.storage.setItem('yantu-assistant-provider', 'api-device');
            el('deviceSettings').close(); await refresh();
            PrepLocal.toast('本机 API 已保存，电脑离线时也可使用');
        } catch (error) { PrepLocal.toast(error.message); }
        finally { button.disabled = false; }
    }
    async function connectPC() {
        const form = el('deviceSettingsForm');
        try {
            await PrepTransport.connect(form.elements.pcUrl.value, form.elements.pcCode.value);
            await PrepLocal.pull(); await DeviceAI.sync();
            PrepLocal.toast('已连接电脑并同步对话');
        } catch (error) { PrepLocal.toast(error.name === 'AbortError' || error instanceof TypeError ? '电脑连接失败，请检查网址和配对码' : error.message); }
    }
    async function saveSettings(event) {
        event.preventDefault();
        const form = event.target, button = form.querySelector('[type=submit]');
        button.disabled = true;
        try {
            const body = Object.fromEntries(new FormData(form));
            body.cloudConsent = form.elements.cloudConsent.checked;
            body.clearKey = form.elements.clearKey.checked;
            config = await api('/api/assistant/settings', body);
            el('assistantProvider').value = config.provider;
            form.elements.apiKey.value = '';
            el('assistantSettings').close(); await status(); PrepLocal.toast('接口已保存');
        } catch (error) { PrepLocal.toast(error.message); }
        finally { button.disabled = false; }
    }
    async function showMemories() {
        try {
            const result = await api('/api/assistant/memories');
            memories = result.memories;
            renderMemories();
            if (!el('memoryPalace').open) el('memoryPalace').showModal();
        } catch (error) { PrepLocal.toast(error.message); }
    }
    function renderMemories() {
        el('memoryRooms').innerHTML = ['all', ...Object.keys(rooms)].map(room => `<button class="${memoryRoom === room ? 'active' : ''}" onclick="StudyAI.memoryRoom('${room}')">${rooms[room] || '全部'}<span>${memories.filter(memory => room === 'all' || memory.room === room).length}</span></button>`).join('');
        el('memoryList').innerHTML = memories.filter(memory => memoryRoom === 'all' || memory.room === memoryRoom).map(memory => `<article class="memory-item"><div><span class="memory-room-name">${rooms[memory.room]} · ${memory.confirmed ? '已确认' : '待确认'}</span><p>${escape(memory.text)}</p><time>${new Date(memory.updated * 1000).toLocaleString('zh-CN')}</time></div><div class="memory-actions">${!memory.confirmed ? `<button class="btn btn-icon btn-ghost" onclick="StudyAI.confirmMemory('${memory.id}')" title="确认记忆" aria-label="确认记忆"><i class="fas fa-check"></i></button>` : ''}<button class="btn btn-icon btn-ghost" onclick="StudyAI.editMemory('${memory.id}')" title="修改记忆" aria-label="修改记忆"><i class="fas fa-pen"></i></button><button class="btn btn-icon btn-danger" onclick="StudyAI.forget('${memory.id}')" title="忘记" aria-label="忘记"><i class="fas fa-trash"></i></button></div></article>`).join('') || '<div class="empty-state">这个房间还没有记忆</div>';
    }
    function changeMemoryRoom(room) { memoryRoom = room; renderMemories(); }
    function clearMemoryForm() { el('memoryForm').reset(); el('memoryForm').elements.id.value = ''; }
    function editMemory(id) { const memory = memories.find(item => item.id === id); if (!memory) return; const form = el('memoryForm'); for (const key of ['id', 'room', 'text']) form.elements[key].value = memory[key]; form.elements.text.focus(); }
    async function confirmMemory(id) { const memory = memories.find(item => item.id === id); if (!memory) return; try { await api('/api/assistant/memory', {...memory, confirmed: true}); await showMemories(); } catch (error) { PrepLocal.toast(error.message); } }
    async function forget(id) { if (!confirm('让小研忘记这条长期记忆？')) return; try { await api('/api/assistant/forget', {id}); await showMemories(); } catch (error) { PrepLocal.toast(error.message); } }
    async function saveMemory(event) { event.preventDefault(); try { await api('/api/assistant/memory', {...Object.fromEntries(new FormData(event.target)), confirmed: true}); clearMemoryForm(); await showMemories(); } catch (error) { PrepLocal.toast(error.message); } }
    async function refresh() {
        if (refreshing) return;
        refreshing = true;
        try { await DeviceAI.sync().catch(() => {}); await status(); await conversationList(); if (!['queued', 'running'].includes(job?.status)) await loadMessages(); }
        catch (_) { if (!deviceMode()) connectionText('电脑主机已下线'); }
        finally { refreshing = false; }
    }
    function suggest(question) { open(); el('assistantInput').value = question; el('assistantInput').focus(); }
    const previousRender = renderView;
    function sizePhoneChat() {
        if (innerWidth > 768) return;
        const header = document.querySelector('.workspace-header');
        const padding = parseFloat(getComputedStyle(el('mainContent')).paddingTop) || 0;
        const margin = parseFloat(getComputedStyle(header).marginBottom) || 0;
        const tabs = document.querySelector('.mobile-tabs').getBoundingClientRect().height;
        const height = (window.visualViewport?.height || innerHeight) - header.getBoundingClientRect().height - padding - margin - tabs - 20;
        el('view-assistant').style.setProperty('--phone-chat-height', `${Math.max(340, height)}px`);
    }
    renderView = view => { previousRender(view); if (view === 'assistant') { el('workspaceTitle').textContent = '小研助手'; requestAnimationFrame(sizePhoneChat); refresh(); } };
    const previousReady = PrepLocal.ready;
    PrepLocal.ready = () => {
        previousReady();
        const perch = document.createElement('section');
        perch.className = 'assistant-perch';
        perch.innerHTML = '<button id="petCompanion" class="pet-companion" onclick="StudyAI.open()" title="和小研聊聊" aria-label="和小研聊聊"><img src="icons/study-pet.png" alt="小研桌宠" draggable="false" /></button><div class="pet-summary"><h3>小研在这里</h3><p id="petConnection">正在连接电脑</p><div class="pet-actions"><button class="btn btn-ghost btn-sm" onclick="StudyAI.suggest(\'结合我的任务、成绩和复盘，分析今天该优先做什么。\')"><i class="fas fa-list-check"></i> 分析今天</button><button class="btn btn-ghost btn-sm" onclick="StudyAI.suggest(\'根据我的学习记录，帮我复习一个最近薄弱的知识点。\')"><i class="fas fa-book-open"></i> 一起复习</button><button class="btn btn-ghost btn-sm" onclick="StudyAI.suggest(\'我想聊聊最近的学习心情，请先看看我的心情墙。\')"><i class="fas fa-comment"></i> 聊聊心情</button></div></div>';
        document.querySelector('.overview-columns > .card:first-child').append(perch);
        el('assistantComposer').onsubmit = event => { event.preventDefault(); send(); };
        el('assistantInput').oninput = () => YantuEnvironment.storage.setItem('yantu-chat-draft-' + (conversation || 'new'), el('assistantInput').value);
        el('assistantInput').onkeydown = event => { if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) { event.preventDefault(); send(); } };
        el('assistantSettingsForm').onsubmit = saveSettings;
        el('deviceSettingsForm').onsubmit = saveDeviceSettings;
        el('memoryForm').onsubmit = saveMemory;
        el('assistantProvider').onchange = () => { providerChosen = true; YantuEnvironment.storage.setItem('yantu-assistant-provider', el('assistantProvider').value); refresh(); };
        setConversation(conversation); DeviceAI.sync().catch(() => {}); status();
        addEventListener('resize', sizePhoneChat);
        window.visualViewport?.addEventListener('resize', sizePhoneChat);
        new ResizeObserver(sizePhoneChat).observe(document.querySelector('.workspace-header'));
        setInterval(() => { if (!document.hidden && el('view-assistant').classList.contains('active')) refresh(); }, 7000);
        document.addEventListener('visibilitychange', () => { if (!document.hidden && el('view-assistant').classList.contains('active')) refresh(); });
    };
    async function clearStudyData() {
        await PrepLocal.flush();
        if (PrepLocal.hasPending()) throw new Error('电脑已下线，无法确认删除，资料保持不变');
        const snapshot = await api('/api/reset-study', {confirmation: 'DELETE_ALL_STUDY_DATA'}, 60000);
        await DeviceAI.clear();
        PrepLocal.accept(snapshot);
        for (const key of YantuEnvironment.keys()) if (key.startsWith('yantu-chat-draft-')) YantuEnvironment.storage.removeItem(key);
        newChat();
        YantuEnvironment.storage.removeItem('yantu-assistant-submission');
        PrepLocal.toast('学习资料、心情墙、对话与记忆已清空');
    }
    const previousConfirmDelete = confirmDelete;
    confirmDelete = async () => {
        if (pendingDeleteAction !== 'deleteAllData') return previousConfirmDelete();
        try { await clearStudyData(); closeDeleteConfirm(); updatePrepControls(); }
        catch (error) { PrepLocal.toast(error.message); }
    };
    const previousExit = exitPrep;
    exitPrep = async clear => {
        if (!clear) return previousExit(false);
        try { await clearStudyData(); el('exitConfirmDialog').close(); updatePrepControls(); }
        catch (error) { PrepLocal.toast(error.message); }
    };
    window.StudyAI = {open, select, newChat, removeConversation, send, stop, retry, copyMessage, source, settings, deviceSettings, connectPC, showMemories, memoryRoom: changeMemoryRoom, clearMemoryForm, editMemory, confirmMemory, forget, suggest, refresh};
})();
