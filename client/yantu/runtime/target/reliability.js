(() => {
    const el = id => document.getElementById(id);
    const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const names = {tasks:'任务',calendarEvents:'考试安排',timelineItems:'阶段规划',timelines:'阶段记录',mistakes:'错题',notes:'笔记',resources:'资料',mindmaps:'导图',papers:'试卷',scores:'成绩',reviews:'复盘',schools:'院校',exams:'考试',moodNotes:'心情便签',moodWalls:'心情墙',focusSessions:'专注记录',reviewAttempts:'复习作答'};
    const fields = {title:'标题',name:'名称',content:'内容',text:'文字',path:'分类',date:'日期',files:'附件',fileId:'试卷附件',solution:'解析',cause:'错误原因',mastery:'掌握情况',nextReview:'复习日期',remark:'备注',tags:'标签',math:'数学',english:'英语',politics:'政治',major:'408',done:'完成状态',favorite:'收藏',practiceStatus:'练习状态','$record':'整条记录',prepStatus:'备考状态',focusTimer:'计时状态'};
    const stamp = value => value ? PrepLocal.stampText(new Date(value * 1000).toISOString()) : '尚无记录';
    const bytes = value => (Number(value || 0) / 1024 / 1024).toFixed(1) + ' MB';
    let statusData, selected, records = [], mode = 'trash', busy = false, lastOffline = false;
    let recoveryOffset = 0, recoveryTotal = 0, recoveryRequest = 0, searchTimer;
    const pageSize = 50;
    function recordName(record) { return record?.title || record?.name || record?.text?.slice(0,60) || record?.date || '未命名记录'; }
    function readable(value) {
        if (value && typeof value === 'object' && 'exists' in value) return value.exists ? readable(value.value) : '未设置';
        if (value === null || value === undefined) return '无';
        if (typeof value === 'boolean') return value ? '是' : '否';
        if (typeof value !== 'object') return String(value);
        if (Array.isArray(value.crop) && Array.isArray(value.strokes)) return `旋转 ${value.rotation||0} 度；裁剪 ${value.crop.map(part=>Math.round(part*100)+'%').join(' / ')}；批注 ${value.strokes.length} 笔`;
        if (Array.isArray(value)) return value.map(item => typeof item === 'object' ? item.name || recordName(item) : item).join('、') || '无';
        return Object.entries(value).filter(([key]) => key !== 'id').map(([key,item]) => `${fields[key] || key}：${readable(item)}`).join('\n');
    }
    function diagnostics(detail = PrepLocal.diagnostics()) {
        if (!el('safetySyncState')) return;
        const stale = detail.lastContact && Date.now() - Date.parse(detail.lastContact) > 45000;
        el('safetySyncState').textContent = detail.online && !stale ? '电脑连接正常' : '电脑连接已中断';
        el('safetyPending').textContent = detail.pending;
        el('safetyConflictCount').textContent = detail.conflicts;
        el('safetyLastContact').textContent = detail.lastContact ? PrepLocal.stampText(detail.lastContact) : '尚未连接';
        el('safetySyncError').textContent = detail.lastError || (stale ? '最近一次电脑响应已超过 45 秒' : '无');
        const offline = !detail.online || stale;
        el('safetySyncState').classList.toggle('safety-warning', Boolean(offline));
        if (detail.initialized && offline && !lastOffline) PrepLocal.toast('电脑连接中断，已保存的文字修改保留在本机');
        lastOffline = offline;
    }
    async function refresh() {
        diagnostics();
        try {
            statusData = await PrepLocal.jsonRequest('/api/protection');
            const data = statusData;
            el('safetyTrashCount').textContent = data.counts.trash || 0;
            el('safetyHistoryCount').textContent = data.counts.history || 0;
            el('safetyBackupStatus').textContent = data.backup.error || (data.backup.lastSuccess ? '最近成功：' + stamp(data.backup.lastSuccess) : '等待首次自动备份');
            el('safetyBackupStatus').classList.toggle('safety-warning', Boolean(data.backup.error));
            el('safetyBackupSize').textContent = bytes(data.backupBytes) + ' / ' + bytes(data.config.maxBytes) + (data.backup.externalFiles ? ' · ' + data.backup.externalFiles + ' 个资源库原件另存，学习 ZIP 仅保留引用，不包含这些原件。' : '');
            el('safetyBackupControls').hidden = !data.local;
            const form = el('safetyBackupForm');
            if (!form.contains(document.activeElement)) {
                form.elements.enabled.checked = data.config.enabled;
                form.elements.keep.value = data.config.keep;
                form.elements.maxMB.value = data.config.maxBytes / 1024 / 1024;
                form.elements.secondaryEnabled.checked = Boolean(data.config.secondary?.enabled);
                form.elements.secondaryPath.value = data.config.secondary?.path || '';
                form.elements.allowSameDisk.checked = Boolean(data.config.secondary?.allowSameDisk);
            }
            const secondary = data.secondary || {};
            el('safetySecondaryStatus').textContent = !data.config.secondary?.enabled ? '第二副本未开启' : secondary.error || (secondary.lastSuccess ? '第二副本最近成功：' + stamp(secondary.lastSuccess) : '第二副本等待生成');
            el('safetySecondaryWarning').textContent = data.config.secondary?.warning || secondary.warning || '';
            el('safetyBackups').replaceChildren();
            for (const backup of data.backups) {
                const row = document.createElement('div'); row.className = 'safety-backup-row';
                row.innerHTML = `<span>${esc(stamp(backup.created))} · ${bytes(backup.size)}</span>`;
                if (data.local) {
                    const button = document.createElement('button');
                    button.className = 'btn btn-icon btn-ghost';
                    button.type = 'button';
                    button.title = '下载备份';
                    button.setAttribute('aria-label', button.title);
                    button.innerHTML = '<i class="fas fa-download"></i>';
                    button.onclick = () => NebulaDownloads.url(backup.id, '/api/protection/backup-file?id=' + encodeURIComponent(backup.id));
                    row.append(button);
                }
                el('safetyBackups').append(row);
            }
        } catch (error) { el('safetyBackupStatus').textContent = '暂不可读取：' + error.message; }
    }
    async function health() {
        const list = el('safetyHealth'); list.textContent='正在检查…';
        const results = [];
        for (const [path, label, inspect] of [
            ['/api/time','电脑响应', result => '正常 · ' + result.systemTime],
            ['/api/notifications','通知后台', result => `${result.scheduler?'运行中':'未运行'} · ${result.deviceCount} 台已授权设备${result.lastTick?' · 最近调度 '+stamp(result.lastTick/1000):' · 等待首次调度'}${result.error?' · '+result.error:''}`],
            ['/api/info','远程入口', result => result.remote?.active ? (result.remote.url?.endsWith('.trycloudflare.com')?'临时地址，重启后可能变化':'已连接') : '未开启，局域网仍可使用']
        ]) {
            const start = performance.now();
            try { const data=await PrepLocal.jsonRequest(path);results.push({label,value:inspect(data),ms:Math.round(performance.now()-start)}); }
            catch(error) { results.push({label,value:error.message,error:true}); }
        }
        list.innerHTML=results.map(item=>`<div class="safety-health-row ${item.error?'safety-warning':''}"><strong>${item.label}</strong><span>${esc(item.value)}${item.ms!==undefined?' · '+item.ms+' ms':''}</span></div>`).join('');
        await refresh();
    }
    function modeOptions() {
        const selector=el('recoveryCollection'), value=selector.value;
        selector.innerHTML='<option value="">全部类型</option>'+Object.entries(names).map(([key,name])=>`<option value="${key}">${name}</option>`).join('');selector.value=value;
    }
    async function open(kind='trash', collection='', record='') {
        mode=kind;selected=null;el('recoveryCollection').value=collection;el('recoveryRecord').value=record;
        recoveryOffset=0;el('recoverySearch').value='';el('recoveryAfter').value='';el('recoveryBefore').value='';
        el('recoveryKind').value=kind;el('recoveryCompare').replaceChildren();
        if(!el('recoveryDialog').open)el('recoveryDialog').showModal();
        await load();
    }
    async function load() {
        const request=++recoveryRequest;
        selected=null;el('recoveryActions').hidden=true;el('recoveryCompare').replaceChildren();
        try {
            const parameters=new URLSearchParams({kind:mode,collection:el('recoveryCollection').value,record:el('recoveryRecord').value,q:el('recoverySearch').value.trim(),offset:recoveryOffset,limit:pageSize});
            if(el('recoveryAfter').value)parameters.set('after',PrepLocal.dayStart(el('recoveryAfter').value)/1000);
            if(el('recoveryBefore').value)parameters.set('before',(PrepLocal.dayStart(el('recoveryBefore').value)+86400000)/1000-.001);
            const result=await PrepLocal.jsonRequest('/api/recovery?'+parameters);
            if(request!==recoveryRequest)return;
            if(Number.isInteger(result.total)){recoveryTotal=result.total;records=result.records;}
            else {
                let legacy=result.records||[];
                if(mode==='conflicts')legacy=(await PrepLocal.jsonRequest('/api/protection')).conflicts||[];
                const query=parameters.get('q').toLocaleLowerCase(),after=Number(parameters.get('after')||0),before=Number(parameters.get('before')||Infinity);
                legacy=legacy.filter(item=>(!query||JSON.stringify(item).toLocaleLowerCase().includes(query))&&item.created>=after&&item.created<=before);
                recoveryTotal=legacy.length;records=legacy.slice(recoveryOffset,recoveryOffset+pageSize);result.hasMore=recoveryOffset+records.length<recoveryTotal;
            }
            if(recoveryTotal&&recoveryOffset>=recoveryTotal){recoveryOffset=Math.floor((recoveryTotal-1)/pageSize)*pageSize;return load();}
            el('recoveryRange').textContent=recoveryTotal?`${recoveryOffset+1}–${recoveryOffset+records.length} / ${recoveryTotal}`:'0 条';
            el('recoveryPrevious').disabled=!recoveryOffset;el('recoveryNext').disabled=!result.hasMore;
            el('recoveryList').innerHTML='';
            records.forEach(item=>{
                const button=document.createElement('button');button.type='button';button.className='safety-record';
                button.innerHTML=`<strong>${esc(mode==='conflicts'?item.title:recordName(item.record))}</strong><span>${esc(names[item.collection||item.operation?.collection]||'设置')} · ${esc(stamp(item.created))}</span>`;
                button.onclick=()=>compare(item);el('recoveryList').append(button);
            });
            if(!records.length)el('recoveryList').innerHTML='<p class="safety-empty">暂无记录</p>';
        }catch(error){el('recoveryList').textContent=error.message;}
    }
    function compare(item) {
        selected=item;el('recoveryActions').hidden=false;
        el('recoveryRestore').hidden=mode==='conflicts'||mode==='conflict';el('recoveryPurge').hidden=mode!=='trash';
        el('recoveryKeepCurrent').hidden=el('recoveryKeepIncoming').hidden=mode!=='conflicts';
        el('recoveryRestore').textContent=mode==='trash'?'恢复记录':'恢复这个版本';
        const body=el('recoveryCompare');
        if(mode==='conflicts'||mode==='conflict') {
            const conflict=mode==='conflict'?item.record:item;
            body.innerHTML=Object.entries(conflict.fields||{}).map(([key,values])=>`<section><h4>${esc(fields[key]||key)}</h4><div class="safety-comparison"><div><h5>编辑前</h5><pre>${esc(readable(values.base))}</pre></div><div><h5>电脑当前</h5><pre>${esc(readable(values.current))}</pre></div><div><h5>待同步版本</h5><pre>${esc(readable(values.incoming))}</pre></div></div></section>`).join('');
        } else {
            const current=(state[item.collection]||[]).find(record=>record.id===item.recordId)||null;
            selected.expected=current?JSON.parse(JSON.stringify(current)):null;
            body.innerHTML=`<div class="safety-comparison"><div><h5>当前记录</h5><pre>${esc(readable(current))}</pre></div><div><h5>${mode==='trash'?'已删除记录':'历史版本'}</h5><pre>${esc(readable(item.record))}</pre></div></div>`;
        }
    }
    async function action(path,body) {
        if(busy)return;busy=true;
        el('recoveryActions').querySelectorAll('button').forEach(button=>button.disabled=true);
        try {
            await PrepLocal.flush();
            if(PrepLocal.hasPending())throw new Error('请先完成待同步修改，再处理恢复或冲突');
            const result=await PrepLocal.jsonRequest(path,body);
            if(result.state)PrepLocal.accept(result);
            PrepLocal.toast('操作已保存');await load();await refresh();
        } catch(error) { PrepLocal.toast(error.message);await load(); }
        finally { busy=false;el('recoveryActions').querySelectorAll('button').forEach(button=>button.disabled=false); }
    }
    function setup() {
        const section=document.createElement('section');section.className='safety-workspace';section.id='safetyWorkspace';
        section.innerHTML=`<header><h3><i class="fas fa-shield-halved"></i> 数据保护与连接</h3><button class="btn btn-ghost" id="safetyCheck"><i class="fas fa-stethoscope"></i> 检查连接</button></header><dl class="safety-diagnostics"><div><dt>连接</dt><dd id="safetySyncState"></dd></div><div><dt>最近响应</dt><dd id="safetyLastContact"></dd></div><div><dt>待同步</dt><dd id="safetyPending">0</dd></div><div><dt>待选择冲突</dt><dd id="safetyConflictCount">0</dd></div><div class="safety-diagnostic-error"><dt>最近连接问题</dt><dd id="safetySyncError"></dd></div></dl><div id="safetyHealth" aria-live="polite"></div><div class="safety-entry-buttons"><button class="btn btn-ghost" id="safetyTrash"><i class="fas fa-trash-can-arrow-up"></i> 回收站 <span id="safetyTrashCount">0</span></button><button class="btn btn-ghost" id="safetyHistory"><i class="fas fa-clock-rotate-left"></i> 历史版本 <span id="safetyHistoryCount">0</span></button><button class="btn btn-ghost" id="safetyConflicts"><i class="fas fa-code-compare"></i> 处理冲突</button></div><h4>本地自动备份</h4><p id="safetyBackupStatus" aria-live="polite"></p><p id="safetyBackupSize"></p><div id="safetyBackupControls"><form id="safetyBackupForm"><label class="workflow-checkbox"><input type="checkbox" name="enabled" /> 每日备份</label><label>保留份数<input type="number" name="keep" min="1" max="30" required /></label><label>容量上限（MB）<input type="number" name="maxMB" min="32" max="20480" required /></label><button class="btn btn-primary" type="submit"><i class="fas fa-check"></i> 保存</button><button class="btn btn-ghost" type="button" id="safetyBackupNow"><i class="fas fa-box-archive"></i> 立即备份</button></form></div><div id="safetyBackups"></div>`;
        el('view-connection').append(section);
        const backupForm=el('safetyBackupForm');
        const secondary=document.createElement('fieldset');secondary.className='safety-secondary-fields';
        secondary.innerHTML='<legend>第二副本</legend><label class="workflow-checkbox"><input name="secondaryEnabled" type="checkbox" /> 开启第二副本</label><label class="safety-destination-path">备份目录<input name="secondaryPath" type="text" autocomplete="off" /></label><label class="workflow-checkbox"><input name="allowSameDisk" type="checkbox" /> 我接受同盘或无法核验物理硬盘的风险</label>';
        backupForm.append(secondary);
        const secondaryStatus=document.createElement('div');secondaryStatus.innerHTML='<p id="safetySecondaryStatus" role="status"></p><p id="safetySecondaryWarning" class="safety-warning"></p>';el('safetyBackupControls').after(secondaryStatus);
        const dialog=document.createElement('dialog');dialog.id='recoveryDialog';dialog.className='safety-dialog';
        dialog.innerHTML='<header class="dialog-header"><h3>恢复与版本比较</h3><button class="btn btn-icon" type="button" title="返回" aria-label="返回"><i class="fas fa-xmark"></i></button></header><div class="dialog-body"><div class="safety-recovery-filters"><select id="recoveryKind" aria-label="记录类别"><option value="trash">回收站</option><option value="history">历史版本</option><option value="conflicts">待选择冲突</option><option value="conflict">冲突留档</option></select><select id="recoveryCollection" aria-label="资料类型"></select><input id="recoveryRecord" aria-label="记录编号" type="hidden" /><button class="btn btn-icon btn-ghost" id="recoveryClear" title="全部记录" aria-label="全部记录"><i class="fas fa-filter-circle-xmark"></i></button></div><div class="safety-recovery-body"><aside id="recoveryList"></aside><div id="recoveryCompare"></div></div><footer id="recoveryActions" hidden><button class="btn btn-primary" id="recoveryRestore">恢复记录</button><button class="btn btn-danger" id="recoveryPurge">永久删除</button><button class="btn btn-ghost" id="recoveryKeepCurrent">保留电脑当前版本</button><button class="btn btn-primary" id="recoveryKeepIncoming">采用待同步版本</button></footer></div>';
        dialog.querySelector('.dialog-header button').onclick=()=>dialog.close();document.body.append(dialog);modeOptions();
        const filters=document.createElement('div');filters.className='safety-recovery-filters';filters.innerHTML='<input id="recoverySearch" type="search" aria-label="搜索恢复记录" placeholder="搜索标题或内容" /><label>从<input id="recoveryAfter" type="date" /></label><label>至<input id="recoveryBefore" type="date" /></label>';
        el('recoveryList').parentElement.before(filters);
        const pagination=document.createElement('nav');pagination.className='safety-pagination';pagination.setAttribute('aria-label','恢复记录分页');pagination.innerHTML='<button class="btn btn-icon btn-ghost" id="recoveryPrevious" title="上一页" aria-label="上一页"><i class="fas fa-chevron-left"></i></button><span id="recoveryRange" role="status"></span><button class="btn btn-icon btn-ghost" id="recoveryNext" title="下一页" aria-label="下一页"><i class="fas fa-chevron-right"></i></button>';
        el('recoveryActions').before(pagination);
        const filtered=()=>{recoveryOffset=0;load();};el('recoverySearch').oninput=()=>{clearTimeout(searchTimer);searchTimer=setTimeout(filtered,200);};el('recoveryAfter').onchange=el('recoveryBefore').onchange=filtered;
        el('recoveryPrevious').onclick=()=>{recoveryOffset=Math.max(0,recoveryOffset-pageSize);load();};el('recoveryNext').onclick=()=>{if(recoveryOffset+pageSize<recoveryTotal){recoveryOffset+=pageSize;load();}};
        el('safetyTrash').onclick=()=>open('trash');el('safetyHistory').onclick=()=>open('history');el('safetyConflicts').onclick=()=>open('conflicts');el('safetyCheck').onclick=health;
        el('recoveryKind').onchange=event=>{mode=event.target.value;filtered();};el('recoveryCollection').onchange=()=>{el('recoveryRecord').value='';filtered();};el('recoveryClear').onclick=()=>{el('recoveryRecord').value='';el('recoveryCollection').value='';el('recoverySearch').value='';el('recoveryAfter').value='';el('recoveryBefore').value='';filtered();};
        el('recoveryRestore').onclick=()=>{if(selected&&confirm('恢复该记录？当前内容会保留为历史版本。'))action('/api/recovery/restore',{id:selected.id,expected:selected.expected});};
        el('recoveryPurge').onclick=()=>{if(selected&&confirm('永久删除这条记录和其历史版本？已有备份中的副本不会自动删除。')&&confirm('确认永久删除，不再从回收站恢复？'))action('/api/recovery/purge',{id:selected.id,confirmation:'PERMANENT_DELETE'});};
        for(const choice of ['Current','Incoming'])el('recoveryKeep'+choice).onclick=()=>{if(selected)action('/api/protection/resolve',{id:selected.id,choice:choice.toLowerCase()});};
        el('safetyBackupForm').onsubmit=async event=>{event.preventDefault();const form=event.target;try{await PrepLocal.jsonRequest('/api/protection/settings',{enabled:form.elements.enabled.checked,keep:Number(form.elements.keep.value),maxBytes:Number(form.elements.maxMB.value)*1024*1024,secondary:{enabled:form.elements.secondaryEnabled.checked,path:form.elements.secondaryPath.value.trim(),allowSameDisk:form.elements.allowSameDisk.checked}});PrepLocal.toast('备份设置已保存');await refresh();}catch(error){PrepLocal.toast(error.message);}};
        el('safetyBackupNow').onclick=async event=>{event.currentTarget.disabled=true;try{await PrepLocal.flush();const saved=await PrepLocal.jsonRequest('/api/protection/backup',{}, {long:true});PrepLocal.toast(saved.externalFiles?'学习记录备份已保存，资源库原件仍需单独保留':'完整备份已保存到电脑数据目录');}catch(error){PrepLocal.toast(error.message);}finally{el('safetyBackupNow').disabled=false;await refresh();}};
        const previousOpen=StudyLibrary.open;
        StudyLibrary.open=(id,...args)=>{previousOpen(id,...args);const {col}=findRecordWithCol(id);let button=el('studyHistoryButton');if(!button){button=document.createElement('button');button.id='studyHistoryButton';button.className='btn btn-icon btn-ghost';button.title='历史版本';button.setAttribute('aria-label',button.title);button.innerHTML='<i class="fas fa-clock-rotate-left"></i>';el('studyDetailEdit').before(button);}button.onclick=()=>open('history',col,id);};
        addEventListener('study-detail',event=>{const button=el('studyHistoryButton');if(button)button.onclick=()=>open('history',event.detail.col,event.detail.id);});
        window.addEventListener('yantu-sync',event=>diagnostics(event.detail));
        const previousConfirm=showDeleteConfirm;
        showDeleteConfirm=(action,message)=>{if(/^https?:$/.test(location.protocol)){previousConfirm(action,(message||'确定删除这些记录？').replace(/此操作不可恢复[！!。]?/g,'学习记录会移入回收站。'));el('deleteSecondConfirm').querySelector('p').textContent='再次确认：学习记录将移入回收站。';}else previousConfirm(action,message);};
        setInterval(()=>{diagnostics();if(document.body.dataset.view==='connection'&&!document.hidden)refresh();},20000);
        refresh();
    }
    const previousReady=PrepLocal.ready;PrepLocal.ready=()=>{setup();previousReady();};
    const previousView=renderView;renderView=view=>{previousView(view);if(view==='connection')refresh();};
    window.Safety={open,refresh,health};
})();
