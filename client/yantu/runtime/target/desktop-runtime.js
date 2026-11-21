(function (root) {
    'use strict';
    const states = Object.freeze({running: '运行中', stopped: '已停止', starting: '启动中',
        unowned: '归属不符', unavailable: '无法核验', active: '已连接', inactive: '未连接',
        verified: '备份成功', failed: '失败', unknown: '未知', exhausted: '恢复次数已用完',
        paused: '恢复已暂停', blocked: '操作已阻止', idle: '待命'});

    function summary(value) {
        const data = value && typeof value === 'object' ? value : {};
        const text = state => states[state] || states.unknown;
        return {local: data.local === true, server: text(data.server), phase: text(data.phase),
            remote: text(data.remote && data.remote.state), backup: text(data.backup && data.backup.state),
            secondary: text(data.secondary && data.secondary.state),
            recoveryEnabled: data.settings?.recoveryEnabled === true,
            loginStartupRequested: data.settings?.loginStartupRequested === true,
            // Requested startup is never presented as an installed OS setting.
            startup: data.settings?.loginStartupApplied === true ? '已启用' : data.settings?.loginStartupRequested === true ? '已请求，尚未安装' : '关闭',
            upgrade: data.upgrade?.prepared === true ? '备份已校验，尚未升级' : '未准备'};
    }

    function createClient(fetcher, location) {
        const local = location.protocol === 'http:' && ['127.0.0.1', 'localhost', '[::1]'].includes(location.hostname);
        return async function request(action, body) {
            if (!local) throw new Error('仅限电脑本机管理');
            const routes = {status: 'status', settings: 'settings', prepare: 'prepare-upgrade', cancel: 'cancel-upgrade'};
            if (!Object.hasOwn(routes, action)) throw new Error('不支持的电脑操作');
            const controller = new AbortController();
            const timer = setTimeout(() => controller.abort(), action === 'prepare' ? 300000 : 10000);
            try {
                const response = await fetcher('/api/desktop/' + routes[action], {
                    method: action === 'status' ? 'GET' : 'POST', credentials: 'same-origin',
                    cache: 'no-store', redirect: 'error', signal: controller.signal,
                    headers: {'Content-Type': 'application/json'},
                    ...(action === 'status' ? {} : {body: JSON.stringify(body || {})})});
                if (!response.ok) throw new Error(response.status === 403 ? '仅限电脑本机管理' : '操作未完成，请在电脑核对状态');
                const result = await response.json();
                if (action === 'status' && result.local !== true) throw new Error('未确认电脑本机权限');
                return result;
            } finally { clearTimeout(timer); }
        };
    }

    function mount(host, options = {}) {
        if (!host) throw new Error('Desktop status host is required');
        const request = options.request || createClient(root.fetch.bind(root), root.location);
        const confirm = options.confirm || root.confirm.bind(root);
        const doc = host.ownerDocument;
        let disposed = false, busy = false, authorized = false;
        const listeners = [];
        const element = (tag, text, className) => {
            const node = doc.createElement(tag);
            if (text) node.textContent = text;
            if (className) node.className = className;
            return node;
        };
        host.classList.add('desktop-runtime');
        host.hidden = true;
        host.replaceChildren();
        const heading = element('h3', '电脑运行');
        const grid = element('dl', '', 'desktop-runtime-status');
        const fields = {};
        for (const [key, label] of Object.entries({server: '后台', remote: '远程连接', backup: '学习备份', secondary: '第二副本', startup: '登录启动', upgrade: '升级保护'})) {
            grid.append(element('dt', label));
            fields[key] = element('dd', '未知'); grid.append(fields[key]);
        }
        const recoveryLabel = element('label', '', 'desktop-runtime-toggle');
        const recovery = element('input'); recovery.type = 'checkbox';
        recoveryLabel.append(recovery, element('span', '失败后有限重试'));
        const startupLabel = element('label', '', 'desktop-runtime-toggle');
        const startup = element('input'); startup.type = 'checkbox';
        startupLabel.append(startup, element('span', '登录后启动'));
        const tools = element('div', '', 'desktop-runtime-tools');
        const prepare = element('button', '准备升级备份'); prepare.type = 'button';
        const cancel = element('button', '取消升级准备'); cancel.type = 'button';
        tools.append(prepare, cancel);
        const message = element('p'); message.setAttribute('role', 'status'); message.setAttribute('aria-live', 'polite');
        host.append(heading, grid, recoveryLabel, startupLabel, tools, message);
        const controls = [recovery, startup, prepare, cancel];
        const listen = (node, event, callback) => {
            node.addEventListener(event, callback); listeners.push(() => node.removeEventListener(event, callback));
        };
        function display(result) {
            const view = summary(result);
            authorized = view.local;
            host.hidden = !authorized;
            if (!authorized) return;
            for (const key of Object.keys(fields)) fields[key].textContent = view[key];
            recovery.checked = view.recoveryEnabled; startup.checked = result.settings?.loginStartupApplied === true || view.loginStartupRequested;
            cancel.disabled = result.upgrade?.prepared !== true;
        }
        async function run(action, body) {
            if (disposed || busy || (action !== 'status' && !authorized)) return;
            busy = true; controls.forEach(node => { node.disabled = true; });
            let result;
            try {
                await request(action, body);
                result = await request('status');
                if (!disposed) { display(result); message.textContent = ''; }
            } catch (_) {
                if (!disposed) {
                    // A failed refresh must not leave old privileged controls actionable.
                    authorized = false;
                    controls.forEach(node => { node.disabled = true; });
                    message.textContent = '电脑状态未确认，操作已暂停';
                }
            } finally {
                busy = false;
                if (!disposed && authorized) {
                    controls.forEach(node => { node.disabled = false; });
                    cancel.disabled = result?.upgrade?.prepared !== true;
                }
            }
        }
        listen(recovery, 'change', () => run('settings', {recoveryEnabled: recovery.checked}));
        listen(startup, 'change', () => {
            const enabled = startup.checked;
            if (enabled && !confirm('启用登录后启动研途？这会为当前 Windows 账号添加研途启动项；可随时在此关闭。不安装系统服务，也不下载组件。')) {
                startup.checked = false; return;
            }
            run('settings', {loginStartupRequested: enabled, ...(enabled ? {confirmation: 'ENABLE_LOGIN_STARTUP'} : {})});
        });
        listen(prepare, 'click', () => {
            if (confirm('创建并校验学习备份？不会下载或执行升级。')) run('prepare', {confirmation: 'PREPARE_UPGRADE'});
        });
        listen(cancel, 'click', () => run('cancel'));
        const interval = setInterval(() => { if (!doc.hidden) run('status'); }, 15000);
        const ready = run('status');
        return {ready, refresh: () => run('status'), dispose() {
            disposed = true; clearInterval(interval); listeners.forEach(remove => remove());
            host.replaceChildren(); host.hidden = true;
        }};
    }

    const api = Object.freeze({summary, createClient, mount});
    if (typeof module === 'object' && module.exports) module.exports = api;
    else root.YantuDesktopRuntime = api;
})(typeof window === 'object' ? window : globalThis);
