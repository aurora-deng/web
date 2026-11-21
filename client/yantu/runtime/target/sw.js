const RECEIPTS = 'yantu-push-receipts-v1';
const SHELL = 'yantu-shell-nebula-v2';
// The standalone worker caches the standalone shell. Account pages have their own scoped storage.
const ASSETS = ['/mobile', '/manifest.webmanifest', '/sw.js', '/device-store.js', '/device-assistant.js', '/local-runtime.js', '/nebula-downloads.js', '/phone-runtime.js', '/mood-runtime.js', '/assistant-runtime.js', '/studio-runtime.js', '/study-library.js', '/gradebook-runtime.js', '/reliability.js', '/workspace.css', '/companion.css', '/studio.css', '/study-library.css', '/reliability.css', '/icons/icon-192.png', '/icons/icon-512.png', '/icons/study-pet.png', '/icons/mood-stationery.png', '/vendor/chart.umd.min.js', '/vendor/lz-string.min.js', '/vendor/email.min.js', '/vendor/css/all.min.css', '/vendor/webfonts/fa-solid-900.woff2', '/vendor/webfonts/fa-regular-400.woff2', '/vendor/webfonts/fa-brands-400.woff2'];
ASSETS.push('/workspace-context.js', '/study-flow.js', '/study-report.js', '/annotation-text.js', '/annotation-selection.js', '/media-preview.js', '/media-workspace.js', '/media-workspace.css');
ASSETS.push('/offline-safety.js', '/vendor/jszip-3.10.1/jszip.min.js');
ASSETS.push('/workspace-schema.js', '/workspace-core.js', '/workspace-upgrade.css');
ASSETS.push('/reading-workspace.js', '/vendor/pdfjs-5.6.205/build/pdf.min.mjs', '/vendor/pdfjs-5.6.205/build/pdf.worker.min.mjs');
ASSETS.push('/reading-import.js');
ASSETS.push('/resource-order.js');
ASSETS.push('/school-library.js','/school-library.css','/data/school-catalog.json');
ASSETS.push('/restore-ui.js');
ASSETS.push('/connection-diagnostics.js');
ASSETS.push('/rich-notes.js', '/vendor/marked-17.0.5/marked.umd.js', '/vendor/dompurify-3.4.17/purify.min.js', '/vendor/katex-0.19.0/dist/katex.min.js', '/vendor/katex-0.19.0/dist/katex.min.css', '/vendor/katex-0.19.0/dist/contrib/auto-render.min.js');
ASSETS.push(...['AMS-Regular','Caligraphic-Bold','Caligraphic-Regular','Fraktur-Bold','Fraktur-Regular','Main-Bold','Main-BoldItalic','Main-Italic','Main-Regular','Math-BoldItalic','Math-Italic','SansSerif-Bold','SansSerif-Italic','SansSerif-Regular','Script-Regular','Size1-Regular','Size2-Regular','Size3-Regular','Size4-Regular','Typewriter-Regular'].map(name=>'/vendor/katex-0.19.0/dist/fonts/KaTeX_'+name+'.woff2'));
ASSETS.push('/capture-inbox.js');
ASSETS.push('/workspace-search.js');
ASSETS.push('/review-cards.js', '/vendor/ts-fsrs-5.4.2/index.umd.js');
ASSETS.push('/knowledge-board.js', '/knowledge-board.css', '/vendor/cytoscape-3.34.3/cytoscape.min.js');
ASSETS.push('/practice-workspace.js', '/practice-workspace.css');
ASSETS.push('/storage-workspace.js', '/storage-workspace.css');
ASSETS.push('/planning-score-adapter.js', '/planning-workspace.js', '/planning-workspace.css');
ASSETS.push('/subject-tools.js', '/subject-tools.css', '/desktop-runtime.js', '/desktop-runtime.css');
ASSETS.push('/exam-workspace.js', '/exam-workspace.css');
ASSETS.push('/workbench-experience.js', '/workbench-experience.css');
ASSETS.push('/assistant-workspace.js', '/assistant-workspace.css', '/assistant-integration.js');
ASSETS.push('/resource-library.js');
ASSETS.push('/resource-backup.js');
ASSETS.push('/activity-classification.js', '/obsidian-bridge.js', '/obsidian-bridge.css');
self.addEventListener('install', event => event.waitUntil((async () => {
    const cache = await caches.open(SHELL);
    await Promise.all(ASSETS.filter(path => path !== '/sw.js').map(async path => {
        const response = await fetch(path, {cache: 'reload'});
        if (response.ok) await cache.put(path, response);
    }));
    await self.skipWaiting();
})()));
self.addEventListener('activate', event => event.waitUntil((async () => {
    for (const key of await caches.keys()) if (key.startsWith('yantu-shell-') && key !== SHELL) await caches.delete(key);
    await self.clients.claim();
})()));
self.addEventListener('fetch', event => {
    const url = new URL(event.request.url);
    if (event.request.method !== 'GET' || url.origin !== self.location.origin || url.pathname.startsWith('/api/')) return;
    if (event.request.mode === 'navigate') {
        event.respondWith((async () => {
            const cache = await caches.open(SHELL);
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000);
            try {
                const response = await fetch(event.request, {signal: controller.signal});
                if (!response.ok) throw new Error('PC unavailable');
                if (['/', '/index.html', '/mobile', '/mobile/'].includes(url.pathname) && response.headers.get('Content-Type')?.includes('text/html')) await cache.put('/mobile', response.clone());
                return response;
            } catch (_) {
                return (['/', '/index.html', '/mobile', '/mobile/'].includes(url.pathname) ? await cache.match('/mobile') : null) || new Response('<!doctype html><meta charset="UTF-8"><h2>电脑主机已下线</h2><p>请等待电脑重新上线。</p>', {headers: {'Content-Type': 'text/html; charset=utf-8'}});
            } finally { clearTimeout(timeout); }
        })());
    } else if (ASSETS.includes(url.pathname) || url.pathname.startsWith('/vendor/')) {
        event.respondWith((async () => {
            const cache = await caches.open(SHELL);
            try {
                const response = await fetch(event.request);
                if (response.ok) await cache.put(url.pathname, response.clone());
                return response;
            } catch (_) { return await cache.match(url.pathname) || Response.error(); }
        })());
    }
});

async function retryReceipts() {
    const cache = await caches.open(RECEIPTS);
    for (const request of await cache.keys()) {
        try {
            const payload = await (await cache.match(request)).json();
            const response = await fetch('/api/push/receipt', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload)});
            if (response.ok) await cache.delete(request);
        } catch (_) { break; }
    }
}

self.addEventListener('push', event => {
    event.waitUntil((async () => {
        let data = {};
        try { data = event.data?.json() || {}; } catch (_) { /* Always display a visible notification. */ }
        await self.registration.showNotification(data.title || '研途提醒', {
            body: data.body || '你有一条新的学习提醒',
            icon: '/icons/icon-192.png', badge: '/icons/icon-192.png',
            tag: data.tag || 'yantu-reminder', renotify: false,
            data: {url: '/mobile?view=notifications', eventId: data.eventId}
        });
        if (data.ack) {
            const cache = await caches.open(RECEIPTS);
            await cache.put(new Request(`${self.location.origin}/push-receipt/${data.ack}`), new Response(JSON.stringify({ack: data.ack}), {headers: {'Content-Type': 'application/json'}}));
            await retryReceipts();
        }
        for (const client of await self.clients.matchAll({type: 'window'})) client.postMessage({type: 'reminder'});
    })());
});

self.addEventListener('notificationclick', event => {
    event.notification.close();
    event.waitUntil((async () => {
        const windows = await self.clients.matchAll({type: 'window', includeUncontrolled: true});
        const url = self.location.origin + '/mobile?view=notifications';
        for (const client of windows) {
            if (new URL(client.url).origin === self.location.origin) {
                await client.navigate(url);
                return client.focus();
            }
        }
        return self.clients.openWindow(url);
    })());
});
self.addEventListener('message', event => {
    if (event.data?.type === 'retry-receipts') event.waitUntil(retryReceipts());
});
self.addEventListener('sync', event => {
    if (event.tag === 'push-receipts') event.waitUntil(retryReceipts());
});
