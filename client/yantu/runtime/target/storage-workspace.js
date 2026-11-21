(() => {
    const W=Workspace,{el,esc,button,icon}=W;
    let cleanup,local=false,page=0,selected=new Set(),heldKey='',held,choosing=false;
    const mb=value=>(value/1024**2).toFixed(2)+' MB';
    const draftPattern=/^(reader-note-draft-|reader-region-draft-|richnote-draft-|card-draft-|board-draft-|practice-local-|planning-draft-|planning-focus-draft-|subject-tools-draft-|exam-local-|workbench-draft-|assistant-local-)[A-Za-z0-9_.-]{1,201}$/;
    function draftText(value){
        if(value?.session)return value.session.title+'\n\n'+(value.session.items||[]).map(item=>item.title+'\n'+(item.answer||'尚未作答')).join('\n\n');
        if(value?.board)return value.board.title+'\n\n'+(value.board.nodes||[]).map(node=>node.label+'\n'+(node.text||'')).join('\n\n');
        const item=value?.values||value||{},parts=[];for(const[key,label]of[['title','标题'],['content','内容'],['front','正面'],['back','背面']])if(typeof item[key]==='string')parts.push(label+'\n'+item[key]);
        return parts.join('\n\n')||JSON.stringify(value,null,2);
    }
    async function refreshDraftCount(){const entries=await DeviceStore.entries('rescue-draft-conflict-'),count=entries.filter(([,value])=>!value.resolved).length;el('storageDrafts')?.querySelector('span')?.replaceChildren(document.createTextNode('本机草稿比较'+(count?' · '+count+' 份待选':'')));}
    async function showDrafts(){
        const list=el('storageDraftList');list.replaceChildren();held=null;heldKey='';el('storageDraftCompare').hidden=true;
        for(const[key,value]of(await DeviceStore.entries('rescue-draft-conflict-')).reverse()){const label=(value.incoming?.board?.title||value.incoming?.session?.title||value.incoming?.values?.title||value.incoming?.title||'本机草稿')+' · '+(value.resolved?'已选择':'待选择');list.append(button(label,'code-compare',()=>{heldKey=key;held=value;el('storageDraftCurrent').textContent=draftText(value.current);el('storageDraftIncoming').textContent=draftText(value.incoming);el('storageDraftCompare').hidden=false;}));}
        if(!list.children.length)list.textContent='暂无不同版本草稿';if(!el('storageDraftDialog').open)el('storageDraftDialog').showModal();await refreshDraftCount();
    }
    async function chooseDraft(incoming){
        if(choosing||!held||!draftPattern.test(held.key))return;
        const saved=structuredClone(held),key=heldKey;
        if(!confirm(incoming?'采用导入草稿作为当前草稿？当前内容仍保留在这份比较记录中。':'保留当前草稿？导入版本仍保留在这份比较记录中。'))return;
        choosing=true;try{
            const latest=await DeviceStore.get(saved.key),value={...saved,resolved:incoming?'incoming':'current',resolvedAt:PrepLocal.nowDate().toISOString()},entries=[];
            if(incoming){value.alternatives=[...saved.alternatives||[]];if(latest!==undefined&&JSON.stringify(latest)!==JSON.stringify(saved.current)&&!value.alternatives.some(item=>JSON.stringify(item)===JSON.stringify(latest)))value.alternatives.push(latest);if(value.alternatives.length>50)throw new Error('本次比较保留的版本过多，请先导出两个版本并整理');entries.push([saved.key,saved.incoming]);}
            entries.push([key,value]);await DeviceStore.batch(entries,{expected:[[saved.key,latest],[key,saved]]});await showDrafts();PrepLocal.toast('草稿选择已保存，重新打开对应编辑器即可继续');
        }finally{choosing=false;}
    }
    async function openCleanup(){
        await PrepLocal.flush();if(PrepLocal.hasPending())throw new Error('请先完成本机待同步内容，再预览电脑附件占用');
        const info=await PrepLocal.jsonRequest('/api/info');local=info.local===true;cleanup=await PrepLocal.jsonRequest('/api/workspace/cleanup');selected.clear();page=0;el('storageCleanupAck').checked=false;el('storageCleanupSearch').value='';renderCleanup();if(!el('storageCleanupDialog').open)el('storageCleanupDialog').showModal();
    }
    function renderCleanup(){
        if(!cleanup)return;const query=el('storageCleanupSearch').value.trim().toLocaleLowerCase(),values=cleanup.orphans.filter(file=>file.name.toLocaleLowerCase().includes(query));page=Math.min(page,Math.max(0,Math.ceil(values.length/50)-1));const list=el('storageCleanupList');list.replaceChildren();
        for(const file of values.slice(page*50,page*50+50)){const row=document.createElement('label');row.className='storage-file-row';const input=document.createElement('input');input.type='checkbox';input.checked=selected.has(file.id);input.disabled=!file.eligible;input.onchange=()=>{input.checked?selected.add(file.id):selected.delete(file.id);renderCleanup();};const name=document.createElement('span');name.textContent=file.name;const size=document.createElement('small');size.textContent=mb(file.bytes)+(file.eligible?'':' · 上传不足一小时');row.append(input,name,size);list.append(row);}
        if(!values.length)list.textContent='暂无匹配的未引用附件';el('storageCleanupSummary').textContent=`电脑附件 ${cleanup.files} 个 · ${mb(cleanup.bytes)}；当前记录或保护历史引用 ${cleanup.protectedFiles} 个；未引用 ${cleanup.orphans.length} 个。`;
        el('storageCleanupPage').textContent=`${page+1} / ${Math.max(1,Math.ceil(values.length/50))}`;el('storageCleanupPrevious').disabled=!page;el('storageCleanupNext').disabled=(page+1)*50>=values.length;el('storageCleanupSelection').textContent=`已选 ${selected.size} 个 · ${mb(cleanup.orphans.filter(file=>selected.has(file.id)).reduce((sum,file)=>sum+file.bytes,0))}`;
        el('storageCleanupDelete').disabled=!local||!selected.size||!el('storageCleanupAck').checked;el('storageCleanupDownload').disabled=!selected.size;el('storageCleanupOwner').textContent=local?'':'永久清理只允许在电脑本机执行';
    }
    async function downloadOrphans(){
        const files=cleanup?.orphans.filter(file=>selected.has(file.id))||[];if(!files.length)return;if(files.reduce((sum,file)=>sum+file.bytes,0)>256*1024**2)throw new Error('本次原件包最多 256 MB，请分批选择');const zip=new JSZip(),manifest=[];
        for(const file of files){el('storageCleanupProgress').textContent='正在下载 '+file.name;const blob=await getFile(file.id);zip.file('files/'+file.id,blob);const sha256=[...new Uint8Array(await crypto.subtle.digest('SHA-256',await blob.arrayBuffer()))].map(n=>n.toString(16).padStart(2,'0')).join('');manifest.push({...file,sha256,path:'files/'+file.id});}
        zip.file('附件清单.json',JSON.stringify({format:'yantu-unreferenced-files-v1',createdAt:PrepLocal.nowDate().toISOString(),files:manifest},null,2));downloadBlob('研途-未引用原件.zip',await zip.generateAsync({type:'blob',compression:'STORE'}));el('storageCleanupProgress').textContent='已下载原件包，未加密；电脑上的附件尚未删除';
    }
    async function deleteOrphans(){
        if(!local||!selected.size||!el('storageCleanupAck').checked)return;const ids=[...selected];if(!confirm(`永久删除 ${ids.length} 个已选未引用附件？这些原件不能通过回收站恢复；已关联的学习记录不删除。`))return;
        el('storageCleanupDelete').disabled=true;const result=await PrepLocal.jsonRequest('/api/workspace/cleanup',{ids,revision:cleanup.revision,confirmation:'DELETE_UNREFERENCED_FILES'});await openCleanup();el('storageCleanupProgress').textContent=`已永久清理 ${result.deleted} 个未引用附件`;
    }
    function setupLocal(){
        if(el('storageDrafts')||!el('offlineSafety'))return;const entry=button('本机草稿比较','code-compare',showDrafts);entry.id='storageDrafts';el('offlineSafety').querySelector('.safety-entry-buttons').append(entry);
        W.dialog('storageDraftDialog','本机草稿比较','<div id="storageDraftList" class="workspace-list"></div><div id="storageDraftCompare" hidden><div class="storage-compare-columns"><section><h4>本机原有版本</h4><pre id="storageDraftCurrent"></pre></section><section><h4>导入版本</h4><pre id="storageDraftIncoming"></pre></section></div><div id="storageDraftActions" class="workspace-actions"></div></div>');
        el('storageDraftActions').append(button('保留当前草稿','check',()=>chooseDraft(false),'btn-primary'),button('采用导入草稿','file-import',()=>chooseDraft(true)),button('导出两个版本','download',()=>{if(held)downloadBlob('研途-草稿双版本.json',new Blob([JSON.stringify(held,null,2)],{type:'application/json'}));}),icon('移除这份比较记录','trash',async()=>{if(heldKey&&confirm('移除这份比较记录及其中保留的其他版本？当前正在使用的草稿不删除，请先导出需要保留的版本。')){await DeviceStore.put(heldKey,undefined);await showDrafts();}}));
        addEventListener('yantu-sync',()=>refreshDraftCount().catch(()=>{}));refreshDraftCount().catch(()=>{});
    }
    function setupCleanup(){
        const entry=button('附件占用与清理','hard-drive',openCleanup);el('safetyWorkspace')?.querySelector('.safety-entry-buttons')?.append(entry);
        W.dialog('storageCleanupDialog','电脑附件占用','<p id="storageCleanupSummary"></p><p class="safety-warning">未同步设备的草稿不在电脑引用核对范围内。这些未引用原件也不保证包含在普通学习备份中，可先下载原件包。</p><input id="storageCleanupSearch" type="search" aria-label="搜索未引用附件" placeholder="搜索附件名" /><div id="storageCleanupList"></div><nav class="workspace-search-pager"><button id="storageCleanupPrevious" class="btn btn-icon" aria-label="上一页" title="上一页"><i class="fas fa-chevron-left"></i></button><span id="storageCleanupPage"></span><button id="storageCleanupNext" class="btn btn-icon" aria-label="下一页" title="下一页"><i class="fas fa-chevron-right"></i></button></nav><p id="storageCleanupSelection"></p><p id="storageCleanupOwner"></p><label class="workflow-checkbox"><input id="storageCleanupAck" type="checkbox" /> 已核对其他设备的未同步草稿，并接受永久删除所选原件</label><div id="storageCleanupActions" class="workspace-actions"></div><p id="storageCleanupProgress" role="status"></p>');
        const download=button('下载选中原件','download',downloadOrphans);download.id='storageCleanupDownload';const remove=button('永久清理所选','trash',deleteOrphans,'btn-danger');remove.id='storageCleanupDelete';el('storageCleanupActions').append(download,remove);el('storageCleanupSearch').oninput=()=>{page=0;renderCleanup();};el('storageCleanupPrevious').onclick=()=>{page--;renderCleanup();};el('storageCleanupNext').onclick=()=>{page++;renderCleanup();};el('storageCleanupAck').onchange=renderCleanup;
    }
    const ready=PrepLocal.ready;PrepLocal.ready=()=>{ready();setupLocal();};addEventListener('workspace-ready',setupCleanup,{once:true});window.WorkspaceStorage={openCleanup,showDrafts,chooseDraft,renderCleanup};
})();
