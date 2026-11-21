(() => {
    const pages = new Map(), pending = new Map();
    const budget = 20 * 1024 * 1024;
    let bytes = 0, pairing;
    if (typeof addEventListener === 'function') addEventListener('resources-retired', event => {
        const ids = event.detail?.fileIds; if (!Array.isArray(ids)) return;
        for (const id of ids) {
            const prefix = JSON.stringify([id]).slice(0, -1) + ',';
            for (const [key, value] of pages) if (key.startsWith(prefix)) { bytes -= value.blob.size; pages.delete(key); }
            for (const [key, value] of pending) if (key.startsWith(prefix)) { value.controller.abort(); pending.delete(key); }
        }
    });
    const isPDF = file => file.type === 'application/pdf' || /\.pdf$/i.test(file.name || '');
    const version = file => JSON.stringify([file.id, file.sha256 || '', file.size || 0, file.lastModified || '']);
    const defaults = () => ({crop: [0, 0, 1, 1], rotation: 0, strokes: []});
    function remember(key, value) {
        if (pages.has(key)) bytes -= pages.get(key).blob.size;
        pages.delete(key);
        pages.set(key, value);
        bytes += value.blob.size;
        while (pages.size > 12 || bytes > budget) {
            const oldest = pages.keys().next().value;
            bytes -= pages.get(oldest).blob.size;
            pages.delete(oldest);
        }
        return value;
    }
    const aborted = () => Object.assign(new Error('页面读取已取消'), {name: 'AbortError'});
    function check(signal) { if (signal?.aborted) throw aborted(); }
    const transient = error => error?.name === 'TypeError' || error?.name === 'TimeoutError' || [408, 429, 500, 502, 503, 504].includes(error?.status);
    async function pause(signal) {
        check(signal);
        await new Promise((resolve, reject) => {
            const cancel = () => { clearTimeout(timer); reject(aborted()); };
            const timer = setTimeout(() => { signal?.removeEventListener('abort', cancel); resolve(); }, 180);
            signal?.addEventListener('abort', cancel, {once: true});
        });
    }
    async function fetchPage(path, options) {
        let result = await PrepTransport.fetch(path, options);
        if (result.status === 401 && !YantuEnvironment.hosted && !PrepTransport.base?.() && typeof location !== 'undefined' && ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname)) {
            // Only renew the existing local owner's session, never bypass remote pairing.
            if (!pairing) pairing = (async () => {
                const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 8000);
                try {
                    const response = await PrepTransport.fetch('/api/pair', {method: 'POST', credentials: 'same-origin', headers: {'Content-Type': 'application/json'}, body: '{}', signal: controller.signal});
                    if (!response.ok) throw Object.assign(new Error('电脑连接授权已失效，请重新连接'), {status: 401});
                } catch (error) {
                    if (controller.signal.aborted) throw Object.assign(new Error('电脑连接恢复超时，请重试'), {name: 'TimeoutError'}); throw error;
                } finally { clearTimeout(timer); }
            })().finally(() => { pairing = null; });
            await pairing; check(options.signal); result = await PrepTransport.fetch(path, options);
        }
        return result;
    }
    async function serverPage(file, number, signal) {
        const controller = new AbortController(); let timedOut = false;
        const timer = setTimeout(() => { timedOut = true; controller.abort(); }, 20000);
        const cancel = () => controller.abort();
        signal?.addEventListener('abort', cancel, {once: true});
        try {
            check(signal);
            const response = await fetchPage('/api/media/page?' + new URLSearchParams({file: file.id, page: number}), {credentials: 'same-origin', cache: 'no-store', signal: controller.signal});
            if (!response.ok) throw Object.assign(new Error((await response.json().catch(() => ({}))).error || '页面读取失败'), {status: response.status});
            return {pages: isPDF(file) ? Number(response.headers.get('X-Pages')) || 1 : 1, blob: await response.blob()};
        } catch (error) {
            check(signal); if (timedOut) throw Object.assign(new Error('PDF 页面读取超时，请重试'), {name: 'TimeoutError'}); throw error;
        } finally { clearTimeout(timer); signal?.removeEventListener('abort', cancel); }
    }
    async function pdfPage(file, number, signal, browserOnly = false) {
        let failure;
        if (!browserOnly) for (let attempt = 0; attempt < 2; attempt++) {
            try { return await serverPage(file, number, signal); }
            catch (error) { check(signal); failure = error; if (error.name === 'AbortError') throw error; if (error.name === 'TimeoutError' || !transient(error) || attempt) break; await pause(signal); }
        }
        if (window.ReaderWorkspace?.previewPage && (!failure || ![401,403,404].includes(failure.status))) {
            try { return await window.ReaderWorkspace.previewPage(file, number, {signal}); }
            catch (error) { check(signal); if (browserOnly || error.name === 'PasswordException') throw error; }
        }
        throw failure || new Error('PDF 暂时无法读取，请连接电脑后重试');
    }
    async function invalidate(file, number = 1) {
        const key = version(file) + ':' + number, hit = pages.get(key);
        if (hit) { bytes -= hit.blob.size; pages.delete(key); }
        await DeviceStore.put(`page-${file.id}-${number}`, undefined).catch(() => {});
    }
    function subscribe(entry, signal) {
        entry.users++;
        return new Promise((resolve, reject) => {
            let finished = false;
            const done = (callback, value) => { if (finished) return; finished = true; entry.users--; signal?.removeEventListener('abort', cancel); callback(value); };
            const cancel = () => { done(reject, aborted()); if (!entry.users && !entry.complete) entry.controller.abort(); };
            entry.promise.then(value => done(resolve, value), error => done(reject, error));
            signal?.addEventListener('abort', cancel, {once: true});
            if (signal?.aborted) cancel();
        });
    }
    async function read(file, number = 1, options = {}) {
        check(options.signal);
        if (!Number.isInteger(number) || number < 1) throw new Error('页码无效');
        const sourceVersion = version(file), key = sourceVersion + ':' + number;
        if (pages.has(key)) { const hit = pages.get(key); pages.delete(key); pages.set(key, hit); return hit; }
        if (pending.has(key) && !pending.get(key).controller.signal.aborted) return subscribe(pending.get(key), options.signal);
        const entry = {controller: new AbortController(), users: 0, complete: false};
        entry.promise = (async () => { try {
            const signal = entry.controller.signal;
            const storeKey = `page-${file.id}-${number}`;
            const cached = await DeviceStore.get(storeKey).catch(() => null);
            check(signal);
            if (cached?.blob && (cached.sourceVersion === sourceVersion ||
                (cached.canonical && file.sha256 && cached.fingerprint === file.sha256) ||
                (cached.canonical && !file.sha256 && cached.fileId === file.id && file.size > 0 && cached.sourceSize === file.size))) {
                return remember(key, cached);
            }
            let blob, count = 1, derived = isPDF(file);
            if (isPDF(file)) {
                ({blob, pages: count} = await pdfPage(file, number, signal));
            } else {
                blob = await DeviceStore.get('file-' + file.id).catch(() => null);
                if (!blob && file.size > 2 * 1024 * 1024 && /^image\/(jpeg|png|webp|bmp)$/.test(file.type)) {
                    try { ({blob} = await serverPage(file, 1, signal)); derived = true; } catch (error) { check(signal); /* The original remains a fallback. */ }
                }
                if (!blob) blob = await getFile(file.id);
            }
            check(signal);
            if (!blob) throw new Error('附件尚未缓存，需要连接电脑');
            const value = {blob, pages: count, sourceVersion, fingerprint: file.sha256 || '', canonical: true};
            if (derived) DeviceStore.put(storeKey, value).catch(() => {});
            return remember(key, value);
        } finally { entry.complete = true; if (pending.get(key) === entry) pending.delete(key); } })();
        pending.set(key, entry);
        return subscribe(entry, options.signal);
    }
    async function image(file, number = 1, options = {}) {
        let value = await read(file, number, options), bitmap;
        try { bitmap = await createImageBitmap(value.blob); }
        catch (error) {
            check(options.signal); await invalidate(file, number); check(options.signal);
            value = await read(file, number, options);
            try { bitmap = await createImageBitmap(value.blob); }
            catch (failure) {
                check(options.signal); if (!isPDF(file) || !window.ReaderWorkspace?.previewPage) throw failure;
                value = await pdfPage(file, number, options.signal, true); bitmap = await createImageBitmap(value.blob);
                remember(version(file) + ':' + number, {...value, sourceVersion: version(file), fingerprint: file.sha256 || '', canonical: true});
            }
        }
        if (options.signal?.aborted) { bitmap.close(); throw aborted(); }
        const scale = Math.min(1, 2200 / Math.max(bitmap.width, bitmap.height));
        if (scale < 1) {
            const original = bitmap;
            try { bitmap = await createImageBitmap(original, {resizeWidth: Math.round(original.width * scale), resizeHeight: Math.round(original.height * scale), resizeQuality: 'high'}); }
            finally { original.close(); }
        }
        if (options.signal?.aborted) { bitmap.close(); throw aborted(); }
        return {image: bitmap, pages: value.pages};
    }
    function marks(records, file, page) {
        return (records || []).filter(mark => mark.fileId === file.id && mark.page === page &&
            (!file.sha256 || !mark.fingerprint || mark.fingerprint === file.sha256));
    }
    function drawMarks(ctx, records, width, height) {
        for (const mark of records || []) {
            if (mark.kind === 'bookmark') continue;
            ctx.save(); ctx.strokeStyle = mark.color || '#d8465b'; ctx.fillStyle = mark.color || '#e0bb20';
            ctx.lineWidth = (mark.width || .003) * Math.min(width, height); ctx.lineJoin = ctx.lineCap = 'round';
            if (mark.kind === 'ink' && mark.points?.length) {
                const points = mark.points.map(([x, y]) => [x * width, y * height]); ctx.beginPath();
                if (points.length === 1) { ctx.arc(...points[0], ctx.lineWidth / 2, 0, Math.PI * 2); ctx.fillStyle = ctx.strokeStyle; ctx.fill(); }
                else { ctx.moveTo(...points[0]); for (const point of points.slice(1)) ctx.lineTo(...point); ctx.stroke(); }
            } else if (mark.kind === 'highlight') {
                ctx.globalAlpha = .25; for (const [x, y, w, h] of mark.regions || []) ctx.fillRect(x * width, y * height, w * width, h * height);
            }
            ctx.restore();
        }
    }
    function paint(canvas, image, edit = defaults(), readingMarks = []) {
        const [x, y, w, h] = edit.crop || [0, 0, 1, 1], rotation = edit.rotation || 0;
        const sx = x * image.width, sy = y * image.height, sw = w * image.width, sh = h * image.height;
        const width = Math.max(1, Math.round(rotation % 180 ? sh : sw)), height = Math.max(1, Math.round(rotation % 180 ? sw : sh));
        if (canvas.width !== width) canvas.width = width;
        if (canvas.height !== height) canvas.height = height;
        const ctx = canvas.getContext('2d');
        ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.globalAlpha = 1;
        ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.save(); ctx.translate(canvas.width / 2, canvas.height / 2); ctx.rotate(rotation * Math.PI / 180); ctx.translate(-sw / 2, -sh / 2);
        ctx.drawImage(image, sx, sy, sw, sh, 0, 0, sw, sh); ctx.translate(-sx, -sy);
        ctx.save(); ctx.beginPath(); ctx.rect(sx, sy, sw, sh); ctx.clip();
        drawMarks(ctx, readingMarks, image.width, image.height);
        for (const stroke of edit.strokes || []) {
            const points = (stroke.points || []).map(([px, py]) => [px * image.width, py * image.height]);
            ctx.save(); ctx.strokeStyle = ctx.fillStyle = stroke.color; ctx.globalAlpha = stroke.tool === 'highlight' ? .28 : 1;
            ctx.lineWidth = stroke.width * Math.min(image.width, image.height); ctx.lineJoin = ctx.lineCap = 'round'; ctx.beginPath();
            if (stroke.tool === 'text' && points.length && typeof stroke.text === 'string') {
                if (window.AnnotationText?.paint(ctx, stroke, image.width, image.height)) { ctx.restore(); continue; }
                const size = Math.max(.008, Math.min(.12, Number(stroke.fontSize) || .025)) * image.width;
                ctx.font = `${size}px "Microsoft YaHei", "PingFang SC", sans-serif`; ctx.textBaseline = 'top';
                const [left, top] = points[0], available = Math.max(size, image.width - left);
                let row = 0;
                for (const paragraph of stroke.text.slice(0, 2000).replace(/\r\n?/g, '\n').split('\n')) {
                    let line = '';
                    for (const character of paragraph) {
                        if (line && ctx.measureText(line + character).width > available) {
                            ctx.fillText(line, left, top + row++ * size * 1.3); line = '';
                        }
                        line += character;
                    }
                    ctx.fillText(line, left, top + row++ * size * 1.3);
                }
            } else if (stroke.tool === 'circle' && points.length > 1) {
                const [a, b] = [points[0], points.at(-1)]; ctx.ellipse((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, Math.max(.5, Math.abs(a[0] - b[0]) / 2), Math.max(.5, Math.abs(a[1] - b[1]) / 2), 0, 0, Math.PI * 2); ctx.stroke();
            } else if (points.length === 1) { ctx.arc(points[0][0], points[0][1], ctx.lineWidth / 2, 0, Math.PI * 2); ctx.fill(); }
            else if (points.length) { ctx.moveTo(...points[0]); for (const point of points.slice(1)) ctx.lineTo(...point); ctx.stroke(); }
            ctx.restore();
        }
        ctx.restore(); ctx.restore();
        return canvas;
    }
    window.MediaPreview = {read, image, paint, defaults, version, marks, invalidate};
})();
