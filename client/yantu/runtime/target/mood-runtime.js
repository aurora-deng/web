(() => {
    const el = id => document.getElementById(id);
    const safe = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
    let currentWall = null, draftWall = null, drag = null, formFiles = [], formWall = null, uploadedKeys = new Set();
    const photoURLs = new Map();
    async function hydratePhotos() {
        for (const image of el('moodStage').querySelectorAll('.sticky-photo img')) {
            const id = new URL(image.src, location.href).pathname.split('/').pop();
            try {
                if (!photoURLs.has(id)) {
                    const blob = await DeviceStore.get('file-' + id) || await getFile(id);
                    photoURLs.set(id, URL.createObjectURL(blob));
                }
                if (image.isConnected) image.src = photoURLs.get(id);
            } catch (_) { image.alt = '照片尚未同步，等待电脑连接'; }
        }
        const active = new Set(notes().flatMap(note => (note.files || []).map(file => file.id)));
        for (const [id, url] of photoURLs) if (!active.has(id)) { URL.revokeObjectURL(url); photoURLs.delete(id); }
    }
    const stamp = () => PrepLocal.nowDate().toISOString();
    const displayTime = value => value ? PrepLocal.stampText(value) : '';
    const walls = () => state.moodWalls ||= [];
    const notes = () => state.moodNotes ||= [];
    const pageNotes = id => notes().filter(note => note.wallId === id);
    function wallSurface(id = currentWall) {
        const value = (walls().find(wall => wall.id === id) || draftWall)?.surface;
        return ['paper', 'grid', 'felt'].includes(value) ? value : 'paper';
    }
    function makeWall() { return {id: uid(), title: '心情墙', createdAt: stamp(), surface: wallSurface()}; }
    function prune() { state.moodWalls = walls().filter(wall => pageNotes(wall.id).length); }
    function selected() {
        if (currentWall && (walls().some(wall => wall.id === currentWall) || draftWall?.id === currentWall)) return currentWall;
        currentWall = walls()[0]?.id;
        if (!currentWall) { draftWall ||= makeWall(); currentWall = draftWall.id; }
        return currentWall;
    }
    function freeWall(preferred) {
        if (pageNotes(preferred).filter(note => note.id !== drag?.id).length < 30) return preferred;
        draftWall = makeWall();
        return draftWall.id;
    }
    function ensureWall(id) {
        if (!walls().some(wall => wall.id === id)) walls().push(draftWall?.id === id ? {...draftWall} : {id, title: '心情墙', createdAt: stamp()});
    }
    function dimensions() {
        const stageWidth = el('moodStage').clientWidth;
        const mobile = stageWidth < 560;
        const width = mobile ? Math.min(154, Math.max(110, Math.floor((stageWidth - 38) / 2))) : 218;
        return {width, height: mobile ? 236 : 274, columns: mobile ? 2 : Math.max(2, Math.floor((stageWidth - 20) / (width + 28)))};
    }
    function initialPosition(index) {
        const {columns} = dimensions();
        const rows = Math.ceil(30 / columns);
        return {x: columns === 1 ? .1 : (index % columns) / (columns - 1) * .92 + .03, y: Math.floor(index / columns) / Math.max(1, rows - 1) * .94 + .02};
    }
    function render() {
        if (drag?.active && !drag.switching) return;
        const id = selected();
        const wallNotes = pageNotes(id).filter(note => !drag?.active || note.id !== drag.id);
        const dim = dimensions();
        const rows = Math.ceil(30 / dim.columns);
        const stage = el('moodStage');
        const fullHeight = Math.max(670, rows * (dim.height + 36) + 50);
        stage.style.height = `${wallNotes.length || drag?.active ? fullHeight : el('moodViewport').clientHeight}px`;
        stage.style.setProperty('--sticky-width', `${dim.width}px`);
        stage.style.setProperty('--sticky-height', `${dim.height}px`);
        el('moodBoard').dataset.surface = wallSurface(id);
        el('moodBoard').querySelectorAll('.mood-surface-swatch').forEach(button => button.setAttribute('aria-pressed', String(button.dataset.surface === wallSurface(id))));
        el('moodCount').textContent = `${wallNotes.length}${drag?.active && drag.target === id ? ' + 1' : ''} / 30`;
        const pages = walls();
        el('moodTabs').innerHTML = pages.map((wall, index) => `<button class="mood-page-tab ${wall.id === id ? 'active' : ''}" data-wall-id="${wall.id}" onclick="MoodWall.select('${wall.id}')" title="第 ${index + 1} 面墙"><i class="fas fa-note-sticky"></i><span>${safe(wall.title || '心情墙')} ${index + 1}</span></button>`).join('') + (draftWall?.id === id && !pages.some(w => w.id === id) ? '<span class="mood-page-draft">新墙</span>' : '') + '<button class="btn btn-icon btn-ghost" onclick="MoodWall.newWall()" title="新的一面墙" aria-label="新的一面墙"><i class="fas fa-plus"></i></button>';
        const pageIndex = pages.findIndex(wall => wall.id === id);
        el('moodWallLabel').textContent = `第 ${String(pageIndex < 0 ? pages.length + 1 : pageIndex + 1).padStart(2, '0')} 面`;
        const photoCount = wallNotes.reduce((count, note) => count + (note.files || []).length, 0);
        el('moodPhotoCount').textContent = photoCount ? `${photoCount} 张照片` : '';
        el('moodPrevious').disabled = pageIndex === 0 || !pages.length;
        el('moodNext').disabled = false;
        stage.innerHTML = wallNotes.length ? wallNotes.map((note, index) => {
            const files = note.files || [];
            const position = note.autoLayout ? initialPosition(index) : {x: Math.min(1, Math.max(0, Number(note.x) || 0)), y: Math.min(1, Math.max(0, Number(note.y) || 0))};
            // Rotate the paper, not the drag hitbox, so pointer coordinates stay stable.
            return `<article class="sticky-note ${safe(note.color || 'lemon')} ${files.length > 1 ? 'has-photos' : ''}" data-note-id="${note.id}" style="left:calc((100% - var(--sticky-width) - 20px) * ${position.x} + 10px);top:calc((100% - var(--sticky-height) - 20px) * ${position.y} + 10px);z-index:${Math.min(10000, Number(note.z) || 1)}"><div class="sticky-paper"><div class="sticky-top"><span class="sticky-grip" title="拖动便签" aria-label="拖动便签"><i class="fas fa-grip"></i></span><div><button class="sticky-icon" onclick="MoodWall.create('${note.id}')" title="编辑便签" aria-label="编辑便签"><i class="fas fa-pen"></i></button><button class="sticky-icon" onclick="MoodWall.remove('${note.id}')" title="删除便签" aria-label="删除便签"><i class="fas fa-xmark"></i></button></div></div>${files.length ? `<button class="sticky-photo" onclick="openViewer('${note.id}',0)" title="打开照片"><img src="/api/files/${encodeURIComponent(files[0].id)}" alt="${safe(files[0].name)}" loading="lazy" draggable="false" />${files.length > 1 ? `<span><i class="fas fa-layer-group" aria-hidden="true"></i> ${files.length} 张</span>` : ''}</button><time class="sticky-photo-time" title="上传时间">上传 ${safe(displayTime(files[0].uploadedAt || note.createdAt))}</time>` : ''}<button class="sticky-content" onclick="MoodWall.create('${note.id}')">${safe(note.text || '')}</button><time class="sticky-time" title="创建时间 ${safe(note.createdAt)}">${safe(displayTime(note.createdAt))}</time></div></article>`;
        }).join('') : drag?.active ? '<div class="mood-empty is-drop-target">松手贴在这面墙</div>' : '<div class="mood-empty"><img src="icons/mood-stationery.png" alt="" width="300" height="210" draggable="false" /><h4>今天的心情，还没有落笔</h4><button class="btn" onclick="MoodWall.create()"><i class="fas fa-pen" aria-hidden="true"></i> 写下此刻</button></div>';
        hydratePhotos();
    }
    async function surface(value) {
        if (!['paper', 'grid', 'felt'].includes(value) || drag) return;
        const id = selected(), wall = walls().find(item => item.id === id) || draftWall;
        wall.surface = value;
        render();
        if (walls().some(item => item.id === id)) { saveState(); await PrepLocal.flush(); }
    }
    function select(id) {
        const changed = currentWall !== id;
        currentWall = id;
        if (drag?.active) { drag.target = freeWall(id); currentWall = drag.target; drag.switching = true; }
        render();
        if (changed) el('moodViewport').scrollTop = 0;
        if (drag) drag.switching = false;
    }
    function newWall() { draftWall = makeWall(); select(draftWall.id); }
    function reveal(id) {
        const note = el('moodStage').querySelector(`[data-note-id="${id}"]`), viewport = el('moodViewport');
        if (!note) return;
        const top = note.offsetTop, bottom = top + note.offsetHeight + 12;
        if (top < viewport.scrollTop || bottom > viewport.scrollTop + viewport.clientHeight) viewport.scrollTop = Math.max(0, top - 28);
    }
    function page(direction) {
        const found = walls().findIndex(wall => wall.id === selected());
        const index = found < 0 ? walls().length : found;
        const next = walls()[index + direction];
        if (next) select(next.id);
        else if (direction > 0 && found >= 0) newWall();
    }
    function photoList() {
        el('moodPhotoList').innerHTML = formFiles.map(file => `<div class="mood-photo-row"><span>${safe(file.name)}<small>${safe(displayTime(file.uploadedAt))}</small></span><button type="button" class="btn btn-icon btn-danger" onclick="MoodWall.removePhoto('${file.id}')" title="移除照片" aria-label="移除照片"><i class="fas fa-trash"></i></button></div>`).join('');
    }
    function removePhoto(id) { formFiles = formFiles.filter(file => file.id !== id); photoList(); }
    function create(id) {
        const form = el('moodForm');
        form.reset();
        const old = notes().find(note => note.id === id);
        const createdAt = old?.createdAt || stamp();
        form.elements.id.value = old?.id || '';
        form.elements.createdAt.value = createdAt;
        form.elements.text.value = old?.text || '';
        form.elements.color.value = old?.color || ['lemon', 'rose', 'mint', 'sky', 'lilac'][notes().length % 5];
        formFiles = [...(old?.files || [])];
        uploadedKeys = new Set();
        formWall = old?.wallId || selected();
        el('moodCreatedTime').textContent = displayTime(createdAt);
        el('moodDialogTitle').textContent = old ? '编辑便签' : '写一张便签';
        photoList();
        el('moodDialog').showModal();
    }
    async function save(event) {
        event.preventDefault();
        const form = event.target;
        const fd = new FormData(form);
        const photos = fd.getAll('photos').filter(file => file.name);
        if (!fd.get('text').trim() && !photos.length && !formFiles.length) { PrepLocal.toast('写一点心情，或放一张照片'); return; }
        const submit = form.querySelector('[type=submit]');
        submit.disabled = true;
        try {
            for (const file of photos) {
                if (!file.type.startsWith('image/')) throw new Error('心情便签只接受照片');
                const key = `${file.name}:${file.size}:${file.lastModified}`;
                if (uploadedKeys.has(key)) continue;
                const id = uid();
                const response = await saveFile(id, file, file.name);
                const metadata = response?.json ? await response.json() : {};
                formFiles.push({id, name: file.name, type: file.type, size: file.size, uploadedAt: metadata.uploadedAt || stamp()});
                uploadedKeys.add(key);
                photoList();
            }
            form.elements.photos.value = '';
            photoList();
            const old = notes().find(note => note.id === fd.get('id'));
            const wallId = old?.wallId || freeWall(formWall);
            ensureWall(wallId);
            const note = {...(old || {...initialPosition(pageNotes(wallId).length), autoLayout: true}), id: old?.id || uid(), wallId, color: fd.get('color'), text: fd.get('text').trim(), files: [...formFiles], createdAt: fd.get('createdAt'), z: Math.max(0, ...notes().map(n => Number(n.z) || 0)) + 1};
            const index = notes().findIndex(n => n.id === note.id);
            if (index < 0) notes().push(note); else state.moodNotes[index] = note;
            currentWall = wallId;
            prune(); saveState();
            await PrepLocal.flush();
            currentWall = notes().find(item => item.id === note.id)?.wallId || wallId;
            if (!PrepLocal.hasPending()) for (const file of old?.files || []) if (!formFiles.some(f => f.id === file.id)) await deleteFile(file.id).catch(() => {});
            el('moodDialog').close(); render();
            if (!old) reveal(note.id);
            PrepLocal.toast(PrepLocal.hasPending() ? '便签已保留，等待电脑连接' : '已贴上心情墙');
        } catch (error) { PrepLocal.toast(error.message); }
        finally { submit.disabled = false; }
    }
    async function remove(id) {
        if (!confirm('删除这张便签和照片？')) return;
        const old = notes().find(note => note.id === id);
        state.moodNotes = notes().filter(note => note.id !== id);
        prune(); saveState(); await PrepLocal.flush();
        if (!PrepLocal.hasPending()) for (const file of old?.files || []) await deleteFile(file.id).catch(() => {});
        currentWall = null; draftWall = null; render();
        el('moodViewport').scrollTop = 0;
    }
    function pointerDown(event) {
        if (event.button !== 0 || event.target.closest('button') || drag) return;
        const element = event.target.closest('[data-note-id]');
        if (!element) return;
        const rect = element.getBoundingClientRect();
        drag = {id: element.dataset.noteId, origin: selected(), target: selected(), startX: event.clientX, startY: event.clientY, grabX: event.clientX - rect.left, grabY: event.clientY - rect.top, width: rect.width, height: rect.height, active: false, ghost: element.cloneNode(true), edgeAt: 0, edge: 0, pointerId: event.pointerId};
        el('moodStage').setPointerCapture(event.pointerId);
    }
    function pointerMove(event) {
        if (!drag || event.pointerId !== drag.pointerId) return;
        if (!drag.active && Math.hypot(event.clientX - drag.startX, event.clientY - drag.startY) < 5) return;
        if (!drag.active) {
            drag.active = true;
            drag.ghost.classList.add('sticky-drag-ghost');
            drag.ghost.style.width = `${drag.width}px`;
            drag.ghost.style.height = `${drag.height}px`;
            drag.ghost.removeAttribute('data-note-id');
            document.body.append(drag.ghost);
            el('moodStage').querySelector(`[data-note-id="${drag.id}"]`)?.classList.add('sticky-drag-hidden');
            document.body.classList.add('dragging-sticky');
            drag.edgeTimer = setInterval(() => {
                if (drag?.edge && performance.now() - drag.edgeAt > 650) { page(drag.edge); drag.edgeAt = performance.now(); }
                if (drag?.scroll) el('moodViewport').scrollBy(0, drag.scroll);
            }, 150);
        }
        event.preventDefault();
        drag.ghost.style.left = `${event.clientX - drag.grabX}px`;
        drag.ghost.style.top = `${event.clientY - drag.grabY}px`;
        drag.lastX = event.clientX; drag.lastY = event.clientY;
        const rect = el('moodStage').getBoundingClientRect();
        const edge = event.clientX <= rect.left + 26 ? -1 : event.clientX >= rect.right - 26 ? 1 : 0;
        if (edge !== drag.edge) { drag.edge = edge; drag.edgeAt = performance.now(); }
        if (edge && performance.now() - drag.edgeAt > 650) { page(edge); drag.edgeAt = performance.now(); }
        el('moodStage').classList.toggle('edge-left', edge === -1);
        el('moodStage').classList.toggle('edge-right', edge === 1);
        el('moodDragStatus').textContent = edge ? (edge === 1 ? '下一面墙' : '上一面墙') : '';
        const viewport = el('moodViewport').getBoundingClientRect();
        drag.scroll = event.clientY > Math.min(viewport.bottom, innerHeight - 45) - 35 ? 24 : event.clientY < Math.max(viewport.top, 85) + 35 ? -24 : 0;
        if (event.clientY > innerHeight - 45 && viewport.bottom > innerHeight - 45) window.scrollBy(0, 16);
        else if (event.clientY < 85 && viewport.top < 85) window.scrollBy(0, -16);
    }
    async function pointerUp(event, cancel = false) {
        if (!drag || (event && event.pointerId !== drag.pointerId)) return;
        const active = drag;
        if (active.active && !cancel) {
            const note = notes().find(n => n.id === active.id);
            const rect = el('moodStage').getBoundingClientRect();
            const dim = dimensions();
            if (note) {
                note.wallId = active.target;
                note.autoLayout = false;
                ensureWall(note.wallId);
                note.x = Math.max(0, Math.min(1, (active.lastX - rect.left - active.grabX - 10) / Math.max(1, rect.width - dim.width - 20)));
                note.y = Math.max(0, Math.min(1, (active.lastY - rect.top - active.grabY - 10) / Math.max(1, rect.height - dim.height - 20)));
                note.z = Math.max(0, ...notes().map(n => Number(n.z) || 0)) + 1;
                currentWall = note.wallId;
                prune(); saveState();
            }
        } else if (cancel) currentWall = active.origin;
        clearInterval(active.edgeTimer);
        active.ghost.remove();
        drag = null;
        document.body.classList.remove('dragging-sticky');
        el('moodStage').classList.remove('edge-left', 'edge-right');
        el('moodDragStatus').textContent = '';
        render();
        if (active.active && !cancel) { await PrepLocal.flush(); currentWall = notes().find(note => note.id === active.id)?.wallId || currentWall; render(); }
    }
    const previousRender = renderView;
    renderView = view => { previousRender(view); if (view === 'mood') { el('workspaceTitle').textContent = '心情墙'; render(); } };
    const previousEdit = editRecord;
    editRecord = id => notes().some(note => note.id === id) ? create(id) : previousEdit(id);
    const previousDelete = deleteItem;
    deleteItem = id => notes().some(note => note.id === id) ? remove(id) : previousDelete(id);
    const previousRefresh = refreshRecords;
    refreshRecords = col => col === 'moodNotes' ? render() : previousRefresh(col);
    const previousReady = PrepLocal.ready;
    PrepLocal.ready = () => {
        previousReady();
        el('moodForm').onsubmit = save;
        el('moodStage').addEventListener('pointerdown', pointerDown);
        el('moodStage').addEventListener('pointermove', pointerMove);
        el('moodStage').addEventListener('pointerup', event => pointerUp(event));
        el('moodStage').addEventListener('pointercancel', event => pointerUp(event, true));
        document.addEventListener('keydown', event => { if (event.key === 'Escape' && drag) pointerUp(null, true); });
        addEventListener('resize', () => { if (el('view-mood').classList.contains('active')) render(); });
    };
    window.MoodWall = {render, create, remove, removePhoto, select, newWall, page, surface, cancelDrag: () => pointerUp(null, true)};
})();
