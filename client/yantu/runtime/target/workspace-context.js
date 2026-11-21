(() => {
    const mount = location.pathname.match(/^\/api\/study\/([0-9a-f]{32})\//);
    const scope = mount?.[1] || '';
    const prefix = scope ? `nebula-study:${scope}:` : '';
    const root = mount?.[0] || '/';
    const pathname = value => scope && String(value).startsWith('/') && !String(value).startsWith('//')
        ? root.slice(0, -1) + value : value;
    const storage = {
        getItem: key => localStorage.getItem(prefix + key),
        setItem: (key, value) => localStorage.setItem(prefix + key, value),
        removeItem: key => localStorage.removeItem(prefix + key),
    };
    const keys = () => Object.keys(localStorage).filter(key => key.startsWith(prefix)).map(key => key.slice(prefix.length));
    let session;
    function requireSession() {
        if (!session) {
            session = fetch('/api/study/session', { credentials: 'same-origin', cache: 'no-store' })
                .then(async response => {
                    if (!response.ok) throw new Error('请重新登录 NEBULA');
                    const current = await response.json();
                    if (current.scope !== scope) throw new Error('账号已切换，请重新打开研途');
                    return current;
                }).catch(error => { session = undefined; throw error; });
        }
        return session;
    }
    async function accountFetch(path, options = {}) {
        const headers = new Headers(options.headers);
        if (!/^(GET|HEAD)$/i.test(options.method || 'GET')) {
            headers.set('X-CSRF-Token', (await requireSession()).csrfToken);
        }
        const response = await fetch(pathname(path), { ...options, headers, credentials: 'same-origin' });
        if (response.status === 401 || response.headers.get('X-Study-Session') === 'changed') {
            window.parent.postMessage({ type: 'nebula-study-session-changed' }, location.origin);
        }
        return response;
    }
    window.YantuEnvironment = Object.freeze({ hosted: Boolean(scope), root, pathname, storage, keys,
        database: name => prefix + name, accountFetch });
})();
