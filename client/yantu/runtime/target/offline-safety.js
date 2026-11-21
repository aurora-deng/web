(() => {
    const el=id=>document.getElementById(id),clone=value=>structuredClone(value);
    const esc=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const fileId=value=>typeof value==='string'&&/^[A-Za-z0-9][A-Za-z0-9_.-]{0,99}$/.test(value)&&!value.endsWith('.');
    const mb=value=>(Number(value||0)/1024/1024).toFixed(1)+' MB';
    let prepared,busy=false;
    const hash=async blob=>[...new Uint8Array(await crypto.subtle.digest('SHA-256',await blob.arrayBuffer()))].map(value=>value.toString(16).padStart(2,'0')).join('');
    function fileReferences(value,result=new Map()){
        if(!value||typeof value!=='object')return result;
        if(Array.isArray(value.files))for(const file of value.files)if(fileId(file.id))result.set(file.id,file);
        if(fileId(value.fileId)&&!result.has(value.fileId))result.set(value.fileId,{id:value.fileId,name:value.fileName||value.title||'附件',type:'application/pdf'});
        if(fileId(value.sourceFileId)&&!result.has(value.sourceFileId))result.set(value.sourceFileId,{id:value.sourceFileId,name:value.fileName||'引用原件',type:'application/pdf'});
        for(const item of Object.values(value))if(item&&typeof item==='object')fileReferences(item,result);
        return result;
    }
    async function refresh(){
        const diagnostic=PrepLocal.diagnostics();
        el('offlinePending').textContent=`${diagnostic.pendingRecords||0} 项学习修改 · ${diagnostic.pendingFiles||0} 个附件 · ${window.DeviceAI?.pending()||0} 项对话/记忆`;
        try{const estimate=await navigator.storage?.estimate?.();el('offlineCapacity').textContent=estimate?`${mb(estimate.usage)} / ${mb(estimate.quota)}`:'此浏览器未提供容量信息';el('offlinePersistence').textContent=await navigator.storage?.persisted?.()?'已获持久存储授权':'未获持久存储授权';}catch(_){el('offlinePersistence').textContent='当前浏览器无法查询';}
        el('offlineCapabilities').textContent=[isSecureContext?'安全连接':'非 HTTPS：部分本机能力受限','serviceWorker'in navigator?'离线页面可用':'无离线页面支持','PushManager'in window&&'Notification'in window?'支持系统推送接口':'不支持系统推送接口'].join(' · ');
    }
    async function persistence(){try{const accepted=await navigator.storage?.persist?.();PrepLocal.toast(accepted?'浏览器已批准持久存储；主动清除站点数据仍会删除本机内容':'浏览器未批准持久存储，请保持同步并保留紧急包');await refresh();}catch(error){PrepLocal.toast(error.message);}}
    async function run(action){if(busy)return;busy=true;el('offlineSafety').querySelectorAll('button').forEach(button=>button.disabled=true);try{await action();}catch(error){PrepLocal.toast(error.message);el('offlineProgress').textContent=error.message;}finally{busy=false;el('offlineSafety').querySelectorAll('button').forEach(button=>button.disabled=false);refresh();}}
    async function exportBundle(){
        if(!isSecureContext||!crypto.subtle)throw new Error('紧急包完整性校验需要 HTTPS 或电脑本机入口');
        const bundle=PrepLocal.rescueSnapshot(),assistant=await DeviceAI.rescueSnapshot(),zip=new JSZip();
        bundle.localDrafts=[];
        for(const prefix of ['reader-note-draft-','reader-region-draft-','richnote-draft-','card-draft-','board-draft-','practice-local-','planning-draft-','planning-focus-draft-','subject-tools-draft-','exam-local-','workbench-draft-','assistant-local-','rescue-draft-conflict-'])for(const [key,value]of await DeviceStore.entries(prefix))bundle.localDrafts.push([key,value]);
        const references=fileReferences(bundle);
        for(const upload of bundle.uploads)if(!references.has(upload.id))references.set(upload.id,upload);
        bundle.files=[];bundle.pages=[];bundle.missingFiles=[];bundle.assistant=assistant;
        let bytes=0,index=0;
        for(const [id,info]of references){
            el('offlineProgress').textContent=`整理附件 ${++index} / ${references.size}`;
            const blob=await DeviceStore.get('file-'+id);
            if(!blob){bundle.missingFiles.push({id,name:info.name||'附件'});continue;}
            if(blob.size>256*1024**2)throw new Error('单个附件超过紧急包 256 MB 限额，请保持本机内容并使用电脑完整备份；现有资料未改动');bytes+=blob.size;if(bytes>512*1024*1024)throw new Error('本机附件超过紧急包 512 MB 限额，请保持本机内容并使用电脑完整备份；现有资料未改动');
            const path='files/'+id;zip.file(path,blob);bundle.files.push({id,name:info.name||'附件',type:blob.type||info.type||'application/octet-stream',size:blob.size,path,sha256:await hash(blob)});
        }
        for(const [key,value]of await DeviceStore.entries('page-')){
            const parsed=/^page-(.+)-(\d+)$/.exec(key),match=parsed?.[1];
            if(!match||!references.has(match)||!(value.blob instanceof Blob))continue;
            const page=Number(parsed[2]);if(!Number.isInteger(page)||page<1||page>10000)continue;
            if(value.blob.size>30*1024**2)throw new Error('单张缓存页超过紧急包 30 MB 限额，现有资料未改动');
            bytes+=value.blob.size;if(bytes>512*1024*1024)throw new Error('紧急包超过 512 MB，请减少离线缓存后重试');
            const path=`pages/${match}-${page}.png`;zip.file(path,value.blob);bundle.pages.push({file:match,page,pages:value.pages,size:value.blob.size,path,sha256:await hash(value.blob),canonical:Boolean(value.canonical),fingerprint:typeof value.fingerprint==='string'?value.fingerprint:''});
        }
        const manifest=JSON.stringify(bundle);if(new Blob([manifest]).size>12*1024**2)throw new Error('学习文字与草稿超过本机紧急包 12 MB 限额，请保持本机内容并使用电脑完整备份；尚未生成不完整的紧急包');zip.file('emergency.json',manifest);
        const output=await zip.generateAsync({type:'blob',compression:'STORE',streamFiles:true},progress=>el('offlineProgress').textContent=`生成紧急包 ${Math.floor(progress.percent)}%`);
        downloadBlob('研途-本机紧急包-'+PrepLocal.dateText(PrepLocal.nowDate())+'.zip',output);
        el('offlineProgress').textContent=`已导出 ${bundle.operations.length} 项学习操作、${assistant.pending.length} 项对话/记忆操作、${bundle.files.length} 个附件${bundle.missingFiles.length?'；另有 '+bundle.missingFiles.length+' 个附件未缓存，包中已列明':''}。不含 API 密钥或配对凭据。`;
    }
    function readEntry(entry,limit,type='blob'){
        if(!entry)throw new Error('紧急包缺少必要文件');
        return new Promise((resolve,reject)=>{let size=0,finished=false;const chunks=[],stream=entry.internalStream('uint8array');stream.on('data',chunk=>{if(finished)return;size+=chunk.length;if(size>limit){finished=true;stream.pause();reject(new Error('紧急包内容超过限制'));return;}chunks.push(chunk);}).on('error',error=>{if(!finished){finished=true;reject(error);}}).on('end',()=>{if(!finished){finished=true;const blob=new Blob(chunks);resolve(type==='text'?blob.text():blob);}}).resume();});
    }
    async function prepare(file){
        if(!file)return;if(file.size>540*1024*1024)throw new Error('紧急包文件超过 540 MB');
        const zip=await JSZip.loadAsync(file),text=await readEntry(zip.file('emergency.json'),12*1024*1024,'text');
        const bundle=JSON.parse(text,(key,value)=>{if(['__proto__','prototype','constructor'].includes(key))throw new Error('紧急包包含不安全字段');return value;});
        PrepLocal.checkRescue(bundle);DeviceAI.checkRescue(bundle.assistant);
        if(!Array.isArray(bundle.files)||!Array.isArray(bundle.pages)||bundle.files.length+bundle.pages.length>5000)throw new Error('紧急包附件清单无效');
        if(bundle.localDrafts!==undefined&&(!Array.isArray(bundle.localDrafts)||bundle.localDrafts.length>5000))throw new Error('紧急包草稿清单无效');
        const entries=[],ids=new Set(),references=fileReferences({state:bundle.state,operations:bundle.operations,localDrafts:bundle.localDrafts});let total=0;
        for(const upload of bundle.uploads)if(!references.has(upload.id))references.set(upload.id,upload);
        const draftPattern=/^(reader-note-draft-|reader-region-draft-|richnote-draft-|card-draft-|board-draft-|practice-local-|planning-draft-|planning-focus-draft-|subject-tools-draft-|exam-local-|workbench-draft-|assistant-local-)[A-Za-z0-9_.-]{1,201}$/;
        for(const entry of bundle.localDrafts||[]){if(!Array.isArray(entry)||entry.length!==2||typeof entry[0]!=='string')throw new Error('紧急包草稿无效');if(entry[0].startsWith('rescue-draft-conflict-')){if(!/^rescue-draft-conflict-[a-f0-9]{64}$/.test(entry[0])||!entry[1]||!draftPattern.test(entry[1].key)||!Object.hasOwn(entry[1],'current')||!Object.hasOwn(entry[1],'incoming'))throw new Error('紧急包草稿比较记录无效');}else if(!draftPattern.test(entry[0]))throw new Error('紧急包包含不可恢复的设备设置');entries.push(entry);}
        for(const info of bundle.files){
            if(!fileId(info.id)||ids.has(info.id)||info.path!=='files/'+info.id||!references.has(info.id)||!Number.isInteger(info.size)||info.size<0||info.size>256*1024*1024)throw new Error('紧急包附件无效');
            const blob=await readEntry(zip.file(info.path),Math.min(info.size+1,256*1024*1024));if(blob.size!==info.size||await hash(blob)!==info.sha256)throw new Error('附件完整性校验失败');
            const existing=await DeviceStore.get('file-'+info.id);if(existing instanceof Blob&&await hash(existing)!==info.sha256)throw new Error('同编号附件与本机内容不同，未覆盖；请保留紧急包并在电脑恢复预览中比较');
            total+=blob.size;if(total>512*1024*1024)throw new Error('紧急包超过本机恢复上限');ids.add(info.id);
            const typed=new Blob([blob],{type:info.type});entries.push(['file-'+info.id,typed],['upload-'+info.id,{id:info.id,name:info.name,type:info.type,size:info.size,selectedAt:PrepLocal.nowDate().toISOString()}]);
        }
        for(const page of bundle.pages){
            if(!fileId(page.file)||!references.has(page.file)||!Number.isInteger(page.page)||page.page<1||page.page>10000||page.path!==`pages/${page.file}-${page.page}.png`||!Number.isInteger(page.size)||page.size<0||page.size>30*1024*1024)throw new Error('紧急包缓存页无效');
            const blob=await readEntry(zip.file(page.path),page.size+1);if(blob.size!==page.size||await hash(blob)!==page.sha256)throw new Error('缓存页校验失败');total+=blob.size;if(total>512*1024*1024)throw new Error('紧急包超过本机恢复上限');
            if(page.fingerprint&&!/^[0-9a-f]{64}$/.test(page.fingerprint))throw new Error('缓存页原件版本无效');entries.push([`page-${page.file}-${page.page}`,{blob:new Blob([blob],{type:'image/png'}),pages:page.pages,canonical:page.canonical===true,fingerprint:page.fingerprint||''}]);
        }
        const missingUploads=(bundle.uploads||[]).filter(item=>!ids.has(item.id));if(missingUploads.length)throw new Error('包内缺少待上传附件，不能恢复成完整记录；请回原手机重新导出');
        prepared={bundle,entries};el('rescueSummary').textContent=`${bundle.operations.length} 项学习修改、${bundle.assistant.pending.length} 项对话/记忆、${ids.size} 个附件。未缓存附件 ${bundle.missingFiles?.length||0} 个。恢复保留操作编号以防重复，已有修改可能在联网后需要选择冲突。`;
        el('rescueIncludeCached').checked=false;if(!el('rescueDialog').open)el('rescueDialog').showModal();
    }
    async function restore(){
        if(!prepared)return;const button=el('rescueConfirm');button.disabled=true;
        try{const {bundle,entries}=prepared;if(!DeviceAI.withRescue)throw new Error('页面组件尚未更新，请刷新后重试');const result=await DeviceAI.withRescue(bundle.assistant,assistantEntries=>PrepLocal.restoreRescue(bundle,[...entries,...assistantEntries],{includeCached:el('rescueIncludeCached').checked}));el('rescueDialog').close();prepared=null;PrepLocal.toast(`已恢复到本机，${result.operations} 项学习修改待同步${result.draftConflicts?'；'+result.draftConflicts+' 份不同草稿已双版本保留，可在“本机草稿比较”选择':''}`);await refresh();}
        catch(error){PrepLocal.toast('恢复未完成：'+error.message+'；没有部分写入学习记录和助手记忆，可修正后重试');}finally{button.disabled=false;}
    }
    async function cacheSelection(){
        const records=Object.values(state).filter(Array.isArray).flat().filter(record=>record&&record.id&&((record.files||[]).length||record.fileId));
        const list=el('offlineCacheList');list.replaceChildren();
        for(const record of records){const row=document.createElement('label');row.className='offline-cache-row';row.innerHTML=`<input type="checkbox" value="${esc(record.id)}" /><span>${esc(record.title||record.name||'未命名资料')}</span>`;list.append(row);}
        if(!records.length)list.textContent='暂无含附件的学习资料';el('offlineCacheDialog')._records=records;el('offlineCacheDialog').showModal();
    }
    async function cacheChosen(){
        const ids=new Set([...el('offlineCacheList input:checked')].map(input=>input.value)),records=el('offlineCacheDialog')._records.filter(record=>ids.has(record.id));let count=0;
        const files=new Map();for(const record of records)for(const file of fileReferences(record).values())files.set(file.id,file);
        const size=[...files.values()].reduce((total,file)=>total+(Number(file.size)||0),0);
        if(size>512*1024**2)throw new Error('所选原件超过本次 512 MB 缓存预算，请分批选择；大 PDF 可先打开所需页面，已打开页面会单独缓存');
        const estimate=await navigator.storage?.estimate?.();if(estimate?.quota&&size>estimate.quota-estimate.usage)throw new Error('本机可用缓存空间不足，请减少选择或先打开所需页面');
        for(const file of files.values()){await getFile(file.id);count++;el('offlineCacheProgress').textContent=`已缓存 ${count} 个附件`;}
        el('offlineCacheProgress').textContent=`本次已缓存 ${count} 个附件`;await refresh();
    }
    function setup(){
        const section=document.createElement('section');section.id='offlineSafety';section.className='safety-workspace';
        section.innerHTML='<header><h3><i class="fas fa-mobile-screen"></i> 本机离线保护</h3><button class="btn btn-icon btn-ghost" id="offlineRefresh" title="刷新本机状态" aria-label="刷新本机状态"><i class="fas fa-rotate"></i></button></header><dl class="safety-diagnostics"><div><dt>本机占用 / 配额</dt><dd id="offlineCapacity"></dd></div><div><dt>持久存储</dt><dd id="offlinePersistence"></dd></div><div class="safety-diagnostic-error"><dt>待同步</dt><dd id="offlinePending"></dd></div></dl><p id="offlineCapabilities"></p><div class="safety-entry-buttons"><button class="btn btn-ghost" id="offlinePersist"><i class="fas fa-shield"></i> 申请持久存储</button><button class="btn btn-ghost" id="offlineCache"><i class="fas fa-download"></i> 选择离线资料</button><button class="btn btn-primary" id="offlineExport"><i class="fas fa-file-export"></i> 导出本机紧急包</button><button class="btn btn-ghost" id="offlineImport"><i class="fas fa-file-import"></i> 恢复紧急包</button><input id="offlineImportFile" type="file" accept=".zip" hidden /></div><p id="offlineProgress" role="status"></p>';
        el('view-connection').append(section);
        const rescue=document.createElement('dialog');rescue.id='rescueDialog';rescue.className='safety-dialog';rescue.innerHTML='<header class="dialog-header"><h3>恢复到本机</h3><button class="btn btn-icon" title="返回" aria-label="返回"><i class="fas fa-xmark"></i></button></header><div class="dialog-body"><p id="rescueSummary"></p><label class="workflow-checkbox"><input id="rescueIncludeCached" type="checkbox" /> 同时补齐本机没有的已缓存学习记录，并等待同步</label><div class="dialog-actions"><button class="btn btn-ghost" id="rescueCancel">退回</button><button class="btn btn-primary" id="rescueConfirm">继续并恢复</button></div></div>';document.body.append(rescue);
        const cache=document.createElement('dialog');cache.id='offlineCacheDialog';cache.className='safety-dialog';cache.innerHTML='<header class="dialog-header"><h3>选择离线资料</h3><button class="btn btn-icon" title="返回" aria-label="返回"><i class="fas fa-xmark"></i></button></header><div class="dialog-body"><div id="offlineCacheList"></div><p id="offlineCacheProgress" role="status"></p><div class="dialog-actions"><button class="btn btn-ghost" id="offlineCacheCancel">返回</button><button class="btn btn-primary" id="offlineCacheSave">缓存所选附件</button></div></div>';document.body.append(cache);
        rescue.querySelector('header button').onclick=el('rescueCancel').onclick=()=>{rescue.close();prepared=null;};el('rescueConfirm').onclick=restore;
        cache.querySelector('header button').onclick=el('offlineCacheCancel').onclick=()=>cache.close();el('offlineCacheSave').onclick=()=>run(cacheChosen);
        el('offlineRefresh').onclick=refresh;el('offlinePersist').onclick=persistence;el('offlineExport').onclick=()=>run(exportBundle);el('offlineCache').onclick=cacheSelection;
        el('offlineImport').onclick=()=>el('offlineImportFile').click();el('offlineImportFile').onchange=event=>{const file=event.target.files[0];event.target.value='';run(()=>prepare(file));};
        addEventListener('yantu-sync',()=>refresh());refresh();
    }
    const previous=PrepLocal.ready;PrepLocal.ready=()=>{previous();setup();};
    window.OfflineSafety={refresh,exportBundle,prepare,restore,cacheSelection,cacheChosen};
})();
