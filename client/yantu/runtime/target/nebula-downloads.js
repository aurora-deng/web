(() => {
    const key = 'nebula-download-directory';
    const dialog = document.getElementById('download-settings');
    const status = dialog.querySelector('[role=status]');
    const button = dialog.querySelector('[data-pick-folder]');
    const available = typeof window.showDirectoryPicker === 'function';
    let handle;
    const loaded = DeviceStore.get(key).then(value => { handle = value; }).catch(() => {});
    const paint = () => {
        status.textContent = handle ? `当前目录：${handle.name}` : '使用浏览器的下载位置。';
        if (!available) status.textContent += ' 此浏览器使用自己的下载设置。';
    };
    const open = async () => {
        await loaded;
        paint();
        if (!dialog.open) dialog.showModal();
    };
    button.disabled = !available;
    button.onclick = async () => {
        try {
            handle = await window.showDirectoryPicker({id: 'nebula-yantu-download', startIn: 'downloads', mode: 'readwrite'});
            await DeviceStore.put(key, handle);
            paint();
        } catch (error) {
            if (error.name !== 'AbortError') status.textContent = '目录授权未完成，请重新选择。';
        }
    };
    dialog.querySelector('[data-clear-folder]').onclick = async () => {
        await DeviceStore.put(key, undefined);
        handle = undefined;
        paint();
    };
    document.querySelector('[data-open-download-settings]').onclick = open;
    const fromLink = () => { if (location.hash === '#download-settings') open(); };
    window.addEventListener('hashchange', fromLink);
    window.addEventListener('message', event => {
        if (YantuEnvironment.hosted && event.origin === location.origin && event.source === window.parent &&
            event.data?.type === 'nebula-study-download-settings') open();
    });
    fromLink();

    function browserDownload(name, url, revoke = false) {
        const link = document.createElement('a');
        link.href = url;
        link.download = name;
        document.body.append(link);
        link.click();
        link.remove();
        if (revoke) setTimeout(() => URL.revokeObjectURL(url), 30000);
    }
    async function writableFolder() {
        await loaded;
        const folder = handle;
        return folder && await folder.queryPermission({mode: 'readwrite'}) === 'granted' ? folder : null;
    }
    async function destination(folder, name) {
        const filename = name.replace(/[\\/:*?"<>|\x00-\x1f]/g, '_').replace(/[. ]+$/, '').slice(0, 160) || '资料';
        try {
            await folder.getFileHandle(filename);
        } catch (error) {
            if (error.name === 'NotFoundError') return folder.getFileHandle(filename, {create: true});
            throw error;
        }
        return folder.getFileHandle(`${crypto.randomUUID().slice(0, 8)}-${filename}`, {create: true});
    }
    let pending = Promise.resolve();
    function enqueue(task) {
        const result = pending.then(task);
        pending = result.catch(() => {});
        return result;
    }
    window.NebulaDownloads = {
        blob(name, blob) {
            return enqueue(async () => {
                try {
                    const folder = await writableFolder();
                    if (folder) {
                        const file = await destination(folder, name);
                        await blob.stream().pipeTo(await file.createWritable());
                        return;
                    }
                } catch (_) { toast('无法写入所选文件夹，改用浏览器下载。'); }
                browserDownload(name, URL.createObjectURL(blob), true);
            });
        },
        url(name, raw) {
            return enqueue(async () => {
                const url = new URL(raw, location.origin);
                if (url.origin !== location.origin) throw new Error('下载地址须属于当前研途实例');
                try {
                    const folder = await writableFolder();
                    if (folder) {
                        const response = await PrepTransport.fetch(url.pathname + url.search, {credentials: 'same-origin', cache: 'no-store'});
                        if (!response.ok || !response.body) throw new Error('下载未成功');
                        const file = await destination(folder, name);
                        await response.body.pipeTo(await file.createWritable());
                        return;
                    }
                } catch (_) { toast('无法写入所选文件夹，改用浏览器下载。'); }
                browserDownload(name, YantuEnvironment.pathname(url.pathname + url.search));
            });
        },
    };
})();
