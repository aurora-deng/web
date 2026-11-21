(() => {
    const W=Workspace,{el,button,icon}=W;
    let saved,request,busy=false,dirty=false,actionError='';
    const size=bytes=>Number(bytes||0)>=1024**3?(bytes/1024**3).toFixed(2)+' GB':(Number(bytes||0)/1024**2).toFixed(1)+' MB';
    function render(value){
        saved=value;const panel=el('resourceBackupPanel'),form=el('resourceBackupForm');panel.hidden=!value.available;
        if(!dirty)for(const key of ['enabled','autoRestore','keepRawCopies'])form.elements[key].checked=Boolean(value[key]);
        for(const input of form.elements)input.disabled=busy||!value.local;
        const phase=({idle:'尚未整理',compressing:'正在压缩',complete:'归档完成',partial:'部分完成',paused:'已暂停',error:'等待处理'}[value.phase]||'等待整理');
        el('resourceBackupPhase').textContent=phase+(value.currentName?' · '+value.currentName:'');
        el('resourceBackupCount').textContent=`${value.completed||0} / ${value.registeredFiles||0}`;
        el('resourceBackupOriginal').textContent=size(value.registeredBytes);
        el('resourceBackupSize').textContent=size(value.archiveBytes);
        el('resourceBackupSaved').textContent=size(value.savedBytes);
        const progress=el('resourceBackupProgress');progress.max=Math.max(1,value.registeredBytes||0);progress.value=Math.min(progress.max,(value.originalBytes||0)+(value.currentBytes||0));
        el('resourceBackupError').textContent=[actionError,value.lastError,value.restoreError,value.errorCount?'未完成 '+value.errorCount+' 项':'',value.rawRetained&&!value.keepRawCopies?'另有 '+value.rawRetained+' 个备用原件保留':''].filter(Boolean).join(' · ');
        const controls=el('resourceBackupActions');controls.querySelector('[data-action=run]').disabled=busy||!value.local||!value.enabled||value.running;
        el('resourceBackupLocal').textContent=value.local?'':'设置仅可在电脑本机修改';
    }
    async function refresh(){
        if(request)return request;
        request=PrepLocal.jsonRequest('/api/resource-backup').then(render).catch(error=>{if(error.status!==404)el('resourceBackupError').textContent=error.message;});
        try{await request;}finally{request=null;}
    }
    async function save(event){
        event.preventDefault();if(busy||!saved?.local)return;const form=event.target;
        const values=Object.fromEntries(['enabled','autoRestore','keepRawCopies'].map(key=>[key,form.elements[key].checked]));
        if(!values.keepRawCopies&&(saved.keepRawCopies||!saved.enabled)&&!confirm('压缩包通过完整性校验后，移除相同的未压缩备用副本？主资源库不删除，失败时原副本保留。'))return;
        actionError='';busy=true;render(saved);
        try{const value=await PrepLocal.jsonRequest('/api/resource-backup/settings',{...values,confirmation:'COMPRESS_RESOURCE_BACKUPS'});dirty=false;render(value);PrepLocal.toast(values.enabled?'资源自动压缩已开启':'自动整理已暂停，已有压缩包保留');}
        catch(error){actionError=error.message;}finally{busy=false;if(saved)render(saved);await refresh();}
    }
    async function run(){if(busy||!saved?.local)return;actionError='';busy=true;render(saved);try{const value=await PrepLocal.jsonRequest('/api/resource-backup/run',{});render(value);}catch(error){actionError=error.message;}finally{busy=false;if(saved)render(saved);await refresh();}}
    function setup(){
        const section=document.createElement('section');section.id='resourceBackupPanel';section.className='safety-workspace';section.hidden=true;
        section.innerHTML='<header><h3>电子资源压缩备份</h3><div id="resourceBackupRefresh"></div></header><dl class="safety-diagnostics"><div><dt>已归档文件</dt><dd id="resourceBackupCount"></dd></div><div><dt>原始资源总量</dt><dd id="resourceBackupOriginal"></dd></div><div><dt>压缩副本</dt><dd id="resourceBackupSize"></dd></div><div><dt>归档体积节省</dt><dd id="resourceBackupSaved"></dd></div></dl><p id="resourceBackupPhase" role="status"></p><progress id="resourceBackupProgress" aria-label="资源归档进度" style="width:100%"></progress><form id="resourceBackupForm"><label class="workflow-checkbox"><input type="checkbox" name="enabled" /> 自动压缩备用电子资源</label><label class="workflow-checkbox"><input type="checkbox" name="autoRestore" /> 使用时自动恢复缺失原件</label><label class="workflow-checkbox"><input type="checkbox" name="keepRawCopies" /> 同时保留未压缩备用副本</label><div id="resourceBackupActions" class="workspace-actions"></div></form><p id="resourceBackupLocal"></p><p id="resourceBackupError" class="safety-warning" role="status"></p>';
        el('view-connection').append(section);el('resourceBackupRefresh').append(icon('刷新资源备份状态','rotate',refresh));
        const saveButton=button('保存设置','check',()=>{} ,'btn-primary');saveButton.type='submit';saveButton.onclick=null;
        const runButton=button('立即整理','boxes-packing',run);runButton.dataset.action='run';
        el('resourceBackupActions').append(saveButton,button('撤回修改','rotate-left',()=>{dirty=false;actionError='';if(saved)render(saved);}),runButton);
        el('resourceBackupForm').onsubmit=save;el('resourceBackupForm').oninput=()=>{dirty=true;};refresh();
        const previous=renderView;renderView=function(id,...args){const result=previous.call(this,id,...args);if(id==='connection')refresh();return result;};
        setInterval(()=>{if(!document.hidden&&document.body.dataset.view==='connection'&&!dirty)refresh();},5000);
    }
    addEventListener('workspace-ready',setup,{once:true});
    window.ResourceBackupUI={refresh};
})();
