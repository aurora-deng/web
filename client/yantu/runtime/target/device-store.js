(() => {
    const ready = new Promise((resolve, reject) => {
        const request = indexedDB.open(YantuEnvironment.database('yantu-device-assistant'), 1);
        request.onupgradeneeded = () => request.result.createObjectStore('data');
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
    });
    async function get(key) {
        const db = await ready;
        return new Promise((resolve, reject) => {
            const request = db.transaction('data').objectStore('data').get(key);
            request.onsuccess = () => resolve(request.result);
            request.onerror = () => reject(request.error);
        });
    }
    async function put(key, value) {
        const db = await ready;
        return new Promise((resolve, reject) => {
            const tx = db.transaction('data', 'readwrite');
            if (value === undefined) tx.objectStore('data').delete(key);
            else tx.objectStore('data').put(value, key);
            tx.oncomplete = resolve;
            tx.onerror = () => reject(tx.error);
            tx.onabort = () => reject(tx.error || new Error('设备存储失败'));
        });
    }
    async function batch(entries, options = {}) {
        const db = await ready;
        const draftPattern=/^(reader-note-draft-|reader-region-draft-|richnote-draft-|card-draft-|board-draft-|practice-local-|planning-draft-|planning-focus-draft-|subject-tools-draft-|exam-local-|workbench-draft-|assistant-local-)[A-Za-z0-9_.-]{1,201}$/;
        const guarded=new Map();
        if(options.preserveDrafts)for(const[key,value]of entries)if(draftPattern.test(key)&&value!==undefined){const serialized=JSON.stringify(value),bytes=new TextEncoder().encode(key+'\n'+serialized),digest=[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(n=>n.toString(16).padStart(2,'0')).join('');guarded.set(key,{serialized,key:'rescue-draft-conflict-'+digest});}
        return new Promise((resolve, reject) => {
            const tx = db.transaction('data', 'readwrite'), object = tx.objectStore('data');let draftConflicts=0,failure;
            for(const[key,expected]of options.expected||[]){const request=object.get(key);request.onsuccess=()=>{if(JSON.stringify(request.result)!==JSON.stringify(expected)){failure=new Error('草稿刚在其他位置变化，尚未覆盖，请重新打开比较');tx.abort();}};}
            for (const [key, value] of entries) {
                const guard=guarded.get(key);
                if(guard){const request=object.get(key);request.onsuccess=()=>{let same=false;try{same=JSON.stringify(request.result)===guard.serialized;}catch(_){}if(request.result===undefined||same)object.put(value,key);else{draftConflicts++;const conflict=object.get(guard.key);conflict.onsuccess=()=>{if(conflict.result===undefined)object.put({key,current:request.result,incoming:value,createdAt:new Date().toISOString(),resolved:''},guard.key);};}};}
                else if(options.preserveDrafts&&key.startsWith('rescue-draft-conflict-')){const request=object.get(key);request.onsuccess=()=>{if(request.result===undefined)object.put(value,key);};}
                else value === undefined ? object.delete(key) : object.put(value, key);
            }
            tx.oncomplete = () => resolve({draftConflicts});
            tx.onerror = tx.onabort = () => reject(failure || tx.error || new Error('设备存储失败'));
        });
    }
    async function entries(prefix) {
        const db = await ready;
        return new Promise((resolve, reject) => {
            const tx = db.transaction('data'), values = [], request = tx.objectStore('data').openCursor(IDBKeyRange.bound(prefix, prefix + '\uffff'));
            request.onsuccess = () => { const cursor = request.result; if (cursor) { values.push([cursor.key, cursor.value]); cursor.continue(); } else resolve(values); };
            request.onerror = () => reject(request.error);
        });
    }
    async function key() {
        let value = await get('encryption-key');
        if (!value) {
            value = await crypto.subtle.generateKey({name: 'AES-GCM', length: 256}, false, ['encrypt', 'decrypt']);
            await put('encryption-key', value);
        }
        return value;
    }
    async function secret(name, value) {
        if (value === undefined) {
            const saved = await get('secret-' + name);
            if (!saved) return '';
            return new TextDecoder().decode(await crypto.subtle.decrypt({name: 'AES-GCM', iv: saved.iv}, await key(), saved.body));
        }
        if (!isSecureContext || !crypto.subtle) throw new Error('请在 HTTPS 手机网址中保存密钥');
        if (!value) { await put('secret-' + name, undefined); return ''; }
        const iv = crypto.getRandomValues(new Uint8Array(12));
        await put('secret-' + name, {iv, body: await crypto.subtle.encrypt({name: 'AES-GCM', iv}, await key(), new TextEncoder().encode(value))});
    }
    let host = YantuEnvironment.storage.getItem('yantu-pc-host') || '';
    function validate(value) {
        const url = new URL(value);
        if (url.username || url.password || url.search || url.hash || (url.protocol !== 'https:' && !(url.protocol === 'http:' && ['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)))) throw new Error('请填写 HTTPS 地址');
        return url;
    }
    async function request(path, options = {}) {
        if (YantuEnvironment.hosted) return YantuEnvironment.accountFetch(path, options);
        if (!host || !String(path).startsWith('/api/')) return fetch(path, options);
        const headers = new Headers(options.headers);
        const token = await secret('pc-token');
        if (token) headers.set('Authorization', 'Bearer ' + token);
        return fetch(host + path, {...options, headers, credentials: 'omit'});
    }
    async function connect(value, code) {
        if (YantuEnvironment.hosted) throw new Error('账号工作区已连接 NEBULA');
        const base = value.trim() ? validate(value.trim()).origin : location.origin;
        const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 15000);
        try {
            const response = await fetch(base + '/api/pair', {method: 'POST', credentials: base === location.origin ? 'same-origin' : 'omit', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({code}), signal: controller.signal});
            const result = await response.json();
            if (!response.ok) throw new Error(result.error || '连接失败');
            await secret('pc-token', result.deviceToken);
            host = base === location.origin ? '' : base;
            YantuEnvironment.storage.setItem('yantu-pc-host', host);
            window.dispatchEvent(new Event('pc-host-change'));
        } finally { clearTimeout(timer); }
    }
    let purgedResourceGeneration = '';
    async function purgeResources(policy) {
        const ids = policy?.fileIds;
        if (!Array.isArray(ids) || ids.length > 1000 || ids.some(id => typeof id !== 'string' || !/^[A-Za-z0-9_.-]{1,100}$/.test(id))) return;
        const generation = String(policy.generation || '') + ':' + ids.join('|');
        if (generation === purgedResourceGeneration) return;
        const exact = new Set(ids.flatMap(id => ['file-' + id, 'file-meta-' + id, 'viewer-position-' + id]));
        const prefixes = ids.flatMap(id => ['page-' + id + '-', 'reader-text-' + id + '-']);
        const db = await ready;
        await new Promise((resolve, reject) => {
            const tx = db.transaction('data', 'readwrite'), object = tx.objectStore('data'), request = object.openKeyCursor();
            request.onsuccess = () => { const cursor = request.result; if (!cursor) return; const key = cursor.key;
                if (typeof key === 'string' && (exact.has(key) || prefixes.some(prefix => key.startsWith(prefix)))) object.delete(key);
                cursor.continue(); };
            tx.oncomplete = () => { purgedResourceGeneration = generation; resolve(); }; tx.onerror = tx.onabort = () => reject(tx.error || new Error('旧资料缓存清理失败'));
        });
    }
    window.DeviceStore = {get, put, secret, batch, entries, purgeResources};
    window.PrepTransport = {fetch: request, connect, validate, base: () => host};
})();
