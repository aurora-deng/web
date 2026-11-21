(() => {
    const el = id => document.getElementById(id);
    const escape = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
    const mobile = /^\/mobile\/?$/.test(location.pathname);
    if (mobile) document.body.classList.add('phone-app');
    let registration, publicKey, subscription, deviceId, installPrompt, refreshing = false;
    let activeDevice = false, subscribing = false, lastFailure = null;
    function failureStatus() {
        let panel = el('pushLastError');
        if (!panel) {
            panel = document.createElement('p'); panel.id = 'pushLastError';
            panel.className = 'notification-status'; panel.setAttribute('role', 'alert');
            el('pushPermission').after(panel);
        }
        panel.hidden = !lastFailure;
        panel.textContent = lastFailure ? `${lastFailure.stage}失败：${lastFailure.message}` : '';
        panel.dataset.stage = lastFailure?.phase || '';
    }
    function keepFailure(phase, stage, error) {
        const message = `${error.name || 'Error'}: ${error.message || error}`.replace(/https?:\/\/\S+/gi, '[服务地址]').slice(0, 600);
        lastFailure = {phase, stage, message, time: Date.now()};
        failureStatus();
        DeviceStore.put('push-last-error', lastFailure).catch(() => {});
    }
    async function bounded(promise, milliseconds) {
        let timer;
        try {
            return await Promise.race([promise, new Promise((_, reject) => {
                timer = setTimeout(() => reject(new Error('浏览器推送服务在 45 秒内未响应')), milliseconds);
            })]);
        } finally { clearTimeout(timer); }
    }
    const states = {queued: '等待发送', accepted: '推送服务已接收', received: '浏览器已提交系统显示', failed: '发送失败', expired: '已过期', cancelled: '已停止'};
    const oldSetup = setupReminders;
    setupReminders = () => {
        clearInterval(window.reminderTimer);
        if (!/^https?:$/.test(location.protocol)) oldSetup();
    };
    sendNotification = async (title, body) => {
        if (registration && typeof Notification !== 'undefined' && Notification.permission === 'granted') await registration.showNotification(title, {body, icon: '/icons/icon-192.png'});
        else PrepLocal.toast(body);
    };

    async function api(path, body) {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 12000);
        let response;
        try { response = await PrepTransport.fetch(path, {credentials: 'same-origin', cache: 'no-store', signal: controller.signal, ...(body === undefined ? {} : {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)})}); }
        finally { clearTimeout(timeout); }
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || '电脑连接中断');
        return result;
    }

    function openView(view) { document.querySelector(`.nav-item[data-view="${view}"]`)?.click(); }
    const previousRender = renderView;
    renderView = view => {
        previousRender(view);
        if (view === 'notifications') { el('workspaceTitle').textContent = '提醒通知'; refresh(); }
        document.querySelectorAll('[data-mobile-view]').forEach(button => button.classList.toggle('active', button.dataset.mobileView === view));
    };

    function consentState() {
        const supported = isSecureContext && 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;
        el('pushSubscribe').disabled = subscribing || !supported || !registration || !publicKey;
        el('pushSubscribe').hidden = Boolean(subscription && activeDevice);
        el('pushUnsubscribe').hidden = !subscription;
        el('pushPermission').textContent = !isSecureContext ? '当前连接不是 HTTPS，手机系统通知不可用' : !supported ? '此浏览器不支持系统推送通知' : Notification.permission === 'denied' ? '通知权限已拒绝，请在浏览器站点权限中重新允许' : subscription && activeDevice ? '本机已订阅电脑提醒' : '本机尚未开启通知，已有其他设备不代表本机已登记';
        if (supported && Notification.permission === 'denied') el('pushSubscribe').disabled = true;
        failureStatus();
    }

    async function refresh() {
        if (refreshing || !/^https?:$/.test(location.protocol)) return;
        refreshing = true;
        try {
            const status = await api('/api/notifications');
            activeDevice = Boolean(status.devices.find(device => device.id === deviceId)?.active);
            publicKey = status.publicKey;
            el('pushScheduler').textContent = status.scheduler ? '电脑后台运行中' : '未运行';
            const settings = status.settings || {};
            const active = status.prepStatus === 'active' && settings.enabled;
            el('pushRoutine').textContent = active ? '已开启' : settings.enabled ? '备考暂停或未开始' : '已关闭';
            el('pushTimes').textContent = [settings.morning || '06:00', settings.noon || '12:00', settings.evening || '18:00'].join(' · ');
            el('pushDeviceCount').textContent = status.deviceCount;
            el('pushPCControls').hidden = !status.local;
            el('pushTestNow').disabled = el('pushTestLater').disabled = !status.deviceCount;
            if (status.local && !status.deviceCount) el('pushTestStatus').textContent = '尚无已授权的手机';
            el('pushDevices').innerHTML = status.devices.length ? status.devices.map(device => `<div class="notification-device"><div><strong>${escape(device.name)}</strong><p>${device.active ? '已订阅' : '已关闭'}${device.error ? ' · ' + escape(device.error) : ''}</p></div>${status.local && device.active ? `<button class="btn btn-icon btn-danger" title="移除设备" aria-label="移除设备" onclick="PrepPush.remove('${device.id}')"><i class="fas fa-trash"></i></button>` : ''}</div>`).join('') : '<div class="empty-state">暂无通知设备</div>';
            el('pushEvents').innerHTML = status.events.length ? status.events.map(event => `<article class="notification-event"><div class="notification-event-heading"><strong>${escape(event.title)}</strong><time>${escape(new Date(event.due * 1000).toLocaleString('zh-CN'))}</time></div><p>${escape(event.body)}</p><div class="notification-deliveries">${event.deliveries.length ? event.deliveries.map(delivery => `<span class="delivery-state ${delivery.status}">${escape(status.devices.find(d => d.id === delivery.deviceId)?.name || '设备')}：${states[delivery.status] || escape(delivery.status)}${delivery.error ? ' · ' + escape(delivery.error) : ''}</span>`).join('') : '<span>无订阅设备，仅保存电脑提醒记录</span>'}</div></article>`).join('') : '<div class="empty-state">暂无提醒记录</div>';
            el('pushEvents').querySelectorAll('.notification-event').forEach((article,index)=>{
                article.querySelectorAll('.delivery-state').forEach((span,deliveryIndex)=>{
                    const delivery=status.events[index].deliveries[deliveryIndex];
                    const times=[`尝试 ${delivery.attempts} 次`];
                    if(delivery.accepted)times.push('服务接收 '+PrepLocal.stampText(new Date(delivery.accepted*1000).toISOString()));
                    if(delivery.received)times.push('浏览器回执 '+PrepLocal.stampText(new Date(delivery.received*1000).toISOString())+' · 锁屏可见性需另行确认');
                    else if(delivery.accepted&&PrepLocal.nowDate().getTime()/1000-delivery.accepted>300)times.push('尚无手机显示回执');
                    const detail=document.createElement('small');detail.textContent=times.join(' · ');detail.className='delivery-timestamps';span.append(detail);
                });
            });
            consentState();
            if (status.error) el('pushPermission').textContent = status.error;
            const info = await api('/api/info');
            const url = info.remote?.active && info.remote.url ? info.remote.url + '/mobile' : null;
            el('phoneEntry').hidden = !url;
            if (url) {
                const link = el('phoneURL');
                if (link.href !== url) { link.href = url; link.textContent = url; el('phoneQR').src = '/api/mobile-qr?v=' + encodeURIComponent(url); }
                const apk = el('apkDownload');
                const base = PrepTransport.base() || '';
                if (apk && (!apk.dataset.ready || apk.dataset.base !== base)) {
                    try {
                        const result = await api(base + '/downloads/yantu_android_001.json');
                        if (result.signed && result.file === 'yantu_android_001.apk') {
                            apk.href = base + '/downloads/yantu_android_001.apk';
                            apk.title = `版本 ${result.version} · ${(result.bytes / 1024).toFixed(0)} KB`;
                            apk.hidden = false; apk.dataset.ready = 'true'; apk.dataset.base = base;
                        }
                    } catch (_) { apk.hidden = true; }
                }
            }
        } catch (error) { el('pushScheduler').textContent = '电脑连接中断'; }
        finally { refreshing = false; }
    }

    async function subscribe() {
        if (subscribing) return;
        subscribing = true;
        let phase = 'permission', stage = '通知授权';
        try {
            // Request consent in the click handler, before any network await.
            const permission = await Notification.requestPermission();
            if (permission !== 'granted') { keepFailure(phase, stage, new Error('未取得本机通知权限')); return; }
            el('pushSubscribe').disabled = true;
            phase = 'push-service'; stage = '浏览器推送服务注册';
            const bytes = Uint8Array.from(atob(publicKey.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - publicKey.length % 4) % 4)), c => c.charCodeAt(0));
            subscription = await bounded((async () => await registration.pushManager.getSubscription() || await registration.pushManager.subscribe({userVisibleOnly: true, applicationServerKey: bytes}))(), 45000);
            phase = 'pc-registration'; stage = '登记到电脑';
            const result = await api('/api/push/subscribe', {subscription: subscription.toJSON(), name: /Android/i.test(navigator.userAgent) ? '安卓手机' : '本机浏览器'});
            deviceId = result.id;
            activeDevice = true;
            YantuEnvironment.storage.setItem('yantu-push-device', deviceId);
            lastFailure = null;
            await DeviceStore.put('push-last-error', undefined).catch(() => {});
            PrepLocal.toast('已开启电脑提醒，请从电脑端发送测试');
        } catch (error) { keepFailure(phase, stage, error); PrepLocal.toast(stage + '失败，详情保留在本页'); }
        finally { subscribing = false; consentState(); refresh(); }
    }
    async function unsubscribe() {
        try {
            if (deviceId) await api('/api/push/unsubscribe', {id: deviceId});
            if (subscription) await subscription.unsubscribe();
            subscription = null;
            YantuEnvironment.storage.removeItem('yantu-push-device');
            PrepLocal.toast('本机通知已关闭');
        } catch (error) { PrepLocal.toast(error.message); }
        finally { consentState(); refresh(); }
    }
    async function remove(id) {
        if (!confirm('停止向这台设备发送提醒？')) return;
        try { await api('/api/push/unsubscribe', {id}); await refresh(); }
        catch (error) { PrepLocal.toast(error.message); }
    }
    async function test(delay) {
        try {
            const result = await api('/api/push/test', {delay});
            el('pushTestStatus').textContent = delay ? `${new Date(result.due).toLocaleTimeString('zh-CN')} 由电脑后台发送` : '已进入电脑发送队列';
            await refresh();
        } catch (error) { PrepLocal.toast(error.message); }
    }
    async function install() {
        if (!installPrompt) return;
        await installPrompt.prompt();
        installPrompt = null;
        el('pushInstall').hidden = true;
    }
    window.addEventListener('beforeinstallprompt', event => {
        event.preventDefault(); installPrompt = event; el('pushInstall').hidden = false;
    });
    window.addEventListener('appinstalled', () => { el('pushInstall').hidden = true; });

    const previousReady = PrepLocal.ready;
    PrepLocal.ready = () => {
        previousReady();
        DeviceStore.get('push-last-error').then(value => { lastFailure = value || null; failureStatus(); }).catch(() => {});
        deviceId = YantuEnvironment.storage.getItem('yantu-push-device');
        if (!YantuEnvironment.hosted && isSecureContext && 'serviceWorker' in navigator) {
            navigator.serviceWorker.register('/sw.js', {scope: '/'}).then(async () => {
                registration = await navigator.serviceWorker.ready;
                subscription = 'pushManager' in registration ? await registration.pushManager.getSubscription() : null;
                registration.active?.postMessage({type: 'retry-receipts'});
                consentState(); refresh();
            }).catch(error => { keepFailure('service-worker', '离线通知组件注册', error); });
            navigator.serviceWorker.addEventListener('message', event => { if (event.data?.type === 'reminder') refresh(); });
        }
        refresh();
        setInterval(() => { if (!document.hidden) refresh(); }, 10000);
        document.addEventListener('visibilitychange', () => { if (!document.hidden) { registration?.active?.postMessage({type: 'retry-receipts'}); refresh(); } });
        const view = new URLSearchParams(location.search).get('view');
        if (view === 'notifications') openView(view);
        else if (mobile) openView('overview');
    };
    window.PrepPush = {subscribe, unsubscribe, refresh, test, install, openView, remove};
})();
