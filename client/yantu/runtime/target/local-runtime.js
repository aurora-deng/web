(() => {
    const collections = new Set(['tasks', 'calendarEvents', 'timelines', 'timelineItems', 'mistakes', 'notes', 'resources', 'mindmaps', 'papers', 'scores', 'reviews', 'schools', 'exams', 'moodNotes', 'moodWalls', 'focusSessions', 'reviewAttempts']);
    for(const collection of window.YantuWorkspaceSchema?.collections||[])collections.add(collection);
    const fileCollections = ['mistakes', 'notes', 'resources', 'mindmaps', 'papers', 'moodNotes'];
    fileCollections.push(...(window.YantuWorkspaceSchema?.collections||[]));
    const rescueSettings = new Set(['prepStatus','prepStart','examYear','targetYear','examDate','studySchedule','focusTimer','reminderSettings','lastTaskRefresh','subjectProfiles','planCapacities','learningPrefs','cardPreferences']);
    const localMode = /^https?:$/.test(location.protocol);
    const pendingKey = `${LOCAL_KEY}-pending-v2`;
    const baselineKey = `${LOCAL_KEY}-baseline-v2`;
    const migrationKey = `${LOCAL_KEY}-files-migrated-v2`;
    let hadLocalState = Boolean(YantuEnvironment.storage.getItem(LOCAL_KEY));
    const clientKey = 'yantu-device-id';
    const clone = value => structuredClone(value);
    const same = (a, b) => a === b || (a !== null && b !== null && typeof a === 'object' && typeof b === 'object' && JSON.stringify(a) === JSON.stringify(b));
    const el = id => document.getElementById(id);
    const html = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
    const originalSaveFile = saveFile;
    const originalGetFile = getFile;
    const originalDeleteFile = deleteFile;
    const originalOpenRecord = openRecordDialog;
    const originalOpenScore = openScoreDialog;
    const uploadDrafts = new WeakMap();
    const originalRenderView = renderView;
    const originalSaveState = saveState;
    let observed = clone(state);
    let pending = [];
    let revision = 0;
    let initialized = false;
    let online = false;
    let flushing = null;
    let pulling = null;
    let flushTimeout;
    let eventSource;
    let lastSync = '';
    let lastContact = null;
    let lastError = '';
    let conflictCount = 0;
    let clockAnchor = null;
    let clockRequest = null;
    let systemOffsetMinutes = -new Date().getTimezoneOffset();
    let mediaURL = '';
    let mediaRequest = 0;
    let toastTimeout;
    let retryDelay = 2000;
    let applying = false;
    let rescuing = false;
    let clientId;
    let cacheDirty = false, cacheWriting = null, lastTimerRender = '';
    const dateFormatter = new Intl.DateTimeFormat('sv-SE', {timeZone:'UTC'});
    const timeFormatter = new Intl.DateTimeFormat('zh-CN', {timeZone:'UTC',hour:'2-digit',minute:'2-digit',second:'2-digit',hour12:false});
    const dayFormatter = new Intl.DateTimeFormat('zh-CN', {timeZone:'UTC',month:'long',day:'numeric',weekday:'long'});
    const uploadQueue = new Map();
    const uploadQueueReady = DeviceStore.entries('upload-').then(entries => entries.forEach(([, item]) => uploadQueue.set(item.id, item)));
    try {
        pending = JSON.parse(YantuEnvironment.storage.getItem(pendingKey) || '[]');
        clientId = YantuEnvironment.storage.getItem(clientKey) || uid();
        YantuEnvironment.storage.setItem(clientKey, clientId);
    } catch (_) { clientId = uid(); }

    function toast(message) {
        const box = el('toast');
        box.textContent = message;
        box.classList.add('visible');
        clearTimeout(toastTimeout);
        toastTimeout = setTimeout(() => box.classList.remove('visible'), 4500);
    }

    function nowMs() {
        return clockAnchor ? clockAnchor.time + performance.now() - clockAnchor.performance : Date.now();
    }

    function nowDate() { return new Date(nowMs()); }

    function systemDate(date = nowDate()) {
        return new Date(date.getTime() + systemOffsetMinutes * 60000);
    }

    function dateText(date) {
        return dateFormatter.format(systemDate(date));
    }

    function timeText() {
        return timeFormatter.format(systemDate());
    }

    function renderClock() {
        el('systemClock').textContent = timeText();
        el('workspaceDate').textContent = dayFormatter.format(systemDate());
        el('clockSource').textContent = clockAnchor ? '电脑系统时间' : '本机系统时间';
        el('connectionTime').textContent = `${dateText(nowDate())} ${timeText()}${clockAnchor ? ' · 已校准电脑系统时间' : ' · 等待电脑校准'}`;
    }

    async function syncClock() {
        if (!localMode || clockRequest) return clockRequest;
        clockRequest = (async () => {
            const samples = await Promise.allSettled(Array.from({length: 3}, async () => {
                const start = performance.now();
                const sample = await jsonRequest('/api/time');
                const finish = performance.now();
                return {...sample, elapsed: finish - start, performance: finish};
            }));
            const valid = samples.filter(sample => sample.status === 'fulfilled').map(sample => sample.value).filter(sample => Number.isFinite(sample.time));
            if (!valid.length) return;
            const best = valid.sort((a, b) => a.elapsed - b.elapsed)[0];
            clockAnchor = {time: best.time + best.elapsed / 2, performance: best.performance};
            systemOffsetMinutes = best.utcOffsetMinutes;
            renderClock();
        })();
        try { await clockRequest; } finally { clockRequest = null; }
    }

    async function request(path, options = {}) {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), options.long ? 300000 : 12000);
        try {
            const response = await PrepTransport.fetch(path, {credentials: 'same-origin', ...options, signal: controller.signal});
            if (!response.ok) {
                const error = await response.json().catch(() => ({}));
                const failure = new Error(error.error || `请求失败 (${response.status})`);
                failure.status = response.status;
                throw failure;
            }
            return response;
        } finally { clearTimeout(timeout); }
    }

    async function jsonRequest(path, body, options = {}) {
        const response = await request(path, {...options, ...(body === undefined ? {} : {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)})});
        return response.json();
    }

    function status(text, connected = online) {
        if (connected && !pending.length && !uploadQueue.size && conflictCount) text = `${conflictCount} 项冲突待选择`;
        el('syncText').textContent = text;
        el('sidebarSyncText').textContent = text;
        el('connectionState').textContent = text;
        document.body.dataset.connection = connected ? 'online' : 'offline';
        el('connectionLastSync').textContent = lastSync || '—';
        window.dispatchEvent(new CustomEvent('yantu-sync', {detail: diagnostics()}));
    }
    function diagnostics() { return {online, initialized, pending:pending.length+uploadQueue.size, pendingRecords:pending.length, pendingFiles:uploadQueue.size, conflicts:conflictCount, lastSync, lastContact, lastError}; }
    function dayStart(day) { return Date.parse(day+'T00:00:00Z')-systemOffsetMinutes*60000; }

    function diff(before, after) {
        const operations = [];
        for (const key of new Set([...Object.keys(before), ...Object.keys(after)])) {
            if (key === 'deviceId' || key === 'resourceRetirement') continue;
            if (collections.has(key)) {
                const oldRecords = new Map((before[key] || []).filter(r => r?.id).map(r => [r.id, r]));
                const newRecords = new Map((after[key] || []).filter(r => r?.id).map(r => [r.id, r]));
                for (const [id, old] of oldRecords) {
                    if (!newRecords.has(id)) operations.push({kind: 'delete', collection: key, id, base:clone(old)});
                }
                for (const [id, record] of newRecords) {
                    const old = oldRecords.get(id) || {};
                    const fields = {};
                    const base = {};
                    const unset = [];
                    for (const field of new Set([...Object.keys(old), ...Object.keys(record)])) {
                        if (field === 'id') continue;
                        if (!(field in record)) { unset.push(field); base[field] = {exists:field in old,value:old[field]??null}; }
                        else if (!same(record[field], old[field])) { fields[field] = record[field]; base[field] = {exists:field in old,value:old[field]??null}; }
                    }
                    if (!oldRecords.has(id) || Object.keys(fields).length || unset.length) operations.push({kind: 'upsert', collection: key, id, fields, unset, base, baseExists:oldRecords.has(id), baseRecord:clone(old)});
                }
            } else if (!same(before[key], after[key])) {
                operations.push({kind: 'setting', key, value: after[key], remove: !(key in after), base:{exists:key in before,value:before[key]??null}});
            }
        }
        return operations.map(op => ({opId: uid(), ...clone(op)}));
    }

    function applyOperations(target, operations) {
        for (const op of operations) {
            if (op.kind === 'setting') {
                if (op.remove) delete target[op.key];
                else target[op.key] = clone(op.value);
                continue;
            }
            const records = target[op.collection] ||= [];
            const index = records.findIndex(r => r.id === op.id);
            if (op.kind === 'delete') {
                if (index >= 0) records.splice(index, 1);
            } else {
                const item = index >= 0 ? {...records[index]} : {id: op.id};
                for (const key of op.unset || []) delete item[key];
                Object.assign(item, clone(op.fields), {id: op.id});
                if (index < 0) records.push(item);
                else records[index] = item;
            }
        }
        return target;
    }

    function persistQueue() {
        cacheDirty = true;
        if (!cacheWriting) cacheWriting = Promise.resolve().then(async () => {
            // IndexedDB takes its own structured snapshot; do not duplicate the full library first.
            while (cacheDirty) {
                cacheDirty = false;
                await DeviceStore.put('workspace', {state, pending, observed, revision});
            }
        }).catch(() => toast('本机存储空间不足，请保持连接并下载完整备份')).finally(() => {cacheWriting = null;if(cacheDirty)persistQueue();});
        try {
            YantuEnvironment.storage.setItem(pendingKey, JSON.stringify(pending));
            const count = Object.values(observed).reduce((total,value)=>total+(Array.isArray(value)?value.length:0),0);
            const serialized = count <= 250 ? JSON.stringify(observed) : '';
            if (serialized && serialized.length <= 750000) {
                YantuEnvironment.storage.setItem(baselineKey, serialized);
                YantuEnvironment.storage.setItem(LOCAL_KEY, JSON.stringify(state));
            } else {
                YantuEnvironment.storage.removeItem(baselineKey);
                YantuEnvironment.storage.removeItem(LOCAL_KEY);
            }
        } catch (_) { /* The complete state and pending edits are in IndexedDB. */ }
    }

    saveState = () => { if (localMode && initialized) capture(); else originalSaveState(); };

    function capture() {
        if (!localMode || !initialized || applying || rescuing) return;
        if (['paused', 'exited'].includes(state.prepStatus) && observed.prepStatus !== state.prepStatus && state.focusTimer?.running) {
            recordFocusSegment(state.focusTimer);
            state.focusTimer.remaining = secondsLeft();
            state.focusTimer.running = false;
            state.focusTimer.endAt = null;
        }
        const operations = diff(observed, state);
        if (!operations.length) return;
        pending.push(...operations);
        applyOperations(observed, operations);
        try { persistQueue(); }
        catch (_) { toast('设备缓存已满，请保持连接并下载完整备份'); }
        status(`待同步 ${pending.length} 项`);
        clearTimeout(flushTimeout);
        flushTimeout = setTimeout(flush, 180);
    }

    function accept(snapshot) {
        const retirement = snapshot.state?.resourceRetirement;
        if (Array.isArray(retirement?.recordIds)) {
            const removed = new Set(retirement.recordIds);
            pending = pending.filter(op => !removed.has(op.id) && !(op.kind === 'setting' && op.key === 'resourceRetirement'));
            DeviceStore.purgeResources?.(retirement).catch(() => toast('旧资料的本机缓存暂未清理，请重新打开软件'));
            dispatchEvent(new CustomEvent('resources-retired', {detail: retirement}));
        }
        const defaults = {mindmaps: [], ...loadState()};
        for (const col of collections) defaults[col] = [];
        defaults.deviceId = clientId;
        state = applyOperations({...defaults, ...snapshot.state}, pending);
        observed = clone(state);
        revision = snapshot.revision;
        try { persistQueue(); } catch (_) { /* The larger IndexedDB cache is written first. */ }
        lastSync = timeText();
        lastContact = nowDate().toISOString();
        lastError = '';
        conflictCount = snapshot.conflictCount || 0;
        online = true;
        status(pending.length ? `待同步 ${pending.length} 项` : '已同步到电脑', true);
        refreshActive();
        capture();
    }

    async function flush() {
        if (!initialized || !localMode || rescuing) return;
        await uploadQueueReady;
        if (rescuing) return;
        if (flushing) return flushing;
        if (cacheWriting) await cacheWriting;
        if (flushing || rescuing) return flushing;
        if (!pending.length && !uploadQueue.size) return;
        flushing = (async () => {
            try {
                await flushFiles();
                capture();
                while (pending.length) {
                    const batch = pending.slice(0, 250);
                    const snapshot = await jsonRequest('/api/sync', {clientId, operations: batch});
                    const ids = new Set(batch.map(op => op.opId));
                    pending = pending.filter(op => !ids.has(op.opId));
                    accept(snapshot);
                }
                retryDelay = 2000;
            } catch (error) {
                online = false;
                lastError = error.message;
                status('等待连接，修改已保留', false);
                if (error.status === 400 || error.status === 500) toast(`同步未完成：${error.message}`);
                if (error.status === 401) await pair();
                clearTimeout(flushTimeout);
                flushTimeout = setTimeout(flush, retryDelay);
                retryDelay = Math.min(30000, retryDelay * 2);
            }
        })();
        try { await flushing; } finally { flushing = null;if(cacheWriting)await cacheWriting; }
    }

    async function pull() {
        if (!initialized || !localMode || pulling || rescuing) return pulling;
        if (flushing) await flushing;
        if (pulling || rescuing) return pulling;
        if (pending.length || uploadQueue.size) { await flush(); return; }
        pulling = (async () => {
            try {
                const snapshot = await jsonRequest(`/api/state?since=${revision}`);
                if (!snapshot.unchanged && snapshot.revision !== revision) accept(snapshot);
                else { online = true; lastContact=nowDate().toISOString(); lastSync=timeText(); lastError=''; conflictCount=snapshot.conflictCount||0; status('已同步到电脑', true); }
            } catch (error) { online = false; lastError=error.message; status('电脑连接已断开', false); }
        })();
        try { await pulling; } finally { pulling = null; }
    }

    async function pair() {
        if (YantuEnvironment.hosted) {
            await jsonRequest('/api/time');
            return;
        }
        if (!PrepTransport.base() && ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname)) {
            await jsonRequest('/api/pair', {});
            return;
        }
        try {
            await jsonRequest('/api/time');
            return;
        } catch (error) {
            if (error.status !== 401) throw error;
        }
        const dialog = el('pairDialog');
        if (!dialog.open) dialog.showModal();
        dialog.oncancel = event => event.preventDefault();
        await new Promise(resolve => {
            el('pairForm').onsubmit = async event => {
                event.preventDefault();
                const button = event.target.querySelector('button');
                button.disabled = true;
                try {
                    await jsonRequest('/api/pair', {code: el('pairCode').value});
                    dialog.close();
                    resolve();
                } catch (error) { el('pairError').textContent = error.message; }
                finally { button.disabled = false; }
            };
        });
    }

    function watchChanges() {
        eventSource?.close();
        if (YantuEnvironment.hosted || PrepTransport.base() || location.hostname.endsWith('.trycloudflare.com')) return;
        eventSource = new EventSource('/api/events');
        eventSource.onmessage = event => {
            if (Number(event.data) > revision) pull();
        };
        eventSource.onerror = () => { online = false; status('连接恢复中', false); };
        eventSource.onopen = () => { online = true; pending.length ? flush() : pull(); };
    }
    window.addEventListener('pc-host-change', () => { watchChanges(); syncClock(); });

    async function boot() {
        if (!localMode) { initialized = true; status('浏览器本地模式', false); return; }
        try {
            await uploadQueueReady;
            const cached = await DeviceStore.get('workspace').catch(() => null);
            if (cached) { state = cached.state; pending = cached.pending; observed = cached.observed; revision = cached.revision; hadLocalState = true; }
            await pair();
            await syncClock();
            const snapshot = await jsonRequest('/api/state');
            const localState = clone(state);
            const baseline = cached ? cached.observed : JSON.parse(YantuEnvironment.storage.getItem(baselineKey) || 'null');
            if (baseline) pending.push(...diff(baseline, localState));
            if (!Object.keys(snapshot.state).length) {
                const missing = applyOperations({}, pending);
                pending.push(...diff(missing, localState));
            } else if (!baseline && hadLocalState) {
                const merged = clone(snapshot.state);
                for (const col of collections) {
                    const ids = new Set((merged[col] || []).map(r => r.id));
                    for (const record of localState[col] || []) {
                        if (!ids.has(record.id)) (merged[col] ||= []).push(record);
                    }
                }
                pending.push(...diff(snapshot.state, merged));
            }
            initialized = true;
            accept(snapshot);
            await flush();
            watchChanges();
            if (hadLocalState && !YantuEnvironment.storage.getItem(migrationKey)) {
                migrateBrowserFiles(localState).then(() => YantuEnvironment.storage.setItem(migrationKey, 'done'))
                    .catch(error => toast(`旧附件迁移未完成：${error.message}`));
            }
        } catch (error) {
            initialized = true;
            online = false;
            status('离线，本机缓存已保留', false);
            toast(error.name === 'AbortError' || error instanceof TypeError ? '电脑已下线，本机资料与对话已保留' : error.message);
        }
    }

    async function migrateBrowserFiles(browserState) {
        if (!window.indexedDB) return;
        for (const col of fileCollections) {
            for (const record of browserState[col] || []) {
                for (const file of recordFiles(record)) {
                    const response = await PrepTransport.fetch(`/api/files/${encodeURIComponent(file.id)}`, {method: 'HEAD'});
                    if (response.ok) continue;
                    const blob = await originalGetFile(file.id).catch(() => null);
                    if (blob) await uploadFile(file.id, blob, file.name);
                }
            }
        }
    }

    async function uploadFile(id, blob, name = blob.name || '附件') {
        if (!localMode) return originalSaveFile(id, blob);
        if (blob.size > 256*1024*1024) throw new Error('单个附件上限 256 MB');
        await uploadQueueReady;
        const item={id,name,type:blob.type||(name.toLowerCase().endsWith('.pdf')?'application/pdf':'application/octet-stream'),size:blob.size,selectedAt:nowDate().toISOString()};
        await DeviceStore.batch([['file-'+id,blob],['upload-'+id,item]]);
        uploadQueue.set(id,item);
        if(!online || navigator.onLine===false)return {json:async()=>({...item,pendingUpload:true})};
        try {return await sendQueuedFile(item,blob);}
        catch(error){
            if(error.name==='AbortError'||error instanceof TypeError){online=false;lastError='附件已保留在本机，等待电脑连接';status(lastError,false);return {json:async()=>({...item,pendingUpload:true})};}
            throw error;
        }
    }

    async function sendQueuedFile(item,blob) {
        const {id,name,type}=item;
        const params = new URLSearchParams({name});
        const path = `/api/files/${encodeURIComponent(id)}?${params}`;
        const partSize = 8 * 1024 * 1024;
        let metadata;
        if (YantuEnvironment.hosted && blob.size > partSize) {
            for (let offset = 0; offset < blob.size; offset += partSize) {
                const end = Math.min(offset + partSize, blob.size);
                const response = await request(path, {method: 'PUT', long: true, body: blob.slice(offset, end),
                    headers: {'Content-Type': type, 'Content-Range': `bytes ${offset}-${end - 1}/${blob.size}`}});
                const progress = await response.json();
                if (progress.received !== end) throw new Error('附件分段确认异常，请重试');
                metadata = progress.file;
            }
            if (!metadata) throw new Error('附件尚未完成入库');
        } else {
            const response = await request(path, {method: 'PUT', headers: {'Content-Type': type}, body: blob, long: true});
            metadata = await response.json();
        }
        await DeviceStore.batch([['upload-'+id,undefined],['attachment-'+id,undefined],['file-meta-'+id,metadata]]);
        uploadQueue.delete(id);
        return {json:async()=>metadata};
    }

    async function flushFiles() {
        for(const item of [...uploadQueue.values()]){
            const blob=await DeviceStore.get('file-'+item.id);
            if(!blob)throw new Error('待上传附件的本机副本缺失，请从紧急导出或原文件恢复');
            const metadata=await(await sendQueuedFile(item,blob)).json();
            for(const col of fileCollections)for(const record of state[col]||[])for(const file of record.files||[])if(file.id===item.id){delete file.pendingUpload;file.uploadedAt=metadata.uploadedAt||nowDate().toISOString();if(metadata.sha256)file.sha256=metadata.sha256;}
        }
    }

    function rescueSnapshot() {
        capture();
        const saved=Object.fromEntries(Object.entries(state).filter(([key])=>collections.has(key)||rescueSettings.has(key)));
        const operations=pending.filter(op=>collections.has(op.collection)||(op.kind==='setting'&&rescueSettings.has(op.key)));
        return {format:'yantu-emergency-v1',createdAt:nowDate().toISOString(),revision,state:clone(saved),operations:clone(operations),uploads:clone([...uploadQueue.values()])};
    }

    function checkRescue(value) {
        const safeId=id=>typeof id==='string'&&/^[A-Za-z0-9_.-]{1,100}$/.test(id);
        const check=(item,depth=0)=>{if(depth>60)throw new Error('紧急包嵌套过深');if(item&&typeof item==='object')for(const [key,child]of Object.entries(item)){if(['__proto__','prototype','constructor'].includes(key))throw new Error('紧急包包含不安全字段');check(child,depth+1);}};
        check(value);
        if(value?.format!=='yantu-emergency-v1'||!Array.isArray(value.operations)||value.operations.length>50000||!value.state||typeof value.state!=='object'||Array.isArray(value.state))throw new Error('无效的紧急包');
        for(const [key,records]of Object.entries(value.state)){
            if(!collections.has(key)&&!rescueSettings.has(key))throw new Error('紧急包不能包含凭据或设备设置');
            if(collections.has(key)){
                if(!Array.isArray(records)||records.length>100000)throw new Error('紧急包记录列表无效');
                const ids=new Set();
                for(const record of records){if(!record||typeof record!=='object'||Array.isArray(record)||!safeId(record.id)||ids.has(record.id))throw new Error('紧急包记录编号无效或重复');ids.add(record.id);}
            }
        }
        if(!Array.isArray(value.uploads)||value.uploads.length>5000||value.uploads.some(item=>!item||!safeId(item.id)||!Number.isInteger(item.size)||item.size<0||item.size>256*1024*1024))throw new Error('紧急包上传队列无效');
        const operationIds=new Set();
        for(const op of value.operations){
            if(!safeId(op.opId)||!['upsert','delete','setting'].includes(op.kind))throw new Error('紧急包操作无效');
            if(operationIds.has(op.opId))throw new Error('紧急包操作编号重复');operationIds.add(op.opId);
            if(op.kind!=='setting'&&(!collections.has(op.collection)||!safeId(op.id)))throw new Error('紧急包记录类型无效');
            if(op.kind==='setting'&&(!rescueSettings.has(op.key)||(!op.remove&&!Object.hasOwn(value.state,op.key))))throw new Error('紧急包不能恢复凭据或设备设置');
            if(op.kind==='upsert'&&(!op.fields||typeof op.fields!=='object'||Array.isArray(op.fields)))throw new Error('紧急包记录字段无效');
        }
        return value;
    }

    async function restoreRescue(value,entries,options={}) {
        if(rescuing)throw new Error('紧急恢复正在进行，请稍后');
        if(state.focusTimer?.running)throw new Error('请先暂停专注计时，再恢复紧急包');
        if(flushing||pulling)await Promise.race([Promise.allSettled([flushing,pulling].filter(Boolean)),new Promise(resolve=>setTimeout(resolve,1000))]);
        if(flushing||pulling)throw new Error('当前同步请求尚未结束，请稍后重试；未改动本机内容');
        rescuing=true;try{return await commitRescue(value,entries,options);}finally{rescuing=false;capture();}
    }

    async function commitRescue(value,entries,options={}) {
        checkRescue(value);capture();await uploadQueueReady;
        if(cacheWriting)await cacheWriting;
        const next=clone(state),known=new Set(pending.map(op=>op.opId)),incoming=clone(value.operations.filter(op=>!known.has(op.opId)));
        for(const op of incoming)if(op.kind==='setting'&&op.key==='focusTimer'&&op.value)Object.assign(op.value,{running:false,endAt:null,segmentStartedAt:null,segmentId:null});
        if(options.includeCached)for(const col of collections)for(const record of value.state[col]||[]){
            if(!next[col]?.some(item=>item.id===record.id)&&!incoming.some(op=>op.collection===col&&op.id===record.id)){
                if(col==='assistantDrafts'||col==='focusSessions'&&record.source==='planning-focus')throw new Error('包内含电脑已确认的专注历史或助手回执，不能作为离线操作重放。可取消“补齐缓存记录”仅恢复未同步内容；完整确认历史请用学习备份恢复预演');
                const {id,...fields}=record;incoming.push({opId:uid(),kind:'upsert',collection:col,id,fields,unset:[],base:{},baseExists:false,baseRecord:{}});
            }
        }
        for(const op of incoming)if(op.kind==='upsert'&&!next[op.collection]?.some(record=>record.id===op.id)){
            const original=value.state[op.collection]?.find(record=>record.id===op.id);
            if(original)(next[op.collection]||=[]).push({...clone(op.baseRecord||original),id:op.id});
        }
        applyOperations(next,incoming);
        const nextPending=[...pending,...clone(incoming)],nextObserved=clone(next);
        const committed=await DeviceStore.batch([...entries,['rescue-before-'+Date.now(),{state:clone(state),pending:clone(pending),observed:clone(observed),revision}],['workspace',{state:next,pending:nextPending,observed:nextObserved,revision}]],{preserveDrafts:true});
        state=next;pending=nextPending;observed=nextObserved;
        for(const [key,item]of entries)if(key.startsWith('upload-')){if(item)uploadQueue.set(item.id,item);else uploadQueue.delete(key.slice(7));}
        try{const previousCopies=await DeviceStore.entries('rescue-before-');if(previousCopies.length>5)await DeviceStore.batch(previousCopies.sort(([a],[b])=>a.localeCompare(b)).slice(0,-5).map(([key])=>[key,undefined]));}catch(_){toast('恢复已提交，旧保护副本暂未自动整理');}
        try{YantuEnvironment.storage.setItem(LOCAL_KEY,JSON.stringify(state));persistQueue();}catch(_){toast('已保留在设备数据库，浏览器小容量缓存已满');}
        try{refreshActive();status(`已恢复，待同步 ${pending.length+uploadQueue.size} 项`,false);}catch(_){toast('恢复已提交，请重新打开页面以显示全部内容');}
        return {operations:incoming.length,files:uploadQueue.size,draftConflicts:committed?.draftConflicts||0};
    }

    saveFile = uploadFile;
    getFile = async id => {
        if (!localMode) return originalGetFile(id);
        // Attachment IDs are immutable: reuse the original, including offline uploads.
        const cached = await DeviceStore.get('file-' + id).catch(() => null);
        if (cached) return cached;
        try {
            const response = await request(`/api/files/${encodeURIComponent(id)}`, {long: true});
            const blob = await response.blob();
            DeviceStore.put('file-' + id, blob).catch(() => {});
            return blob;
        } catch (error) {
            const blob = await DeviceStore.get('file-' + id);
            if (blob) return blob;
            throw error;
        }
    };
    deleteFile = async id => {
        if (!localMode) return originalDeleteFile(id);
        await request(`/api/files/${encodeURIComponent(id)}`, {method: 'DELETE'});
        await DeviceStore.put('file-' + id, undefined);
        await DeviceStore.put('attachment-' + id, undefined);
    };

    function recordFiles(record) {
        const files = [...(record?.files || [])];
        if (record?.fileId && !files.some(f => f.id === record.fileId)) files.push({id: record.fileId, name: `${record.title || '试卷'}.pdf`, type: 'application/pdf'});
        return files;
    }

    findRecordWithCol = id => {
        for (const col of fileCollections) {
            const record = (state[col] || []).find(r => r.id === id);
            if (record) return {record, col};
        }
        return {record: null, col: null};
    };
    findRecord = id => findRecordWithCol(id).record;
    editRecord = id => {
        const {record, col} = findRecordWithCol(id);
        if (record) col === 'papers' ? openPaperDialog(id) : openRecordDialog(col, id);
    };
    getListContainerId = col => ({mistakes: 'mistakeList', notes: 'noteList', resources: 'resourceList', mindmaps: 'mindmapList', papers: 'paperList'}[col]);
    getFilterContainerId = col => ({mistakes: 'mistakeFilter', notes: 'noteFilter', resources: 'resourceFilter', mindmaps: 'mindmapFilter', papers: 'paperFilter'}[col]);

    renderRecordCard = (record, subject) => {
        const files = recordFiles(record);
        const previewIndex = files.findIndex(file => file.type?.startsWith('image/') && (!file.size || file.size <= 12 * 1024 * 1024));
        const title = record.title || `${record.year || ''}年${record.subject || ''}试卷`;
        const cover = previewIndex >= 0 ? `<button class="record-thumbnail" onclick="openViewer('${record.id}',${previewIndex})" title="打开照片" aria-label="打开 ${html(title)} 的照片"><i class="fas fa-image" aria-hidden="true"></i><img data-preview-file="${html(files[previewIndex].id)}" data-preview-record="${html(record.id)}" alt="${html(files[previewIndex].name)}" width="94" height="110" hidden /></button>` : `<div class="record-file-mark" aria-hidden="true"><i class="fas ${files.length ? 'fa-file-lines' : 'fa-book-open'}"></i></div>`;
        return `<article class="record-item file-record" data-record-id="${html(record.id)}">${cover}<div class="record-body"><div class="record-heading"><div><h4><span class="badge badge-${html(subject)}">${html(subject)}</span> ${html(title)}</h4><div class="record-meta">${html(record.path || record.subject || '')} · ${html(record.date || '')} · ${files.length} 个附件</div></div><div class="record-toolbar"><button class="btn btn-icon btn-ghost" onclick="editRecord('${record.id}')" title="编辑" aria-label="编辑"><i class="fas fa-pen"></i></button><button class="btn btn-icon btn-danger" onclick="deleteItem('${record.id}')" title="删除" aria-label="删除"><i class="fas fa-trash"></i></button></div></div>${record.content || record.remark ? `<p class="record-content">${html(record.content || record.remark)}</p>` : ''}<div class="attachment-strip">${files.map((file, index) => `<button class="attachment-button" onclick="openViewer('${record.id}', ${index})" title="${html(file.name)}"><i class="fas ${file.type?.startsWith('image/') ? 'fa-image' : file.type === 'application/pdf' ? 'fa-file-pdf' : 'fa-file-lines'}"></i><span>${html(file.name)}</span></button>`).join('')}</div>${files.length ? `<div class="record-toolbar"><button class="btn btn-primary btn-sm" onclick="openViewer('${record.id}')"><i class="fas fa-eye"></i> 打开</button><button class="btn btn-ghost btn-sm" onclick="downloadRecord('${record.id}')"><i class="fas fa-download"></i> 下载附件</button></div>` : ''}</div></article>`;
    };

    openRecordDialog = (col, id) => {
        originalOpenRecord(col, id);
        uploadDrafts.delete(el('recordForm'));
        el('recordDialogTitle').textContent = `${id ? '编辑' : '添加'}${{mistakes: '错题', notes: '笔记', resources: '资料', mindmaps: '导图'}[col]}`;
        const form = el('recordForm');
        form.elements.namedItem('date').value ||= formatDate(nowDate());
        renderExistingFiles(id);
    };

    function renderExistingFiles(id) {
        const record = findRecord(id);
        el('existingAttachments').innerHTML = recordFiles(record).map(file => `<div class="attachment-row"><span>${html(file.name)}</span><button type="button" class="btn btn-icon btn-danger" onclick="PrepLocal.removeAttachment('${id}','${file.id}')" title="删除此附件" aria-label="删除此附件"><i class="fas fa-trash"></i></button></div>`).join('');
    }

    function busy(form, active) {
        const button = form.querySelector('[type=submit]');
        button.disabled = active;
        if (active) { button.dataset.label = button.innerHTML; button.textContent = '正在保存…'; }
        else button.innerHTML = button.dataset.label || '保存';
    }
    async function draftFiles(form, field, oldFiles) {
        let draft = uploadDrafts.get(form);
        if (!draft) {
            draft = {id:form.elements.id.value || uid(), uploaded:new Map(), ids:new Map()};
            uploadDrafts.set(form, draft);
            form.elements.id.value = draft.id;
        }
        const files = [...oldFiles];
        for (const [index, file] of new FormData(form).getAll(field).filter(file => file.name).entries()) {
            const key = `${index}:${file.name}:${file.size}:${file.lastModified}`;
            if (!draft.uploaded.has(key)) {
                if (!draft.ids.has(key)) draft.ids.set(key, uid());
                const id = draft.ids.get(key), response = await uploadFile(id, file);
                const metadata = response?.json ? await response.json() : {};
                draft.uploaded.set(key, {id,name:file.name,type:file.type || (file.name.toLowerCase().endsWith('.pdf') ? 'application/pdf' : 'application/octet-stream'),size:file.size,...(metadata.sha256?{sha256:metadata.sha256}:{}),...(metadata.pendingUpload?{pendingUpload:true,selectedAt:metadata.selectedAt}:{uploadedAt:metadata.uploadedAt || nowDate().toISOString()})});
            }
            const metadata = draft.uploaded.get(key);
            if(metadata.pendingUpload&&!uploadQueue.has(metadata.id)){
                const uploaded=await DeviceStore.get('file-meta-'+metadata.id);
                if(uploaded){delete metadata.pendingUpload;metadata.uploadedAt=uploaded.uploadedAt||nowDate().toISOString();if(uploaded.sha256)metadata.sha256=uploaded.sha256;}
            }
            if (!files.some(item => item.id === metadata.id)) files.push(metadata);
        }
        return {id:draft.id, files};
    }

    el('recordForm').onsubmit = async event => {
        event.preventDefault();
        const form = event.target;
        const fd = new FormData(form);
        const col = fd.get('collection');
        const old = state[col].find(r => r.id === fd.get('id'));
        busy(form, true);
        try {
            const {id, files} = await draftFiles(form, 'files', old?.files || []);
            const record = {...old,id,path:fd.get('path'),title:fd.get('title'),content:fd.get('content'),date:fd.get('date') || formatDate(nowDate()),files,updatedAt:nowDate().toISOString()};
            if (form.elements.namedItem('tags')) record.tags = String(fd.get('tags') || '').split(/[,，;；]/).map(tag => tag.trim()).filter(Boolean);
            if (form.elements.namedItem('favorite')) record.favorite = fd.has('favorite');
            window.StudyLibrary?.classify(record, fd);
            if (col === 'mistakes' && form.elements.namedItem('cause')) Object.assign(record,{cause:fd.get('cause'),solution:fd.get('solution'),mastery:fd.get('mastery') || 'new',nextReview:fd.get('nextReview') || ''});
            window.StudyFlow?.extendRecord(record, fd, form);
            const index = state[col].findIndex(r => r.id === id);
            if (index < 0) state[col].unshift(record);
            else state[col][index] = record;
            saveState();
            uploadDrafts.delete(form);
            el('recordDialog').close();
            refreshRecords(col);
            const visible = window.StudyLibrary?.saved?.(col,id);
            window.dispatchEvent(new CustomEvent('workspace-change', {detail:{collection:col,id}}));
            toast(visible===false ? '已保存，当前筛选未显示该记录' : pending.length ? '已保存在本机，后台同步中' : '已保存');
            flush().catch(error => toast('记录已保留在本机：' + error.message));
        } catch (error) { toast(`保存失败：${error.message}`); }
        finally { busy(form, false); }
    };

    el('paperForm').onsubmit = async event => {
        event.preventDefault();
        const form = event.target;
        const fd = new FormData(form);
        const old = state.papers.find(r => r.id === fd.get('id'));
        busy(form, true);
        try {
            const {id,files} = await draftFiles(form, 'file', recordFiles(old));
            const fileId = old?.fileId || files.find(file => file.type === 'application/pdf')?.id;
            const data = {...old,id,title:fd.get('title')?.trim() || `${fd.get('year')}年${fd.get('subject')}真题`,year:fd.get('year'),subject:fd.get('subject'),remark:fd.get('remark'),date:old?.date || formatDate(nowDate()),files,updatedAt:nowDate().toISOString()};
            if (form.elements.namedItem('tags')) data.tags = String(fd.get('tags') || '').split(/[,，;；]/).map(tag => tag.trim()).filter(Boolean);
            if (form.elements.namedItem('practiceStatus')) data.practiceStatus = fd.get('practiceStatus');
            if (form.elements.namedItem('favorite')) data.favorite = fd.has('favorite');
            window.StudyLibrary?.classify(data, fd);
            if (fileId) data.fileId = fileId;
            const index = state.papers.findIndex(r => r.id === id);
            if (index < 0) state.papers.unshift(data);
            else state.papers[index] = data;
            saveState();
            uploadDrafts.delete(form);
            el('paperDialog').close();
            renderPapers();
            if(window.StudyLibrary?.saved?.('papers',id)===false)toast('已保存，当前筛选未显示该试卷');
            window.dispatchEvent(new CustomEvent('workspace-change', {detail:{collection:'papers',id}}));
            flush().catch(error => toast('试卷已保留在本机：' + error.message));
        } catch (error) { toast(error.message); }
        finally { busy(form, false); }
    };

    window.deleteItem = async id => {
        const {record, col} = findRecordWithCol(id);
        if (!record || !confirm('删除这条记录及其附件？')) return;
        state[col] = state[col].filter(r => r.id !== id);
        saveState();
        await flush();
        if (!pending.length) {
            for (const file of recordFiles(record)) {
                await deleteFile(file.id).catch(error => { if (error.status !== 409) toast(error.message); });
            }
        }
        refreshRecords(col);
    };

    async function removeAttachment(recordId, fileId) {
        const record = findRecord(recordId);
        if (!record || !confirm('从本组删除这个附件？')) return;
        record.files = (record.files || []).filter(f => f.id !== fileId);
        if (record.fileId === fileId) delete record.fileId;
        saveState();
        await flush();
        if (!pending.length) await deleteFile(fileId).catch(error => { if (error.status !== 409) toast(error.message); });
        renderExistingFiles(recordId);
        window.StudyLibrary?.paperAttachments(recordId);
        refreshActive();
    }

    window.openViewer = async (id, index = 0, options = {}) => {
        const record = findRecord(id);
        if (!record || !recordFiles(record).length) return;
        window.StudyLibrary?.touch(id);
        currentViewer = {id, idx: index, zoom: 1};
        if (!el('viewerDialog').open) el('viewerDialog').showModal();
        await renderViewerMedia(options);
    };
    window.viewerPage = step => {
        const files = recordFiles(findRecord(currentViewer.id));
        if (!files.length) return;
        currentViewer.idx = (currentViewer.idx + step + files.length) % files.length;
        currentViewer.zoom = 1;
        renderViewerMedia();
    };
    window.viewerZoom = step => {
        currentViewer.zoom = Math.max(0.25, Math.min(5, currentViewer.zoom + step));
        applyZoom();
    };

    function applyZoom() {
        const media = el('viewerBody').querySelector('.viewer-media, .viewer-frame');
        if (!media) return;
        media.style.width = `${currentViewer.zoom * 100}%`;
        media.style.maxWidth = 'none';
        media.style.height = media.tagName === 'IFRAME' ? `${currentViewer.zoom * 100}%` : 'auto';
    }

    function invalidateViewer() {
        mediaRequest++;
        if (mediaURL) URL.revokeObjectURL(mediaURL);
        mediaURL = '';
    }

    renderViewerMedia = async () => {
        const requestId = ++mediaRequest;
        const record = findRecord(currentViewer.id);
        const files = recordFiles(record);
        const file = files[currentViewer.idx];
        if (!file) { el('viewerDialog').close(); return; }
        el('viewerTitle').textContent = record.title || '资料查看';
        el('viewerMeta').textContent = `${currentViewer.idx + 1} / ${files.length} · ${file.name}`;
        el('viewerBody').innerHTML = '<div class="viewer-message">正在读取…</div>';
        if (mediaURL) { URL.revokeObjectURL(mediaURL); mediaURL = ''; }
        try {
            let url;
            if (localMode && !PrepTransport.base()) {
                url = `/api/files/${encodeURIComponent(file.id)}`;
            } else {
                const blob = await getFile(file.id);
                if (!blob) throw new Error('附件未找到');
                url = URL.createObjectURL(blob);
            }
            if (requestId !== mediaRequest || !el('viewerDialog').open) { if (url.startsWith('blob:')) URL.revokeObjectURL(url); return; }
            if (url.startsWith('blob:')) mediaURL = url;
            const media = document.createElement(file.type?.startsWith('audio/') ? 'audio' : file.type?.startsWith('image/') ? 'img' : 'iframe');
            media.className = media.tagName === 'IMG' ? 'viewer-media' : 'viewer-frame';
            media.src = url;
            if (media.tagName === 'AUDIO') { media.controls = true; media.preload = 'metadata'; }
            media.title = file.name;
            if (media.tagName === 'IMG') media.alt = file.name;
            if (media.tagName === 'IMG') media.onerror = () => { if (requestId === mediaRequest) el('viewerBody').textContent = '浏览器不能解码此图片，可下载原件查看；原件与记录已保留。'; };
            el('viewerBody').replaceChildren(media);
            el('viewerBody').scrollTo(0, 0);
            applyZoom();
        } catch (error) { if (requestId === mediaRequest) el('viewerBody').innerHTML = `<div class="viewer-message">${html(error.message)}</div>`; }
    };

    async function fullscreen() {
        const dialog = el('viewerDialog');
        if (document.fullscreenElement) { await document.exitFullscreen(); return; }
        if (dialog.classList.contains('is-expanded')) { dialog.classList.remove('is-expanded'); return; }
        dialog.classList.add('is-expanded');
        try {
            const shell = dialog.querySelector('.viewer-shell');
            if (shell.requestFullscreen && document.fullscreenEnabled) await shell.requestFullscreen();
        } catch (_) { /* A viewport-sized dialog works where the browser disallows native fullscreen. */ }
    }

    window.downloadRecord = async id => {
        try {
            for (const file of recordFiles(findRecord(id))) {
                if (file.library && !PrepTransport.base()) {
                    await NebulaDownloads.url(file.name, '/api/files/' + encodeURIComponent(file.id) + '?download=1');
                    continue;
                }
                const blob = await getFile(file.id);
                if (blob) downloadBlob(file.name, blob);
            }
        } catch (error) { toast(error.message); }
    };

    function focusTimer() {
        return state.focusTimer ||= {minutes: 25, remaining: 1500, running: false, endAt: null};
    }
    function recordFocusSegment(timer) {
        if (!timer.segmentId || !timer.segmentStartedAt) return;
        const start = Number(timer.segmentStartedAt), end = Math.min(nowMs(), Number(timer.endAt) || nowMs());
        const seconds = Math.max(0, Math.floor((end - start) / 1000));
        if (seconds > 0) {
            const sessions = state.focusSessions ||= [];
            const item = {id:timer.segmentId, startedAt:new Date(start).toISOString(), endedAt:new Date(end).toISOString(), seconds, subject:timer.subject || '未分类', activityType:YantuActivity.activityType(timer.segmentActivityType), source:'timer'};
            const index = sessions.findIndex(session => session.id === item.id);
            if (index < 0) sessions.push(item);
            else if (sessions[index].seconds < seconds) sessions[index] = item;
        }
        delete timer.segmentId;
        delete timer.segmentStartedAt;
        delete timer.segmentActivityType;
    }
    function secondsLeft() {
        const timer = focusTimer();
        return timer.running ? Math.max(0, Math.ceil((timer.endAt - nowMs()) / 1000)) : timer.remaining;
    }
    function renderTimer() {
        const timer = focusTimer();
        const seconds = secondsLeft();
        const signature = [seconds,timer.minutes,timer.running].join('|');
        if (signature === lastTimerRender) return;
        lastTimerRender = signature;
        el('focusClock').textContent = `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`;
        const dial = el('focusDial');
        if (dial) {
            dial.style.setProperty('--focus-progress', `${Math.min(100, Math.max(0, seconds / (timer.minutes * 60) * 100))}%`);
            dial.dataset.state = timer.running ? 'running' : seconds < timer.minutes * 60 && seconds > 0 ? 'paused' : 'idle';
            dial.setAttribute('aria-valuemax', String(timer.minutes * 60));
            dial.setAttribute('aria-valuenow', String(seconds));
            dial.setAttribute('aria-valuetext', el('focusClock').textContent);
        }
        el('focusStatus').textContent = timer.running ? '专注中' : seconds < timer.minutes * 60 && seconds > 0 ? '已暂停' : seconds === 0 ? '已结束' : '待开始';
        el('focusStart').innerHTML = `<i class="fas fa-play"></i> ${seconds < timer.minutes * 60 && seconds > 0 ? '继续' : '开始'}`;
        el('focusStart').disabled = timer.running;
        el('focusPause').disabled = !timer.running;
        el('focusMinutes').disabled = timer.running;
        if (document.activeElement !== el('focusMinutes')) el('focusMinutes').value = timer.minutes;
        if (timer.running && seconds === 0) {
            recordFocusSegment(timer);
            timer.running = false;
            timer.remaining = 0;
            timer.endAt = null;
            lastTimerRender = '';
            saveState();
            toast('本轮专注结束');
            sendNotification('专注结束', '本轮学习已完成，可以休息一下。');
        }
    }
    function timerAction(action) {
        const timer = focusTimer();
        if(action==='start'&&state.planningFocusLease?.status==='running'){toast('关联专注仍在计时，请先暂停；已有时间会保留');return;}
        if (action === 'start' && !timer.running) {
            if (timer.remaining <= 0) timer.remaining = timer.minutes * 60;
            timer.endAt = nowMs() + timer.remaining * 1000;
            timer.segmentId = uid();
            timer.segmentStartedAt = nowMs();
            timer.segmentActivityType = YantuActivity.activityType(timer.activityType);
            timer.running = true;
        } else if (action === 'pause' && timer.running) {
            recordFocusSegment(timer);
            timer.remaining = secondsLeft();
            timer.running = false;
            timer.endAt = null;
        } else if (action === 'reset') {
            if (timer.running) recordFocusSegment(timer);
            Object.assign(timer, {remaining: timer.minutes * 60, running: false, endAt: null});
        }
        saveState();
        renderTimer();
    }
    function timerDuration(value) {
        const timer = focusTimer();
        if (timer.running) return;
        timer.minutes = Math.max(1, Math.min(180, Number(value) || 25));
        timer.remaining = timer.minutes * 60;
        saveState();
        renderTimer();
    }

    renderView = view => {
        originalRenderView(view);
        if (view === 'mindmaps') renderRecords('mindmaps', el('mindmapList'), el('mindmapFilter'));
        if (view === 'exams') renderExams();
        if (view === 'connection') refreshConnection();
        const titles = {overview: '今日总览', timeline: '时间周期表', mistakes: '错题集', resources: '学习资料', mindmaps: '思维导图', papers: '真题试卷', scores: '成绩追踪', review: '每日复盘', schools: '目标院校', exams: '考试日历', connection: '设备与数据'};
        el('workspaceTitle').textContent = titles[view] || '研途备考';
        renderTimer();
    };

    function refreshActive() {
        if (!initialized) return;
        applying = true;
        try {
            const current = document.querySelector('.nav-item.active')?.dataset.view || 'overview';
            const filters = [...document.querySelectorAll('.view.active .category-bar')].map(bar => [bar.id, bar.dataset.activeFilter || 'all']);
            renderView(current);
            for (const [id, filter] of filters) {
                const bar = el(id);
                if (filter !== 'all' && bar.dataset.activeFilter !== filter) {
                    const button = [...bar.querySelectorAll('button')].find(btn => btn.textContent.startsWith(filter));
                    if (button) filterRecords(button, filter);
                }
            }
            updateCountdown();
            if (el('statusDialog').open) updateStatusDialog();
            if (el('viewerDialog').open && !findRecord(currentViewer.id)) el('viewerDialog').close();
        } finally { applying = false; }
    }

    openScoreDialog = id => {
        originalOpenScore(id);
        el('scoreForm').elements.namedItem('date').value ||= formatDate(nowDate());
    };

    async function refreshConnection() {
        if (!localMode) { el('connectionPath').textContent = '请使用本地服务打开'; return; }
        try {
            const info = await jsonRequest('/api/info');
            el('connectionAddresses').innerHTML = info.urls.length ? info.urls.map(url => `<div class="address-row"><a href="${html(url)}" target="_blank" rel="noopener">${html(url)}</a><button class="btn btn-icon btn-ghost" onclick="PrepLocal.copyAddress('${url}')" title="复制地址" aria-label="复制地址"><i class="fas fa-copy"></i></button></div>`).join('') : '未检测到局域网网卡';
            el('connectionCode').textContent = info.local ? info.code : '请在电脑端查看';
            el('connectionPath').textContent = info.dataPath || '资料保存在已连接的电脑上';
            el('connectionFiles').textContent = `${info.files} 个文件 · ${(info.bytes / 1024 / 1024).toFixed(1)} MB`;
            const remote = info.remote || {};
            el('connectionRemote').innerHTML = remote.active && remote.url ? `<div class="address-row"><a href="${html(remote.url)}" target="_blank" rel="noopener">${html(remote.url)}</a><button class="btn btn-icon btn-ghost" onclick="PrepLocal.copyAddress('${remote.url}')" title="复制远程地址" aria-label="复制远程地址"><i class="fas fa-copy"></i></button></div>` : html(remote.error || '未启用');
            el('remoteStart').hidden = !info.local;
            el('remoteStop').hidden = !info.local;
            el('remoteStart').disabled = remote.active || !remote.available;
            el('remoteStop').disabled = !remote.active;
            el('rotateCodeButton').hidden = !info.local;
            status(online ? pending.length ? `待同步 ${pending.length} 项` : '已同步到电脑' : '等待连接', online);
        } catch (error) { toast(error.message); }
    }

    function showConnection() { document.querySelector('[data-view=connection]').click(); }
    async function copyAddress(url) {
        if (navigator.clipboard && window.isSecureContext) { await navigator.clipboard.writeText(url); toast('地址已复制'); }
        else prompt('手机访问地址', url);
    }
    async function rotateCode() {
        await jsonRequest('/api/pair-code', {});
        await refreshConnection();
    }

    async function controlRemote(action) {
        const button = el(action === 'start' ? 'remoteStart' : 'remoteStop');
        button.disabled = true;
        try {
            toast(action === 'start' ? '正在连接远程网络…' : '正在关闭远程连接…');
            await request(`/api/remote/${action}`, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}', long: true});
            toast(action === 'start' ? '远程地址已就绪' : '远程访问已关闭');
        } catch (error) { toast(error.message); }
        finally { await refreshConnection(); }
    }

    function studySettings() {
        return state.studySchedule || {weekly: 5, monthly: 25, major: 3, holidays: []};
    }
    function studyDay(date) { return !isRestDay(date) && !(studySettings().holidays || []).includes(formatDate(date)); }
    function nextStudyExam(interval) {
        let date = new Date(`${state.prepStart || formatDate(nowDate())}T12:00:00+08:00`);
        let count = 0;
        const today = formatDate(nowDate());
        const weekdays = state.reminderSettings?.customRestDays || [];
        if (state.reminderSettings?.restDayMode === 'custom' && weekdays.length === 7) return '未设置学习日';
        for (let i = 0; i < 10000; i++) {
            if (studyDay(date)) count++;
            if (count && count % interval === 0 && formatDate(date) >= today) return formatDate(date);
            date.setDate(date.getDate() + 1);
        }
        return '请调整安排';
    }
    function nextMajorExam(months) {
        const start = new Date(`${state.prepStart || formatDate(nowDate())}T12:00:00+08:00`);
        for (let period = 1; period < 200; period++) {
            const date = new Date(start);
            date.setDate(1);
            date.setMonth(start.getMonth() + months * period);
            const last = new Date(date.getFullYear(), date.getMonth() + 1, 0).getDate();
            date.setDate(Math.min(start.getDate(), last));
            for (let skip = 0; skip < 366 && !studyDay(date); skip++) date.setDate(date.getDate() + 1);
            if (formatDate(date) >= formatDate(nowDate())) return formatDate(date);
        }
        return '请调整安排';
    }
    function renderExams() {
        const settings = studySettings();
        const form = el('studyScheduleForm');
        if (!form.contains(document.activeElement)) {
            form.elements.namedItem('start').value = state.prepStart || formatDate(nowDate());
            for (const key of ['weekly', 'monthly', 'major']) form.elements.namedItem(key).value = settings[key];
            form.elements.namedItem('holidays').value = (settings.holidays || []).join(',');
        }
        el('scheduledExams').innerHTML = [['周测', nextStudyExam(settings.weekly)], ['月考', nextStudyExam(settings.monthly)], ['大考', nextMajorExam(settings.major)]].map(([title, date]) => `<div class="scheduled-exam"><strong>${title}</strong><span>${date}</span><button class="btn btn-ghost btn-sm" onclick="PrepLocal.openExam(null,'${title}','${date}')">安排内容</button></div>`).join('');
        el('calendarEventList').innerHTML = [...(state.calendarEvents || [])].sort((a, b) => a.date.localeCompare(b.date)).map(event => `<div class="record-item"><div class="record-heading"><div><h4>${html(event.title)}</h4><p class="record-meta">${html(event.date)}</p><p class="record-content">${html(event.content)}</p></div><div class="record-toolbar"><button class="btn btn-icon btn-ghost" onclick="PrepLocal.openExam('${event.id}')" title="编辑"><i class="fas fa-pen"></i></button><button class="btn btn-icon btn-danger" onclick="PrepLocal.deleteExam('${event.id}')" title="删除"><i class="fas fa-trash"></i></button></div></div></div>`).join('');
    }
    function openExam(id, title = '', date = formatDate(nowDate())) {
        const form = el('calendarExamForm');
        form.reset();
        form.elements.namedItem('id').value = id || '';
        form.elements.namedItem('title').value = title;
        form.elements.namedItem('date').value = /^\d{4}-\d{2}-\d{2}$/.test(date) ? date : formatDate(nowDate());
        if (id) fillForm(form, state.calendarEvents.find(event => event.id === id));
        el('examDialog').showModal();
    }
    function deleteExam(id) {
        if (!confirm('删除这场考试？')) return;
        state.calendarEvents = state.calendarEvents.filter(event => event.id !== id);
        saveState();
        renderExams();
    }

    async function backup() {
        try {
            if (localMode) {
                await flush();
                if (pending.length || uploadQueue.size) throw new Error('请等同步完成后再备份，或导出本机紧急包');
                if (window.DeviceAI) await DeviceAI.sync();
                await NebulaDownloads.url('yantu-backup.zip', '/api/export');
            } else {
                const files = {};
                for (const col of fileCollections) {
                    for (const record of state[col] || []) {
                        for (const file of recordFiles(record)) {
                            if (files[file.id]) continue;
                            const blob = await originalGetFile(file.id);
                            if (blob) files[file.id] = await new Promise((resolve, reject) => { const reader = new FileReader(); reader.onload = () => resolve(reader.result); reader.onerror = reject; reader.readAsDataURL(blob); });
                        }
                    }
                }
                downloadBlob('yantu-backup.json', new Blob([JSON.stringify({version: 2, state, files})], {type: 'application/json'}));
            }
        } catch (error) { toast(error.message); }
    }
    exportData = backup;

    async function importFile(file) {
        if (!file) return;
        try {
            toast('正在导入…');
            if (file.name.toLowerCase().endsWith('.zip')) {
                if (!localMode) throw new Error('ZIP 备份需要在本地服务中导入');
                await flush();
                if(window.RestoreUI&&window.Workspace?.ready()){
                    const result=await RestoreUI.open(file);if(!result)return;accept(result);
                }else{const response = await request('/api/import', {method: 'POST', body: file, long: true});accept(await response.json());}
            } else {
                const text = (await file.text()).trim();
                const data = JSON.parse(text.startsWith('{') ? text : LZString.decompressFromBase64(text));
                if(await importPayload(data)===false)return;
            }
            el('importDialog').close();
            refreshActive();
            toast('资料已导入');
        } catch (error) { toast(`导入失败：${error.message}`); }
    }
    async function importPayload(data) {
        if (!data.state) throw new Error('无效备份');
        if(window.RestoreUI&&window.Workspace?.ready()){
            const result=await RestoreUI.legacy(data);if(result)accept(result);return Boolean(result);
        }
        for (const [id, url] of Object.entries(data.files || {})) {
            const response = await fetch(url);
            const blob = await response.blob();
            let name = '附件';
            for (const col of fileCollections) {
                const file = (data.state[col] || []).flatMap(recordFiles).find(f => f.id === id);
                if (file) name = file.name;
            }
            await uploadFile(id, blob, name);
        }
        for (const [key, value] of Object.entries(data.state)) {
            if (collections.has(key)) {
                const records = state[key] ||= [];
                for (const record of value) {
                    const index = records.findIndex(r => r.id === record.id);
                    if (index < 0) records.push(record);
                    else records[index] = record;
                }
            } else if (key !== 'deviceId') state[key] = value;
        }
        saveState();
        await flush();
    }
    window.handleImportFile = input => importFile(input.files[0]);
    window.importData = async () => {
        try {
            const text = el('importDataInput').value.trim();
            if(await importPayload(JSON.parse(text.startsWith('{') ? text : LZString.decompressFromBase64(text)))===false)return;
            el('importDialog').close();
            refreshActive();
            toast('资料已导入');
        } catch (error) { toast(error.message); }
    };

    function ready() {
        el('workspaceDate').parentElement.parentElement.prepend(el('toggleSidebar'));
        document.querySelector('.overview-columns > .card:last-child').prepend(document.querySelector('.focus-tool'));
        renderClock();
        el('sidebarSyncText').textContent = localMode ? '已连接本机数据' : '浏览器本地模式';
        document.querySelectorAll('.nav-item').forEach(button => button.addEventListener('click', () => el('mainContent').scrollIntoView({block: 'start'})));
        el('viewerDialog').addEventListener('close', () => {
            mediaRequest++;
            if (mediaURL) URL.revokeObjectURL(mediaURL);
            mediaURL = '';
            el('viewerBody').replaceChildren();
            el('viewerDialog').classList.remove('is-expanded');
            if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
        });
        document.addEventListener('fullscreenchange', () => {
            if (!document.fullscreenElement) el('viewerDialog').classList.remove('is-expanded');
        });
        document.addEventListener('keydown', event => {
            if (!el('viewerDialog').open || !el('viewerDialog').contains(event.target) || /INPUT|TEXTAREA|SELECT/.test(event.target.tagName)) return;
            if (event.key === 'ArrowLeft') viewerPage(-1);
            if (event.key === 'ArrowRight') viewerPage(1);
        });
        el('backupImport').onchange = event => importFile(event.target.files[0]);
        el('studyScheduleForm').onsubmit = event => {
            event.preventDefault();
            const data = Object.fromEntries(new FormData(event.target));
            state.prepStart = data.start;
            state.studySchedule = {weekly: Math.max(1, Number(data.weekly) || 5), monthly: Math.max(1, Number(data.monthly) || 25), major: Math.max(1, Number(data.major) || 3), holidays: data.holidays.split(/[,，\s]+/).filter(value => /^\d{4}-\d{2}-\d{2}$/.test(value))};
            saveState();
            renderExams();
            toast('考试安排已保存');
        };
        el('calendarExamForm').onsubmit = event => {
            event.preventDefault();
            const data = Object.fromEntries(new FormData(event.target));
            data.id ||= uid();
            const index = state.calendarEvents.findIndex(r => r.id === data.id);
            if (index < 0) state.calendarEvents.push(data);
            else state.calendarEvents[index] = data;
            saveState();
            el('examDialog').close();
            renderExams();
        };
        renderTimer();
        capture();
        setInterval(renderTimer, 250);
        setInterval(renderClock, 1000);
        setInterval(syncClock, 30000);
        let day = formatDate(nowDate());
        setInterval(() => {
            const today = formatDate(nowDate());
            if (today !== day) { day = today; renderOverview(); updateCountdown(); }
            if (localMode) pending.length ? flush() : pull();
        }, YantuEnvironment.hosted ? 5000 : location.hostname.endsWith('.trycloudflare.com') ? 3000 : 15000);
        window.addEventListener('online', () => { flush(); pull(); });
        document.addEventListener('visibilitychange', () => { if (!document.hidden) { syncClock(); renderTimer(); pull(); } });
        if (state.reminderSettings?.enabled) setupReminders();
        refreshConnection();
        document.body.classList.add('app-ready');
    }

    window.PrepLocal = {boot, ready, capture, toast, fullscreen, timerAction, timerDuration, showConnection, refreshConnection, copyAddress, rotateCode, removeAttachment, openExam, deleteExam, backup, nowDate, dateText, timeText, diagnostics, dayStart, rescueSnapshot, checkRescue, restoreRescue, invalidateViewer, fileQueue:()=>clone([...uploadQueue.values()]), resetUploads: id => uploadDrafts.delete(el(id)), weekday: date => systemDate(date).getUTCDay(), controlRemote, flush, pull, accept, jsonRequest, hasPending: () => pending.length > 0 || uploadQueue.size > 0, stampText: value => new Intl.DateTimeFormat('zh-CN', {timeZone: 'UTC', year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false}).format(systemDate(new Date(value)))};
})();
