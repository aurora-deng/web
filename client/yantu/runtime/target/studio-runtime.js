(() => {
    const el = id => document.getElementById(id);
    const escape = value => String(value ?? '').replace(/[&<>"']/g, char => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
    const icons = {overview:'fa-house',timeline:'fa-route',mistakes:'fa-circle-xmark',resources:'fa-book-open',mindmaps:'fa-diagram-project',papers:'fa-file-lines',scores:'fa-chart-line',review:'fa-pen-nib',schools:'fa-building-columns',exams:'fa-calendar-days',connection:'fa-link',notifications:'fa-bell',mood:'fa-heart',assistant:'fa-robot'};
    const urls = new Map(), pendingFiles = new Map(), queue = [], queued = new WeakSet();
    function previewSource(image) {
        const id=image.dataset.previewFile,record=findRecord(image.dataset.previewRecord),file=record?.files?.find(item=>item.id===id)||{id};
        const edit=record?.fileEdits?.[id+':1'],marks=window.MediaPreview?MediaPreview.marks(state.readingMarks,file,1):[];
        return {id,file,edit,marks,key:JSON.stringify([image.dataset.previewRecord||'',id,file.sha256||'',edit||null,marks])};
    }
    let activeLoads = 0;
    const observer = new IntersectionObserver(entries => {
        for (const entry of entries) if (entry.isIntersecting) {
            observer.unobserve(entry.target);
            const image = entry.target.querySelector('img[data-preview-file]');
            if (image && !queued.has(image)) { queued.add(image); queue.push(image); }
        }
        pump();
    }, {rootMargin:'120px'});

    function trimPreviews() {
        const visible = new Set([...document.querySelectorAll('.view.active [data-preview-file]')].filter(image => { const rect = image.parentElement.getBoundingClientRect(); return rect.width>0 && rect.height>0 && rect.bottom >= 0 && rect.top <= innerHeight; }).map(image => image.dataset.previewKey));
        for (const [id, url] of urls) {
            if (urls.size <= 24) break;
            if (!visible.has(id)) { URL.revokeObjectURL(url); urls.delete(id); }
        }
    }
    async function previewURL(source) {
        const {id,file,edit,marks,key}=source;
        if (urls.has(key)) return urls.get(key);
        if (!pendingFiles.has(key)) pendingFiles.set(key, (async()=>{
            if(window.MediaPreview){
                const {image}=await MediaPreview.image(file),canvas=document.createElement('canvas'),small=document.createElement('canvas');
                try{await window.AnnotationText?.prepareEdit(edit);MediaPreview.paint(canvas,image,edit,marks);const scale=Math.min(1,320/Math.max(canvas.width,canvas.height));small.width=Math.max(1,Math.round(canvas.width*scale));small.height=Math.max(1,Math.round(canvas.height*scale));small.getContext('2d').drawImage(canvas,0,0,small.width,small.height);return await new Promise((resolve,reject)=>small.toBlob(blob=>blob?resolve(blob):reject(new Error('缩略图生成失败'))));}
                finally{image.close();canvas.width=canvas.height=small.width=small.height=1;}
            }
            return await DeviceStore.get('file-'+id)||await getFile(id);
        })());
        try {
            const blob = await pendingFiles.get(key);
            if (!urls.has(key)) urls.set(key, URL.createObjectURL(blob));
            return urls.get(key);
        } finally { pendingFiles.delete(key); }
    }
    function pump() {
        // Only two visible attachment previews may load at a time.
        while (activeLoads < 2 && queue.length) {
            const image = queue.shift();
            if (!image.isConnected || !image.closest('.view.active') || !image.parentElement.getClientRects().length) { queued.delete(image); continue; }
            activeLoads++;
            (async () => {
                let loadedKey;
                try {
                    const source=previewSource(image),url = await previewURL(source);
                    loadedKey=source.key;
                    if (!image.isConnected||previewSource(image).key!==source.key) return;
                    image.dataset.previewKey=source.key;
                    image.src = url;
                    await image.decode();
                    if (!image.isConnected||previewSource(image).key!==source.key) return;
                    image.hidden = false;
                    image.previousElementSibling.hidden = true;
                } catch (_) {
                    if (image.isConnected) image.closest('button').title = '打开照片';
                } finally { queued.delete(image); activeLoads--; trimPreviews(); pump(); if(image.isConnected&&image.hidden&&loadedKey&&previewSource(image).key!==loadedKey)observer.observe(image.parentElement); }
            })();
        }
    }
    function observePreviews() {
        observer.disconnect();
        for (const image of document.querySelectorAll('.view.active img[data-preview-file]')) {
            if(image.dataset.previewKey&&image.dataset.previewKey!==previewSource(image).key){image.hidden=true;image.previousElementSibling.hidden=false;}
            if(image.hidden&&!queued.has(image))observer.observe(image.parentElement);
        }
        trimPreviews();
    }
    addEventListener('workspace-change',observePreviews);
    function empty(container, icon, text, command, action) {
        if (!container) return;
        container.innerHTML = `<div class="empty-state"><i class="fas ${icon}" aria-hidden="true"></i><p>${escape(text)}</p>${command ? `<button class="btn btn-ghost btn-sm" onclick="${action}"><i class="fas fa-plus" aria-hidden="true"></i> ${command}</button>` : ''}</div>`;
    }
    function updateWorkspace(view = document.body.dataset.view || 'overview') {
        document.body.dataset.view = view;
        el('workspaceMark').innerHTML = `<i class="fas ${icons[view] || 'fa-book-open'}"></i>`;
        let text = '';
        if (view === 'overview') {
            const today = formatDate(PrepLocal.nowDate());
            const tasks = (state.tasks || []).filter(task => task.date === today && !['cancelled', 'unknown'].includes(YantuActivity.taskStatus(task)));
            text = `${tasks.length} 项今日任务 · ${tasks.filter(task => task.done).length} 项完成`;
        } else if (view === 'timeline') {
            const stages = state.timelines || [];
            text = `${stages.length} 个阶段 · ${stages.filter(stage => stage.completed).length} 个完成`;
        } else if (view === 'resources') text = `${(state.notes || []).length} 篇笔记 · ${(state.resources || []).length} 份资料`;
        else if (view === 'mistakes') text = `${(state.mistakes || []).length} 条错题`;
        else if (view === 'mindmaps') text = `${(state.mindmaps || []).length} 份思维导图`;
        else if (view === 'papers') text = `${(state.papers || []).length} 份真题试卷`;
        else if (view === 'scores') text = `${(state.scores || []).length} 次成绩记录`;
        else if (view === 'review') text = `${(state.reviews || []).length} 篇学习复盘`;
        else if (view === 'schools') text = `${(state.schools || []).length} 所目标院校`;
        else if (view === 'exams') text = `${(state.calendarEvents || []).length} 场自定义考试`;
        else if (view === 'mood') text = `${(state.moodNotes || []).length} 张便签 · ${(state.moodWalls || []).length} 面墙`;
        el('workspaceSummary').textContent = text;
    }
    function scoreInsights(records = state.scores || []) {
        const scores = [...records].sort((a,b) => String(a.date || '').localeCompare(String(b.date || '')));
        const latest = scores.at(-1), previous = scores.at(-2);
        const total = scoreTotal;
        const change = latest && previous && scoreComplete(latest) && scoreComplete(previous) ? Number((total(latest) - total(previous)).toFixed(2)) : null;
        const trend = !latest ? '暂无成绩' : !scoreComplete(latest) ? '部分科目未录入' : change === null ? previous ? '缺少完整对比成绩' : '首份成绩记录' : `较上次 ${change > 0 ? '+' : ''}${change} 分`;
        el('scoreInsights').innerHTML = `<div class="score-cell"><small>${latest&&!scoreComplete(latest)?'已录分数':'最近总分'}</small><strong>${latest ? total(latest) : '--'}</strong><em>${trend}</em></div>` + [['数学','math'],['英语','english'],['政治','politics'],['408','major']].map(([name,key]) => `<div class="score-cell"><small>${name}</small><strong>${escape(scoreValue(latest?.[key]) ?? '--')}</strong><em>${key === 'english' || key === 'politics' ? '满分 100' : '满分 150'}</em></div>`).join('');
        for (const chart of [chartTotal, chartSubject]) if (chart) {
            chart.stop();
            chart.options.animation = false;
            for (const dataset of chart.data.datasets) {
                dataset.borderColor = {数学:'#6685b1',英语:'#c47782',政治:'#bda463','408':'#5d9fa7',总分:'#5b9d83'}[dataset.label] || dataset.borderColor;
                dataset.pointRadius = 3;
                dataset.pointHoverRadius = 5;
                dataset.borderWidth = 2;
                if (dataset.label === '总分') dataset.backgroundColor = '#5b9d8317';
            }
            chart.options.plugins.legend.labels = {boxWidth:9,boxHeight:9,usePointStyle:true,color:'#83909a',font:{size:11}};
            for (const axis of Object.values(chart.options.scales)) {
                axis.grid = {...axis.grid,color:'#e9edf0'};
                axis.ticks = {...axis.ticks,color:'#98a3aa',font:{size:10}};
            }
            chart.update('none');
        }
        updateWorkspace();
    }
    const previousList = renderRecordList;
    renderRecordList = (...args) => {
        previousList(...args);
        const container = args[0], col = container.dataset.collection;
        if (!container.querySelector('.file-record')) {
            const labels = {mistakes:['fa-circle-xmark','还没有错题记录','添加错题'],notes:['fa-book-open','还没有学习笔记','添加笔记'],resources:['fa-folder-open','还没有电子资料','上传资料'],mindmaps:['fa-diagram-project','还没有思维导图','上传导图'],papers:['fa-file-lines','还没有真题试卷','添加试卷']};
            const label = labels[col] && [...labels[col]];
            if (label && args[3] !== 'all') label[1] = '该分类暂无记录';
            if (label) empty(container,...label,col === 'papers' ? 'openPaperDialog()' : `openRecordDialog('${col}')`);
        }
        observePreviews();
    };
    const previousTimeline = renderTimeline;
    renderTimeline = () => { previousTimeline(); if (!state.timelines.length) empty(el('timelineList'),'fa-route','还没有阶段规划','添加阶段','openTimelineDialog()'); updateWorkspace(); };
    const previousScores = renderScores;
    renderScores = (records = state.scores) => { previousScores(records); scoreInsights(records); };
    const previousSchools = renderSchools;
    renderSchools = () => { previousSchools(); if (!state.schools.length) empty(el('schoolList'),'fa-building-columns','还没有目标院校','添加院校','openSchoolDialog()'); updateWorkspace(); };
    const previousReview = renderReview;
    renderReview = () => { previousReview(); if (!state.reviews.length) empty(el('reviewList'),'fa-pen-nib','还没有复盘记录'); updateWorkspace(); };
    const previousOverview = renderOverview;
    renderOverview = () => { previousOverview(); updateWorkspace(); };
    const previousView = renderView;
    renderView = view => { document.body.dataset.view = view; previousView(view); updateWorkspace(view); observePreviews(); };
    const previousReady = PrepLocal.ready;
    PrepLocal.ready = () => { previousReady(); updateWorkspace(document.querySelector('.nav-item.active')?.dataset.view || 'overview'); observePreviews(); };
    window.StudioUI = {refreshPreviews:observePreviews, updateWorkspace};
    addEventListener('pagehide', event => { if (event.persisted) return; for (const url of urls.values()) URL.revokeObjectURL(url); urls.clear(); });
})();
