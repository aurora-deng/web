(() => {
    const el=id=>document.getElementById(id),copy=value=>JSON.parse(JSON.stringify(value));
    const esc=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    let active,generation=0,viewerGeneration=0,viewing,down=false,pendingStroke,cropSelection,drawFrame=0,viewerPositionTimer,prefetchTimer,inlineGeneration=0,inlineController,exportingPDF=false;
    let textEditor,viewerDrawFrame=0,viewerPointer,mediaPointer;
    const pageMarks=(file,page)=>MediaPreview.marks(state.readingMarks,file,page);
    const readingNotes=file=>(state.notes||[]).filter(note=>note.sourceFileId===file.id);
    const canvas=()=>el('mediaCanvas');
    const currentFile=()=>active?.file;
    const key=()=>currentFile().id+':'+active.page;
    const defaults=()=>({crop:[0,0,1,1],rotation:0,strokes:[]});
    const prepareText=edit=>window.AnnotationText?.prepareEdit(edit)||Promise.resolve();
    const textLoads=new WeakMap();
    function hydrateText(edit,refresh){
        if(!window.AnnotationText||AnnotationText.ready(edit)||textLoads.has(edit))return;
        const work=prepareText(edit).then(refresh).catch(error=>PrepLocal.toast(error.message)).finally(()=>textLoads.delete(edit));textLoads.set(edit,work);
    }
    async function prepareRecordText(record,file){
        const before=JSON.stringify(record.fileEdits||{}),edits=copy(record.fileEdits||{});
        for(const [key,edit]of Object.entries(edits))if(key.startsWith(file.id+':'))await prepareText(edit);
        if(JSON.stringify(edits)===before)return;
        const latest=findRecord(record.id);if(!latest||JSON.stringify(latest.fileEdits||{})!==before)throw new Error('批注已在其他位置改变，请重新打开后导出；未覆盖任何内容');
        latest.fileEdits=edits;saveState();
    }
    const supported=file=>file&&(file.type?.startsWith('image/')||file.type==='application/pdf'||/\.pdf$/i.test(file.name));
    const pdf=file=>file.type==='application/pdf'||/\.pdf$/i.test(file.name);
    function pagePoint(event,surface,image,edit){
        const rect=surface.getBoundingClientRect(),[cx,cy,cw,ch]=edit.crop||[0,0,1,1],angle=(edit.rotation||0)*Math.PI/180;
        const x=(event.clientX-rect.left)/rect.width*surface.width-surface.width/2,y=(event.clientY-rect.top)/rect.height*surface.height-surface.height/2;
        return [Math.max(0,Math.min(1,(Math.cos(angle)*x+Math.sin(angle)*y+cw*image.width/2+cx*image.width)/image.width)),Math.max(0,Math.min(1,(-Math.sin(angle)*x+Math.cos(angle)*y+ch*image.height/2+cy*image.height)/image.height))];
    }
    const textAt=(edit,point,image)=>AnnotationSelection.hit(edit,point,image);
    const ownerEdit=owner=>owner===viewing?owner.draft.edit:owner.edit;
    const ownerCheckpoint=owner=>owner===viewing?viewerCheckpoint():checkpoint();
    const ownerChanged=owner=>owner===viewing?viewerChanged():mark();
    function selectedText(owner){const stroke=owner?.selectedText;if(stroke&&!ownerEdit(owner).strokes.includes(stroke))owner.selectedText=null;return owner?.selectedText;}
    function selectText(owner,stroke){
        owner.selectedText=stroke||null;owner.textStyleSession=null;
        const prefix=owner===viewing?'viewer':'media',font=el(prefix+'FontSize'),color=el(prefix==='viewer'?'viewerInkColor':'mediaColor');
        if(stroke){color.value=stroke.color;const size=String(Math.round((stroke.fontSize||.025)*1000));font.querySelector('[data-text-size]')?.remove();if(![...font.options].some(option=>option.value===size)){const option=new Option(size,size);option.dataset.textSize='true';font.append(option);}font.value=size;}
    }
    function deleteSelectedText(owner){
        const stroke=selectedText(owner);if(!stroke)return;finishText(false);ownerCheckpoint(owner);ownerEdit(owner).strokes=ownerEdit(owner).strokes.filter(value=>value!==stroke);selectText(owner,null);ownerChanged(owner);
    }
    function styleSelectedText(owner,key,value,complete=false){
        if(!owner?.image)return;finishText();const stroke=selectedText(owner);if(!stroke)return;
        if(stroke[key]!==value){if(owner.textStyleSession!==key){ownerCheckpoint(owner);owner.textStyleSession=key;}stroke[key]=value;delete stroke.mathRaster;ownerChanged(owner);}
        if(complete)owner.textStyleSession=null;
    }
    function bindTextStyle(prefix,owner){
        const color=el(prefix==='viewer'?'viewerInkColor':'mediaColor'),font=el(prefix+'FontSize');
        color.addEventListener('input',()=>styleSelectedText(owner(),'color',color.value));
        color.addEventListener('change',()=>styleSelectedText(owner(),'color',color.value,true));
        font.addEventListener('change',()=>styleSelectedText(owner(),'fontSize',Number(font.value)/1000,true));
    }
    function renderTextSelection(surface,owner){
        const wrap=surface.parentElement,stroke=selectedText(owner);let selection=wrap.querySelector('.media-text-selection');
        const prefix=owner===viewing?'viewer':'media',remove=el(prefix+'DeleteText');if(remove)remove.disabled=!stroke||owner.original;
        if(!stroke||owner.original){selection?.remove();return;}
        const edit={...ownerEdit(owner),rotation:((ownerEdit(owner).rotation||0)+(owner===viewing?owner.rotation:0))%360};
        const rect=AnnotationSelection.project(AnnotationSelection.bounds(stroke,owner.image),owner.image,edit,surface);if(!rect){selection?.remove();return;}
        if(!selection){selection=document.createElement('div');selection.className='media-text-selection';selection.innerHTML='<div class="media-selection-actions"><button type="button" class="btn btn-icon" aria-label="编辑选中文字批注" title="编辑文字"><i class="fas fa-pen-to-square"></i></button><button type="button" class="btn btn-icon" aria-label="删除选中文字批注" title="删除文字 (Delete)"><i class="fas fa-trash"></i></button></div>';wrap.append(selection);
            selection.querySelector('[title="编辑文字"]').onclick=event=>{event.stopPropagation();const current=selectedText(owner);if(!current)return;const bounds=surface.getBoundingClientRect(),eventPoint={clientX:bounds.left+12,clientY:bounds.top+12};if(owner===viewing)viewerText(eventPoint,surface,current.points[0],current);else mediaText(eventPoint,current.points[0],current);};
            selection.querySelector('[title="删除文字 (Delete)"]').onclick=event=>{event.stopPropagation();deleteSelectedText(owner);};
        }
        Object.assign(selection.style,{left:rect[0]+'px',top:rect[1]+'px',width:rect[2]+'px',height:rect[3]+'px'});selection.classList.toggle('actions-below',rect[1]<38);
    }
    function finishText(commit=true){
        if(!textEditor)return;const saved=textEditor;textEditor=null;
        clearTimeout(saved.timer);const text=saved.input.value.trim();saved.form.remove();if(commit&&text)saved.apply(text);
    }
    function composeText(surface,event,stroke,apply){
        finishText();const bounds=surface.getBoundingClientRect(),wrap=surface.parentElement;
        const form=document.createElement('form');form.className='media-text-editor';form.innerHTML='<textarea rows="4" maxlength="2000" aria-label="输入文字批注"></textarea><section class="media-text-preview annotation-rich" aria-label="批注效果预览"></section><div><button type="button" class="btn btn-icon" title="取消文字批注 (Esc)" aria-label="取消文字批注"><i class="fas fa-xmark"></i></button><button type="submit" class="btn btn-icon" title="确认文字批注 (Ctrl+Enter)" aria-label="确认文字批注"><i class="fas fa-check"></i></button></div>';
        const stage=surface.closest('#viewerStage,#mediaStage');
        Object.assign(form.style,{left:Math.max(0,Math.min(event.clientX-bounds.left,Math.max(0,bounds.width-360)))+'px',top:Math.max(0,Math.min(event.clientY-bounds.top,Math.max(0,bounds.height-360)))+'px',maxWidth:Math.max(100,Math.min(bounds.width,innerWidth-32))+'px',maxHeight:Math.max(160,Math.min(innerHeight*.7,(stage?.clientHeight||500)-20))+'px'});
        const input=form.querySelector('textarea');input.value=stroke?.text||'';wrap.append(form);textEditor={form,input,apply};
        const saved=textEditor,preview=form.querySelector('.media-text-preview');
        const refresh=()=>window.AnnotationText?.render(input.value,preview).catch(error=>{if(textEditor===saved)preview.title=error.message;});
        input.oninput=()=>{clearTimeout(saved.timer);saved.timer=setTimeout(refresh,160);};refresh();
        form.onsubmit=event=>{event.preventDefault();finishText();};form.querySelector('[type=button]').onclick=()=>finishText(false);
        input.onkeydown=event=>{if(event.isComposing)return;if(event.key==='Escape'){event.preventDefault();event.stopPropagation();finishText(false);}else if(event.key==='Enter'&&(event.ctrlKey||event.metaKey)){event.preventDefault();finishText();}};input.focus();
    }
    function checkpoint(){active.undo.push(copy(active.edit));if(active.undo.length>30)active.undo.shift();active.redo=[];}
    function mark(){active.dirty=true;draw();}
    function queueDraw(){if(drawFrame)return;const saved=active;drawFrame=requestAnimationFrame(()=>{drawFrame=0;if(active===saved)draw();});}
    function geometry(){const image=active.image,[x,y,w,h]=active.edit.crop;return {sx:x*image.width,sy:y*image.height,sw:w*image.width,sh:h*image.height,angle:active.edit.rotation*Math.PI/180};}
    function draw(){
        if(!active?.image)return;
        const saved=active,edit=saved.edit;hydrateText(edit,()=>{if(active===saved&&saved.edit===edit)draw();});
        const element=MediaPreview.paint(canvas(),active.image,active.edit,pageMarks(active.file,active.page));
        if(cropSelection){const {sx,sy,sw,sh,angle}=geometry(),ctx=element.getContext('2d'),[x,y,w,h]=cropSelection;ctx.save();ctx.translate(element.width/2,element.height/2);ctx.rotate(angle);ctx.translate(-sw/2-sx,-sh/2-sy);ctx.strokeStyle='#268470';ctx.lineWidth=3;ctx.setLineDash([12,8]);ctx.strokeRect(x*active.image.width,y*active.image.height,w*active.image.width,h*active.image.height);ctx.restore();}
        fit();renderTextSelection(element,active);el('mediaUndo').disabled=!active.undo.length;el('mediaRedo').disabled=!active.redo.length;el('mediaApplyCrop').disabled=!cropSelection;el('mediaSave').disabled=!active.dirty;
    }
    function fit(){if(!active?.image)return;const stage=el('mediaStage'),element=canvas();const width=Math.min(element.width,Math.max(100,stage.clientWidth-32),Math.max(140,stage.clientHeight-20)*element.width/element.height)*Number(el('mediaZoom').value)/100;el('mediaCanvasWrap').style.width=width+'px';renderTextSelection(element,active);}
    function point(event){return pagePoint(event,canvas(),active.image,active.edit);}
    async function response(path){const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),65000);try{const result=await PrepTransport.fetch(path,{credentials:'same-origin',signal:controller.signal});if(!result.ok)throw new Error((await result.json().catch(()=>({}))).error||'附件处理失败');return result;}finally{clearTimeout(timeout);}}
    async function load(){
        const request=++generation,saved=active,file=StudyLibrary.files(findRecord(active.id)||{})[active.index];
        active.file=file;active.image?.close?.();active.image=null;down=false;cropSelection=null;el('mediaSave').disabled=true;
        if(!supported(file)){canvas().width=1;canvas().height=1;el('mediaFilename').textContent=file?.name||'附件已移除';el('mediaStatus').textContent='此附件可下载原件，暂不支持批注';return;}
        el('mediaStatus').textContent='正在读取页面…';
        el('mediaFilename').textContent=file.name;el('mediaGroup').textContent=`附件 ${active.index+1} / ${StudyLibrary.files(findRecord(active.id)).length}`;
        el('mediaPDFPages').hidden=!pdf(file);el('mediaOCRResult').hidden=true;
        el('mediaExportFormat').querySelector('[value=pdf]').disabled=!pdf(file);if(!pdf(file))el('mediaExportFormat').value='png';
        try{
            const {image:bitmap,pages}=await MediaPreview.image(file,saved.page);
            if(request!==generation||active!==saved){bitmap.close?.();return;}
            active.image=bitmap;active.pages=pages;active.edit={...defaults(),...copy(findRecord(active.id)?.fileEdits?.[key()]||{})};active.edit.strokes||=[];active.dirty=false;active.undo=[];active.redo=[];
            el('mediaPage').value=active.page;el('mediaPage').max=pages;el('mediaPageCount').textContent='/ '+pages;el('mediaStatus').textContent='';draw();
        }catch(error){if(request===generation){canvas().width=1;canvas().height=1;el('mediaStatus').textContent=error.message;}}
    }
    function leave(){finishText();return !active?.dirty||confirm('还有未保存的修改，放弃修改并继续？');}
    function close(){if(leave())el('mediaDialog').close();}
    async function open(id,index=0,page=1){if(el('mediaDialog').open&&!leave())return;active={id,index,page,dirty:false,tool:'view',undo:[],redo:[]};el('mediaDialog').querySelectorAll('[data-tool]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.tool==='view')));el('mediaZoom').value=100;if(!el('mediaDialog').open)el('mediaDialog').showModal();StudyLibrary.touch(id);await load();}
    async function moveFile(step){if(!leave())return;const files=StudyLibrary.files(findRecord(active.id));active.index=(active.index+step+files.length)%files.length;active.page=1;await load();}
    async function movePage(step){if(!leave())return;active.page=Math.max(1,Math.min(active.pages,active.page+step));await load();}
    async function save(){
        finishText();
        if(!active?.image)return false;
        const saved=active,edit=saved.edit,file=currentFile();
        try{await prepareText(edit);}catch(error){PrepLocal.toast(error.message);return false;}
        if(active!==saved||saved.edit!==edit)return false;
        const record=findRecord(saved.id);if(!record||!file||!StudyLibrary.files(record).some(item=>item.id===file.id)){PrepLocal.toast('原记录或附件已删除，请先从回收站恢复');return false;}
        record.fileEdits={...(record.fileEdits||{}),[key()]:copy(active.edit)};record.updatedAt=PrepLocal.nowDate().toISOString();saveState();active.dirty=false;draw();
        if(el('viewerDialog').open&&currentViewer.id===record.id)renderViewerMedia({preservePage:true});
        dispatchEvent(new CustomEvent('workspace-change',{detail:{collection:findRecordWithCol(record.id).col,id:record.id}}));
        PrepLocal.toast(PrepLocal.hasPending()?'批注已保留在本机，后台同步中':'批注已保存，原件未更改');
        PrepLocal.flush().catch(error=>PrepLocal.toast('批注已保留在本机：'+error.message));return true;
    }
    async function download(){
        const file=currentFile();if(!active?.image)return;
        try{
            finishText();const saved=active;await prepareText(saved.edit);if(active!==saved)return;
            if(pdf(file)&&el('mediaExportFormat').value==='pdf'){
                if(!await save())return;await prepareRecordText(findRecord(active.id),file);await PrepLocal.flush();if(PrepLocal.hasPending())throw new Error('完整 PDF 导出需要先连接电脑并完成同步，可先下载当前页图片');
                const result=await response('/api/media/export?'+new URLSearchParams({file:file.id,record:active.id,format:'pdf'}));downloadBlob(file.name.replace(/\.pdf$/i,'')+'-批注.pdf',await result.blob());
            }else{const selection=cropSelection,page=active.page;cropSelection=null;draw();canvas().toBlob(blob=>{if(blob)downloadBlob(file.name.replace(/\.[^.]+$/,'')+`-第${page}页-批注.png`,blob);},'image/png');cropSelection=selection;}
        }catch(error){PrepLocal.toast(error.message);}
    }
    async function ocr(){
        const file=currentFile(),button=el('mediaOCR'),request=generation,recordId=active.id;button.disabled=true;
        try{const result=await PrepLocal.jsonRequest('/api/assistant/attachment?'+new URLSearchParams({id:file.id,page:active.page}),undefined,{long:true});if(request!==generation)return;el('mediaOCRText').value=result.text||'';el('mediaOCRResult').hidden=false;el('mediaOCRResult').dataset.record=recordId;}
        catch(error){PrepLocal.toast(error.message);}finally{button.disabled=false;}
    }
    function organize(id){
        const record=findRecord(id);if(!record)return;let items=copy(StudyLibrary.files(record));
        const dialog=el('mediaOrderDialog');dialog.dataset.record=id;dialog._items=items;
        const render=()=>{el('mediaOrderList').replaceChildren();items.forEach((file,index)=>{const row=document.createElement('div');row.className='media-order-row';row.draggable=true;row.dataset.index=index;row.innerHTML=`<button type="button" class="btn btn-icon btn-ghost media-order-handle" title="拖动排序" aria-label="拖动排序"><i class="fas fa-grip-vertical"></i></button><span>${esc(file.name)}</span><button type="button" class="btn btn-icon btn-ghost" title="向前移动" aria-label="向前移动"><i class="fas fa-arrow-up"></i></button><button type="button" class="btn btn-icon btn-ghost" title="向后移动" aria-label="向后移动"><i class="fas fa-arrow-down"></i></button>`;const shift=step=>{const to=index+step;if(to<0||to>=items.length)return;[items[index],items[to]]=[items[to],items[index]];render();};row.querySelector('[title="向前移动"]').onclick=()=>shift(-1);row.querySelector('[title="向后移动"]').onclick=()=>shift(1);row.ondragstart=event=>event.dataTransfer.setData('text/plain',String(index));row.ondragover=event=>event.preventDefault();row.ondrop=event=>{event.preventDefault();const from=Number(event.dataTransfer.getData('text/plain'));if(Number.isInteger(from)&&from>=0&&from<items.length){items.splice(index,0,items.splice(from,1)[0]);render();}};
            const handle=row.querySelector('.media-order-handle');handle.onpointerdown=event=>{event.preventDefault();handle.setPointerCapture(event.pointerId);};handle.onpointerup=event=>{const target=document.elementFromPoint(event.clientX,event.clientY)?.closest('.media-order-row');if(target){const to=Number(target.dataset.index);items.splice(to,0,items.splice(index,1)[0]);render();}};el('mediaOrderList').append(row);});};
        dialog._save=async()=>{const current=findRecord(id);if(!current){PrepLocal.toast('记录已删除');return;}const latest=StudyLibrary.files(current);if(latest.map(file=>file.id).sort().join('|')!==items.map(file=>file.id).sort().join('|')){PrepLocal.toast('附件列表已变化，请重新打开排序');return;}current.files=items;saveState();dialog.close();StudyLibrary.refresh(findRecordWithCol(id).col);dispatchEvent(new CustomEvent('workspace-change'));PrepLocal.flush().catch(error=>PrepLocal.toast(error.message));};render();dialog.showModal();
    }
    function draftCamera(form,input){
        const tools=document.createElement('div');tools.className='media-capture-tools';tools.innerHTML='<button class="btn btn-ghost btn-sm" type="button"><i class="fas fa-camera"></i> 拍照添加</button><input type="file" accept="image/*" capture="environment" hidden /><div class="media-draft-list"></div>';
        input.after(tools);const camera=tools.querySelector('input');tools.querySelector('button').onclick=()=>camera.click();
        const render=()=>{const files=[...input.files],list=tools.querySelector('.media-draft-list');list.replaceChildren();files.forEach((file,index)=>{const row=document.createElement('div');row.className='media-order-row';row.draggable=true;row.dataset.index=index;row.innerHTML=`<i class="fas fa-grip-vertical"></i><span>${esc(file.name)}</span><button type="button" class="btn btn-icon btn-ghost" title="向前移动" aria-label="向前移动"><i class="fas fa-arrow-up"></i></button><button type="button" class="btn btn-icon btn-ghost" title="移除待上传文件" aria-label="移除待上传文件"><i class="fas fa-xmark"></i></button>`;const update=next=>{const transfer=new DataTransfer();next.forEach(file=>transfer.items.add(file));input.files=transfer.files;render();};row.querySelector('[title="向前移动"]').onclick=()=>{if(index){[files[index],files[index-1]]=[files[index-1],files[index]];update(files);}};row.querySelector('[title="移除待上传文件"]').onclick=()=>{files.splice(index,1);update(files);};row.ondragstart=event=>event.dataTransfer.setData('text/plain',index);row.ondragover=event=>event.preventDefault();row.ondrop=event=>{event.preventDefault();const from=Number(event.dataTransfer.getData('text/plain'));if(Number.isInteger(from)&&from>=0&&from<files.length){files.splice(index,0,files.splice(from,1)[0]);update(files);}};list.append(row);});};
        input.addEventListener('change',()=>{PrepLocal.resetUploads(form.id);render();});camera.onchange=()=>{const transfer=new DataTransfer();[...input.files,...camera.files].forEach(file=>transfer.items.add(file));input.files=transfer.files;camera.value='';PrepLocal.resetUploads(form.id);render();};form.addEventListener('reset',()=>setTimeout(render));
    }
    function viewerEdit(){return viewing?.draft?.edit||defaults();}
    function viewerCheckpoint(){const draft=viewing.draft;draft.undo.push(copy(draft.edit));if(draft.undo.length>30)draft.undo.shift();draft.redo=[];}
    function viewerChanged(){viewing.draft.dirty=true;paintViewer();}
    async function saveViewerDraft(notify=false){
        finishText();if(!viewing?.draft)return true;
        const saved=viewing,draft=saved.draft,edit=draft.edit,key=saved.file.id+':'+saved.page;
        try{await prepareText(edit);}catch(error){PrepLocal.toast(error.message);return false;}
        if(viewing!==saved||draft.edit!==edit)return false;const record=findRecord(saved.id);
        if(!draft.dirty&&JSON.stringify(draft.edit)===draft.base)return true;
        if(!record||!StudyLibrary.files(record).some(file=>file.id===saved.file.id)){PrepLocal.toast('原记录或附件已删除，当前批注未覆盖任何数据');return false;}
        const latest=JSON.stringify(record.fileEdits?.[key]||defaults());
        if(latest!==draft.base&&latest!==JSON.stringify(draft.edit)){
            el('viewerEditStatus').textContent='存在其他修改，当前草稿已保留';
            DeviceStore.put('viewer-conflict-'+key,{edit:copy(draft.edit),base:draft.base,updatedAt:PrepLocal.nowDate().toISOString()}).catch(()=>{});
            PrepLocal.toast('本页批注已在其他地方更改，请先下载当前页保留草稿，再重新打开；未覆盖最新内容');return false;
        }
        const old=record.fileEdits,updated=record.updatedAt;
        try{record.fileEdits={...(old||{}),[key]:copy(draft.edit)};record.updatedAt=PrepLocal.nowDate().toISOString();saveState();}
        catch(error){record.fileEdits=old;record.updatedAt=updated;PrepLocal.toast('批注暂未保存：'+error.message);return false;}
        draft.base=JSON.stringify(draft.edit);draft.dirty=false;saved.signature=viewerSignature(record,saved.file,saved.page);paintViewer();
        dispatchEvent(new CustomEvent('workspace-change',{detail:{collection:findRecordWithCol(record.id).col,id:record.id}}));
        if(notify)PrepLocal.toast(PrepLocal.hasPending()?'批注已保存到本机，等待同步':'批注已保存，原件未更改');
        PrepLocal.flush().then(()=>{if(viewing===saved&&!draft.dirty)updateEditControls();}).catch(()=>{if(viewing===saved)el('viewerEditStatus').textContent='已保存到本机，等待同步';});return true;
    }
    function setViewerTool(tool){
        if(!viewing?.image)return;finishText();viewing.tool=tool;viewing.original=false;
        if(!['view','text'].includes(tool))selectText(viewing,null);
        for(const button of el('viewerEditingTools').querySelectorAll('[data-viewer-tool]'))button.setAttribute('aria-pressed',String(button.dataset.viewerTool===tool));
        const surface=el('viewerBody').querySelector('canvas');if(surface){surface.style.touchAction='none';surface.style.cursor=tool==='text'?'text':tool==='view'?'grab':'crosshair';}paintViewer();
    }
    function updateEditControls(){
        if(!viewing)return;const draft=viewing.draft,ready=!!viewing.image;
        el('viewerSave').disabled=!ready||!draft?.dirty;el('viewerUndo').disabled=!draft?.undo.length;el('viewerRedo').disabled=!draft?.redo.length;
        el('viewerApplyCrop').disabled=!viewing.crop||viewing.crop[2]<.01||viewing.crop[3]<.01;el('viewerEditStatus').textContent=draft?.dirty?'未保存':PrepLocal.hasPending()?'已保存到本机，等待同步':'已保存';
        for(const button of el('viewerEditingTools').querySelectorAll('[data-viewer-tool]')){button.disabled=!ready;button.setAttribute('aria-pressed',String(button.dataset.viewerTool===(viewing.tool||'view')));}
    }
    function viewerText(event,surface,point,original){
        const saved=viewing,draft=saved.draft,existing=original||textAt(draft.edit,point,saved.image),origin=existing?.points[0]||point;
        composeText(surface,event,existing,text=>{if(viewing!==saved)return;viewerCheckpoint();const stroke={...(existing||{}),tool:'text',text,color:existing?.color||el('viewerInkColor').value,width:.003,fontSize:existing?.fontSize||Number(el('viewerFontSize').value)/1000,points:[origin]};if(existing)draft.edit.strokes[draft.edit.strokes.indexOf(existing)]=stroke;else(draft.edit.strokes||=[]).push(stroke);selectText(saved,stroke);viewerChanged();});
    }
    function mediaText(event,point,original){
        const saved=active,existing=original||textAt(saved.edit,point,saved.image);
        composeText(canvas(),event,existing,text=>{if(active!==saved)return;checkpoint();const stroke={...(existing||{}),tool:'text',text,color:existing?.color||el('mediaColor').value,width:.003,fontSize:existing?.fontSize||Number(el('mediaFontSize').value)/1000,points:existing?.points||[point]};if(existing)saved.edit.strokes[saved.edit.strokes.indexOf(existing)]=stroke;else saved.edit.strokes.push(stroke);selectText(saved,stroke);mark();});
    }
    function bindViewerCanvas(surface){
        surface.tabIndex=0;
        surface.style.touchAction='none';surface.style.cursor=viewing.tool==='text'?'text':viewing.tool==='view'?'grab':'crosshair';
        surface.onpointerdown=event=>{
            if(!viewing?.image||event.button!==0||el('mediaDialog').open)return;
            event.preventDefault();surface.focus({preventScroll:true});finishText();surface.setPointerCapture(event.pointerId);const edit={...viewerEdit(),rotation:((viewerEdit().rotation||0)+viewing.rotation)%360},point=pagePoint(event,surface,viewing.image,edit);
            const text=['view','text'].includes(viewing.tool)&&!viewing.original&&textAt(viewerEdit(),point,viewing.image);
            selectText(viewing,text||null);
            if(text){viewerPointer={text,start:point,origin:[...text.points[0]],box:AnnotationSelection.bounds(text,viewing.image),client:[event.clientX,event.clientY],moved:false};paintViewer();return;}
            if(viewing.tool==='view'){viewerPointer={pan:[event.clientX,event.clientY,el('viewerBody').scrollLeft,el('viewerBody').scrollTop]};paintViewer();return;}
            if(viewing.tool==='text'){viewerText(event,surface,point);return;}
            if(viewing.tool==='crop'){viewing.crop=null;viewerPointer={crop:point};return;}
            viewerCheckpoint();const stroke={tool:viewing.tool,color:el('viewerInkColor').value,width:Number(el('viewerInkWidth').value)/1000,points:[point]};(viewing.draft.edit.strokes||=[]).push(stroke);viewing.draft.dirty=true;viewerPointer={stroke};paintViewer(true);
        };
        surface.onpointermove=event=>{
            if(!viewerPointer||!viewing?.image)return;
            if(viewerPointer.pan){const [x,y,left,top]=viewerPointer.pan;el('viewerBody').scrollLeft=left+x-event.clientX;el('viewerBody').scrollTop=top+y-event.clientY;return;}
            event.preventDefault();const edit={...viewerEdit(),rotation:((viewerEdit().rotation||0)+viewing.rotation)%360},point=pagePoint(event,surface,viewing.image,edit);
            if(viewerPointer.text){
                if(!viewerPointer.moved){if(Math.hypot(event.clientX-viewerPointer.client[0],event.clientY-viewerPointer.client[1])<3)return;viewerCheckpoint();viewerPointer.moved=true;}
                viewerPointer.text.points[0]=AnnotationSelection.moved(viewerPointer.origin,viewerPointer.start,point,viewerPointer.box,edit.crop);viewing.draft.dirty=true;
            }else if(viewerPointer.crop){const [x,y]=viewerPointer.crop;viewing.crop=[Math.min(x,point[0]),Math.min(y,point[1]),Math.abs(x-point[0]),Math.abs(y-point[1])];}
            else if(viewerPointer.stroke.tool==='circle')viewerPointer.stroke.points[1]=point;
            else if(viewerPointer.stroke.points.length<10000)viewerPointer.stroke.points.push(point);
            if(!viewerDrawFrame){const saved=viewing;viewerDrawFrame=requestAnimationFrame(()=>{viewerDrawFrame=0;if(viewing===saved)paintViewer(true);});}
        };
        surface.onpointerup=surface.onpointercancel=()=>{viewerPointer=null;cancelAnimationFrame(viewerDrawFrame);viewerDrawFrame=0;if(viewing?.image)paintViewer();};
        surface.ondblclick=event=>{if(['view','text'].includes(viewing?.tool)){const edit={...viewerEdit(),rotation:((viewerEdit().rotation||0)+viewing.rotation)%360},point=pagePoint(event,surface,viewing.image,edit);if(textAt(viewerEdit(),point,viewing.image))viewerText(event,surface,point);}};
    }
    async function viewerOCR(){
        const saved=viewing;if(!saved)return;el('viewerOCR').disabled=true;
        try{const result=await PrepLocal.jsonRequest('/api/assistant/attachment?'+new URLSearchParams({id:saved.file.id,page:saved.page}),undefined,{long:true});if(viewing!==saved)return;el('viewerOCRText').value=result.text||'';el('viewerOCRPanel').hidden=false;}
        catch(error){PrepLocal.toast(error.message);}finally{el('viewerOCR').disabled=false;}
    }
    function viewerSignature(record,file,page){return MediaPreview.version(file)+'|'+JSON.stringify([record.fileEdits?.[file.id+':'+page]||defaults(),pageMarks(file,page),readingNotes(file)]);}
    function rememberViewer(saved=viewing){
        clearTimeout(viewerPositionTimer);if(!saved?.image)return;
        DeviceStore.put('viewer-position-'+saved.file.id,{sourceVersion:MediaPreview.version(saved.file),page:saved.page,mode:saved.mode,zoom:saved.zoom||1,rotation:saved.rotation}).catch(()=>{});
    }
    function saveViewerPosition(){clearTimeout(viewerPositionTimer);const saved=viewing;viewerPositionTimer=setTimeout(()=>{if(viewing===saved)rememberViewer(saved);},250);}
    function fitViewer(){
        if(!viewing?.image)return;const body=el('viewerBody'),surface=body.querySelector('canvas');if(!surface)return;
        const style=getComputedStyle(body),width=Math.max(80,body.clientWidth-parseFloat(style.paddingLeft)-parseFloat(style.paddingRight)),height=Math.max(80,body.clientHeight-parseFloat(style.paddingTop)-parseFloat(style.paddingBottom));
        const scale=(viewing.mode==='page'?Math.min(width/surface.width,height/surface.height):width/surface.width)*currentViewer.zoom;
        surface.style.width=Math.max(1,Math.round(surface.width*scale))+'px';surface.style.height='auto';surface.style.maxWidth='none';
        surface.parentElement.style.width=surface.style.width;
        renderTextSelection(surface,viewing);
        viewing.zoom=currentViewer.zoom;const percent=String(Math.round(currentViewer.zoom*100)),select=el('viewerScale');select.querySelector('[data-custom-scale]')?.remove();if(![...select.options].some(option=>option.value===percent)){const option=new Option(percent+'%',percent);option.dataset.customScale='true';select.append(option);}select.value=percent;
        for(const mode of ['page','width'])el(mode==='page'?'viewerFitPage':'viewerFitWidth').setAttribute('aria-pressed',String(viewing.mode===mode));
    }
    async function goViewerPage(number){
        if(!viewing?.pages)return;const saved=viewing,page=Math.max(1,Math.min(saved.pages,Math.trunc(Number(number))||saved.page));
        if(page!==saved.page){const ok=await saveViewerDraft();if(viewing!==saved)return;if(ok)renderViewerMedia({page,preservePage:true});else el('viewerPDFPage').value=saved.page;}else el('viewerPDFPage').value=page;
    }
    function fillViewerNotes(record,file,page){
        const host=el('viewerAnnotationList'),marks=pageMarks(file,page).filter(mark=>mark.text),notes=readingNotes(file).filter(note=>!note.sourcePage||note.sourcePage===page),texts=(viewerEdit().strokes||[]).filter(stroke=>stroke.tool==='text');host.replaceChildren();
        for(const stroke of texts){
            const row=document.createElement('div');row.className='viewer-annotation-note';const label=document.createElement('small'),text=document.createElement('div'),actions=document.createElement('div');label.textContent='页面文字';text.textContent=stroke.text;window.AnnotationText?.render(stroke.text,text).catch(error=>text.title=error.message);actions.className='viewer-text-actions';
            for(const [title,icon,action]of [['编辑文字批注','pen-to-square',()=>{const surface=el('viewerBody').querySelector('canvas'),rect=surface.getBoundingClientRect();setViewerTool('text');selectText(viewing,stroke);viewerText({clientX:rect.left+20,clientY:rect.top+20},surface,stroke.points[0],stroke);}],['删除文字批注','trash',()=>{selectText(viewing,stroke);deleteSelectedText(viewing);}]]){const button=document.createElement('button');button.className='btn btn-icon btn-ghost';button.title=title;button.setAttribute('aria-label',title);button.innerHTML=`<i class="fas fa-${icon}"></i>`;button.onclick=action;actions.append(button);}text.onclick=()=>{setViewerTool('view');selectText(viewing,stroke);paintViewer();};row.append(label,text,actions);host.append(row);
        }
        for(const mark of marks){const row=document.createElement('div');row.className='viewer-annotation-note';const label=document.createElement('small');label.textContent=mark.kind==='highlight'?'高亮摘录':mark.kind==='bookmark'?'书签':'批注';const text=document.createElement('p');text.textContent=mark.text;row.append(label,text);host.append(row);}
        for(const note of notes){const row=document.createElement('button');row.className='viewer-annotation-note viewer-linked-note';row.type='button';const title=document.createElement('strong'),text=document.createElement('p');title.textContent=note.title||'阅读笔记';text.textContent=String(note.content||'').slice(0,1000);row.append(title,text);row.onclick=()=>window.Workspace?.openRecord(note.id);host.append(row);}
        if(!host.childNodes.length)host.textContent='本页暂无文字批注';
        el('viewerNotesToggle').setAttribute('aria-label',`批注与阅读笔记 (${marks.length+notes.length+texts.length})`);el('viewerNotesCount').textContent=String(marks.length+notes.length+texts.length);
    }
    function paintViewer(strokeOnly=false){
        if(!viewing?.image)return;const saved=viewing,record=findRecord(saved.id);if(!record)return;
        const current=viewerEdit();hydrateText(current,()=>{if(viewing===saved&&viewerEdit()===current)paintViewer();});
        const edit={...(saved.original?defaults():viewerEdit())};edit.rotation=((edit.rotation||0)+saved.rotation)%360;
        const marks=saved.original?[]:pageMarks(saved.file,saved.page),body=el('viewerBody');let surface=body.querySelector('canvas');if(!surface){const wrap=document.createElement('div');wrap.className='viewer-canvas-wrap';surface=document.createElement('canvas');surface.className='viewer-media viewer-page-canvas';wrap.append(surface);body.replaceChildren(wrap);bindViewerCanvas(surface);}
        surface.setAttribute('aria-label',saved.file.name+' · 第 '+saved.page+' 页');surface.dataset.page=saved.page;MediaPreview.paint(surface,saved.image,edit,marks);
        if(saved.crop&&!saved.original){const [x,y,w,h]=saved.crop,[cx,cy,cw,ch]=edit.crop||[0,0,1,1],ctx=surface.getContext('2d');ctx.save();ctx.translate(surface.width/2,surface.height/2);ctx.rotate(edit.rotation*Math.PI/180);ctx.translate(-cw*saved.image.width/2-cx*saved.image.width,-ch*saved.image.height/2-cy*saved.image.height);ctx.strokeStyle='#15946c';ctx.lineWidth=3;ctx.setLineDash([12,8]);ctx.strokeRect(x*saved.image.width,y*saved.image.height,w*saved.image.width,h*saved.image.height);ctx.restore();}
        renderTextSelection(surface,saved);updateEditControls();if(strokeOnly)return;
        saved.signature=viewerSignature(record,saved.file,saved.page);fillViewerNotes(record,saved.file,saved.page);updateViewerControls();fitViewer();
        el('viewerPageStatus').textContent=saved.original?'原件对比':saved.draft.dirty?'批注草稿':record.fileEdits?.[saved.file.id+':'+saved.page]||marks.length?'已显示全部批注':'原件视图';
        el('viewerOriginalToggle').setAttribute('aria-pressed',String(saved.original));saveViewerPosition();
    }
    function schedulePrefetch(saved){
        clearTimeout(prefetchTimer);saved.prefetch?.abort();
        if(!pdf(saved.file)||saved.page>=saved.pages||navigator.connection?.saveData||/^(slow-)?2g$/.test(navigator.connection?.effectiveType||''))return;
        prefetchTimer=setTimeout(()=>{if(viewing!==saved||document.hidden)return;saved.prefetch=new AbortController();MediaPreview.read(saved.file,saved.page+1,{signal:saved.prefetch.signal}).catch(()=>{});},250);
    }
    async function downloadPreview(fullPDF=false){
        const saved=viewing;if(!saved?.image||(fullPDF&&exportingPDF))return;if(fullPDF){exportingPDF=true;el('viewerDownloadAll').disabled=true;PrepLocal.toast('正在生成完整批注 PDF…');}
        try{
            if(fullPDF){if(!await saveViewerDraft())return;await prepareRecordText(findRecord(saved.id),saved.file);await PrepLocal.flush();if(PrepLocal.hasPending())throw new Error('完整 PDF 导出需要连接电脑完成同步，当前页图片可直接下载');const result=await response('/api/media/export?'+new URLSearchParams({file:saved.file.id,record:saved.id,format:'pdf'}));downloadBlob(saved.file.name.replace(/\.pdf$/i,'')+'-全部批注.pdf',await result.blob());}
            else{finishText();await prepareText(saved.draft.edit);if(viewing!==saved)return;paintViewer();const surface=el('viewerBody').querySelector('canvas'),name=saved.file.name.replace(/\.[^.]+$/,'')+`-第${saved.page}页.png`;surface?.toBlob(blob=>{if(blob)downloadBlob(name,blob);},'image/png');}
        }catch(error){PrepLocal.toast(error.message);}finally{if(fullPDF){exportingPDF=false;el('viewerDownloadAll').disabled=false;}}
    }
    function viewerControls(){
        if(el('viewerDocumentControls'))return;
        const controls=document.createElement('div');controls.id='viewerDocumentControls';controls.className='viewer-document-controls';controls.hidden=true;
        controls.innerHTML='<div id="viewerPDFNavigation"><button class="btn btn-icon btn-ghost" id="viewerPreviousPDF" title="上一页" aria-label="PDF 上一页"><i class="fas fa-chevron-left"></i></button><input id="viewerPDFPage" type="number" min="1" value="1" aria-label="查看 PDF 页码" /><span id="viewerPDFCount"></span><button class="btn btn-icon btn-ghost" id="viewerNextPDF" title="下一页" aria-label="PDF 下一页"><i class="fas fa-chevron-right"></i></button></div><span id="viewerPageStatus" role="status"></span><button class="btn btn-icon btn-ghost" id="viewerReadOriginal" title="原文阅读与文字选择" aria-label="原文阅读与文字选择"><i class="fas fa-book-open"></i></button>';
        el('viewerStage').after(controls);
        const tools=document.createElement('div');tools.id='viewerReadingTools';tools.className='viewer-reading-tools';tools.innerHTML='<select id="viewerAttachment" aria-label="切换组内附件"></select><div role="group" aria-label="页面适配"><button id="viewerFitPage" class="btn btn-icon btn-ghost" title="整页适配" aria-label="整页适配" aria-pressed="true"><i class="fas fa-expand"></i></button><button id="viewerFitWidth" class="btn btn-icon btn-ghost" title="适应宽度" aria-label="适应宽度" aria-pressed="false"><i class="fas fa-arrows-left-right"></i></button></div><select id="viewerScale" aria-label="阅读缩放比例">'+[50,75,100,125,150,200,300,500].map(value=>`<option value="${value}">${value}%</option>`).join('')+'</select><button id="viewerRotateView" class="btn btn-icon btn-ghost" title="旋转视图" aria-label="旋转视图"><i class="fas fa-rotate-right"></i></button><button id="viewerOriginalToggle" class="btn btn-icon btn-ghost" title="对比原件" aria-label="对比原件" aria-pressed="false"><i class="fas fa-eye"></i></button><button id="viewerNotesToggle" class="btn btn-icon btn-ghost" title="批注与阅读笔记" aria-label="批注与阅读笔记" aria-expanded="false"><i class="fas fa-comment-dots"></i><small id="viewerNotesCount">0</small></button>';
        for(const [id,label,icon,full]of [['viewerDownloadPage','下载当前页图片','image',false],['viewerDownloadAll','下载完整批注 PDF','file-arrow-down',true]]){const item=document.createElement('button');item.id=id;item.type='button';item.className='btn btn-icon btn-ghost';item.title=label;item.setAttribute('aria-label',label);item.innerHTML=`<i class="fas fa-${icon}"></i>`;item.onclick=()=>downloadPreview(full);tools.append(item);}
        el('viewerStage').before(tools);
        const editing=document.createElement('div');editing.id='viewerEditingTools';editing.className='viewer-editing-tools';editing.hidden=true;
        const toolButton=(id,label,icon,attrs='')=>`<button type="button" id="${id}" class="btn btn-icon btn-ghost" title="${label}" aria-label="${label.split(' (')[0]}" ${attrs}><i class="fas fa-${icon}"></i></button>`;
        editing.innerHTML='<div role="group" aria-label="页面批注工具">'+[['view','移动页面 (V)','hand'],['pen','画笔 (P / Ctrl+Alt+A)','pen'],['highlight','高亮 (H / Ctrl+Shift+H)','highlighter'],['circle','圈画 (O)','circle'],['text','文字批注 (T)','font'],['crop','裁剪 (C)','crop-simple']].map(([tool,label,icon])=>toolButton('viewerTool-'+tool,label,icon,`data-viewer-tool="${tool}" aria-pressed="${tool==='view'}"`)).join('')+'</div><input id="viewerInkColor" type="color" value="#df5262" title="批注颜色" aria-label="批注颜色" /><input id="viewerInkWidth" type="range" min="1" max="20" value="4" title="笔画粗细" aria-label="笔画粗细" /><select id="viewerFontSize" title="文字字号" aria-label="文字字号">'+[12,16,20,24,32,40,48,64].map(n=>`<option value="${n}" ${n===24?'selected':''}>${n}</option>`).join('')+'</select><div role="group" aria-label="批注操作">'+[['viewerApplyCrop','应用裁剪','check'],['viewerRotatePage','旋转并保存页面方向','rotate-right'],['viewerUndo','撤销 (Ctrl+Z)','arrow-rotate-left'],['viewerRedo','重做 (Ctrl+Y / Ctrl+Shift+Z)','arrow-rotate-right'],['viewerResetPage','清除本页编辑','eraser'],['viewerOCR','识别文字','i-cursor'],['viewerSave','保存批注 (Ctrl+S)','floppy-disk']].map(([id,label,icon])=>toolButton(id,label,icon)).join('')+'</div><span id="viewerEditStatus" role="status"></span>';
        el('viewerStage').before(editing);editing.querySelectorAll('[data-viewer-tool]').forEach(button=>button.onclick=()=>setViewerTool(button.dataset.viewerTool));
        const deleteText=document.createElement('button');deleteText.id='viewerDeleteText';deleteText.type='button';deleteText.className='btn btn-icon btn-ghost';deleteText.title='删除选中批注 (Delete)';deleteText.setAttribute('aria-label','删除选中批注');deleteText.innerHTML='<i class="fas fa-trash"></i>';deleteText.disabled=true;deleteText.onclick=()=>deleteSelectedText(viewing);el('viewerUndo').before(deleteText);bindTextStyle('viewer',()=>viewing);
        el('viewerSave').onclick=()=>saveViewerDraft(true);
        el('viewerApplyCrop').onclick=()=>{const crop=viewing?.crop;if(!crop||crop[2]<.01||crop[3]<.01)return;finishText();viewerCheckpoint();viewing.draft.edit.crop=crop;viewing.crop=null;viewerChanged();};
        el('viewerRotatePage').onclick=()=>{if(!viewing?.image)return;finishText();viewerCheckpoint();viewing.draft.edit.rotation=((viewing.draft.edit.rotation||0)+90)%360;viewing.crop=null;viewerChanged();};
        el('viewerUndo').onclick=()=>{if(!viewing?.draft.undo.length)return;finishText();const draft=viewing.draft;draft.redo.push(copy(draft.edit));draft.edit=draft.undo.pop();viewing.crop=null;viewerChanged();};
        el('viewerRedo').onclick=()=>{if(!viewing?.draft.redo.length)return;finishText();const draft=viewing.draft;draft.undo.push(copy(draft.edit));draft.edit=draft.redo.pop();viewing.crop=null;viewerChanged();};
        el('viewerResetPage').onclick=()=>{if(viewing?.image&&confirm('清除本页裁剪、旋转和页面批注？阅读笔记保留，原件不会改变。')){finishText(false);viewerCheckpoint();viewing.draft.edit=defaults();viewing.crop=null;viewerChanged();}};
        el('viewerOCR').onclick=viewerOCR;
        const main=document.createElement('div');main.className='viewer-document-main';el('viewerStage').before(main);const panel=document.createElement('aside');panel.id='viewerAnnotationPanel';panel.hidden=true;panel.innerHTML='<header><strong>本页批注与笔记</strong><button id="viewerNotesClose" class="btn btn-icon btn-ghost" title="收起批注" aria-label="收起批注"><i class="fas fa-xmark"></i></button></header><div id="viewerAnnotationList"></div>';main.append(el('viewerStage'),panel);
        const ocrPanel=document.createElement('section');ocrPanel.id='viewerOCRPanel';ocrPanel.hidden=true;ocrPanel.innerHTML='<textarea id="viewerOCRText" rows="3" aria-label="识别文字（待校对）"></textarea><div>'+toolButton('viewerOCRCancel','关闭识别结果','xmark')+toolButton('viewerOCRInsert','插入为本页文字批注','font')+'</div>';main.after(ocrPanel);
        el('viewerOCRCancel').onclick=()=>ocrPanel.hidden=true;el('viewerOCRInsert').onclick=()=>{const text=el('viewerOCRText').value.trim().slice(0,2000);if(!text||!viewing?.image)return;viewerCheckpoint();(viewing.draft.edit.strokes||=[]).push({tool:'text',text,points:[[.08,.08]],color:el('viewerInkColor').value,width:.003,fontSize:Number(el('viewerFontSize').value)/1000});ocrPanel.hidden=true;viewing.original=false;viewerChanged();};
        el('viewerPDFPage').onchange=event=>goViewerPage(event.target.value);el('viewerPDFPage').onkeydown=event=>{if(event.key==='Enter')goViewerPage(event.target.value);};
        el('viewerPreviousPDF').onclick=()=>goViewerPage(viewing.page-1);el('viewerNextPDF').onclick=()=>goViewerPage(viewing.page+1);
        el('viewerReadOriginal').onclick=async()=>{if(viewing&&window.ReaderWorkspace&&await saveViewerDraft())ReaderWorkspace.open(viewing.id,currentViewer.idx,viewing.page);};
        el('viewerAttachment').onchange=async event=>{if(!await saveViewerDraft()){event.target.value=String(currentViewer.idx);return;}currentViewer.idx=Number(event.target.value);renderViewerMedia();};
        for(const mode of ['page','width'])el(mode==='page'?'viewerFitPage':'viewerFitWidth').onclick=()=>{if(!viewing)return;viewing.mode=mode;currentViewer.zoom=1;fitViewer();saveViewerPosition();};
        el('viewerScale').onchange=event=>{currentViewer.zoom=Number(event.target.value)/100;fitViewer();saveViewerPosition();};
        el('viewerRotateView').onclick=()=>{if(viewing){viewing.rotation=(viewing.rotation+90)%360;paintViewer();}};
        el('viewerOriginalToggle').onclick=()=>{if(viewing){viewing.original=!viewing.original;paintViewer();}};
        el('viewerNotesClose').onclick=()=>{panel.hidden=true;el('viewerNotesToggle').setAttribute('aria-expanded','false');fitViewer();};
        el('viewerNotesToggle').onclick=()=>{panel.hidden=!panel.hidden;el('viewerNotesToggle').setAttribute('aria-expanded',String(!panel.hidden));fitViewer();};
    }
    function updateViewerControls(){
        if(!viewing)return;const pages=viewing.pages||1;
        el('viewerPDFNavigation').hidden=!pdf(viewing.file);el('viewerPDFPage').value=viewing.page;el('viewerPDFPage').max=pages;el('viewerPDFPage').disabled=!viewing.pages;
        el('viewerDownloadAll').hidden=!pdf(viewing.file);
        el('viewerPDFCount').textContent='/ '+(viewing.pages||'…');el('viewerPreviousPDF').disabled=!viewing.pages||viewing.page<=1;el('viewerNextPDF').disabled=!viewing.pages||viewing.page>=pages;
        const record=findRecord(viewing.id),files=StudyLibrary.files(record||{}),select=el('viewerAttachment'),signature=JSON.stringify(files.map(file=>[file.id,file.name]));if(select.dataset.files!==signature){select.replaceChildren(...files.map((file,index)=>new Option(`${index+1}. ${file.name}`,String(index))));select.dataset.files=signature;}select.value=String(currentViewer.idx);select.hidden=files.length<2;
        const left=el('viewerStage').querySelector('.viewer-pager-left'),right=el('viewerStage').querySelector('.viewer-pager-right');left.disabled=pdf(viewing.file)?!viewing.pages||viewing.page<=1:files.length<2;right.disabled=pdf(viewing.file)?!viewing.pages||viewing.page>=pages:files.length<2;
    }
    async function previewViewer(record,file,options,request){
        const previous=viewing,sameFile=previous?.id===record.id&&previous.file.id===file.id;
        if(previous&&!sameFile)rememberViewer(previous);
        let preference;if(!options.page&&!(options.preservePage&&sameFile))preference=await DeviceStore.get('viewer-position-'+file.id).catch(()=>null);
        if(request!==viewerGeneration||!el('viewerDialog').open)return;if(preference?.sourceVersion!==MediaPreview.version(file))preference=null;
        const position=(state.readingPositions||[]).filter(value=>value.fileId===file.id).sort((a,b)=>String(b.updatedAt).localeCompare(String(a.updatedAt)))[0];
        const page=pdf(file)?Math.max(1,Math.min(file.pageCount||10000,Math.trunc(options.page||(options.preservePage&&sameFile?previous.page:preference?.page||position?.page||1)))):1;
        const sameLocation=sameFile&&previous.page===page&&MediaPreview.version(previous.file)===MediaPreview.version(file),samePage=sameLocation&&!options.retry;
        clearTimeout(prefetchTimer);clearTimeout(viewerPositionTimer);
        const edit=copy(record.fileEdits?.[file.id+':'+page]||defaults());edit.strokes||=[];
        viewing={id:record.id,file,page,pages:sameFile?previous.pages:file.pageCount||null,image:samePage?previous.image:null,signature:viewerSignature(record,file,page),controller:samePage?previous.controller:new AbortController(),mode:sameFile?previous.mode:preference?.mode||'page',rotation:sameFile?previous.rotation:preference?.rotation||0,original:sameFile?previous.original:false,tool:sameFile?previous.tool||'view':'view',draft:sameLocation&&previous.draft?.dirty?previous.draft:{edit,base:JSON.stringify(record.fileEdits?.[file.id+':'+page]||defaults()),undo:[],redo:[],dirty:false}};
        if(!sameFile)currentViewer.zoom=Math.max(.25,Math.min(5,Number(preference?.zoom)||1));
        if(!samePage){previous?.controller?.abort();previous?.image?.close?.();}
        const saved=viewing,body=el('viewerBody');
        el('viewerTitle').textContent=record.title||'资料查看';el('viewerMeta').textContent=`${currentViewer.idx+1} / ${StudyLibrary.files(record).length} · ${file.name}`;
        el('viewerDocumentControls').hidden=false;el('viewerPageStatus').textContent='正在读取…';updateViewerControls();
        el('viewerReadingTools').hidden=false;el('viewerEditingTools').hidden=false;el('viewerOCRPanel').hidden=true;updateEditControls();
        if(!samePage)body.innerHTML='<div class="viewer-message">正在读取页面…</div>';
        try{
            const loading=!saved.image?MediaPreview.image(file,page,{signal:saved.controller.signal}):null;previous?.prefetch?.abort();
            if(loading){const result=await loading;if(request!==viewerGeneration||!el('viewerDialog').open){result.image.close();return;}saved.image=result.image;saved.pages=result.pages;}
            if(request!==viewerGeneration||!el('viewerDialog').open)return;
            const latest=findRecord(record.id);if(!latest||!StudyLibrary.files(latest).some(item=>item.id===file.id))throw new Error('附件已移除');
            await prepareText(saved.draft.edit).catch(error=>PrepLocal.toast(error.message));if(request!==viewerGeneration||viewing!==saved)return;
            paintViewer();if(!samePage)body.scrollTop=0;schedulePrefetch(saved);
        }catch(error){
            if(request!==viewerGeneration||error.name==='AbortError')return;el('viewerPageStatus').textContent='页面读取失败';
            const message=document.createElement('div');message.className='viewer-message';message.setAttribute('role','alert');const text=document.createElement('p');
            text.textContent=error.name==='TypeError'?'电脑连接暂时中断，PDF 原件与批注仍保留':error.message||'PDF 暂时无法读取';message.append(text);
            for(const [title,icon,action]of [['重试读取','rotate-right',async()=>{await MediaPreview.invalidate(file,page);if(viewing===saved)renderViewerMedia({page,preservePage:true,retry:true});}],['原文阅读','book-open',async()=>{if(await saveViewerDraft())window.ReaderWorkspace?.open(saved.id,currentViewer.idx,page);}]]){const button=document.createElement('button');button.className='btn btn-ghost';button.innerHTML=`<i class="fas fa-${icon}"></i> ${title}`;button.onclick=async()=>{button.disabled=true;try{await action();}finally{button.disabled=false;}};message.append(button);}body.replaceChildren(message);
        }
    }
    function detailSource(record){
        let owner=record,files=StudyLibrary.files(record),index=files.findIndex(file=>pdf(file));
        if(record.sourceFileId){owner=findRecord(record.sourceId);files=owner?StudyLibrary.files(owner):[];index=files.findIndex(file=>file.id===record.sourceFileId);}
        if(index<0&&record.anchorIds?.length){const anchor=(state.sourceAnchors||[]).find(item=>record.anchorIds.includes(item.id)),source=anchor&&findRecord(anchor.sourceId);if(source){owner=source;files=StudyLibrary.files(source);index=files.findIndex(file=>file.id===anchor.fileId);if(index>=0)return {owner,file:files[index],index,page:anchor.page};}}
        if(index<0||!owner||!supported(files[index]))return;const file=files[index];
        const marked=[...Object.keys(owner.fileEdits||{}).filter(key=>key.startsWith(file.id+':')).map(key=>Number(key.slice(file.id.length+1))),...(state.readingMarks||[]).filter(mark=>mark.fileId===file.id).map(mark=>mark.page)].filter(page=>Number.isInteger(page)&&page>0).sort((a,b)=>a-b);
        return {owner,file,index,page:record.sourcePage||marked[0]||1};
    }
    async function renderInline(record,force=false){
        const body=el('studyDetailBody');if(!body||(!force&&!el('studyDetail').open))return;const source=detailSource(record);let section=el('studyPDFPreview');
        if(!source){section?.remove();return;}
        const {owner,file,index,page}=source,signature=viewerSignature(owner,file,page);
        if(section?.dataset.signature===signature)return;
        inlineController?.abort();inlineController=new AbortController();const controller=inlineController,request=++inlineGeneration;
        if(!section){section=document.createElement('section');section.id='studyPDFPreview';section.className='detail-pdf-preview';(body.querySelector('.detail-attachments')||body.querySelector('.detail-text'))?.after(section);if(!section.isConnected)body.append(section);}
        section.dataset.record=record.id;section.dataset.signature=signature;section.replaceChildren();
        const header=document.createElement('header'),heading=document.createElement('h4'),expand=document.createElement('button'),surface=document.createElement('canvas'),status=document.createElement('p');heading.textContent=`${record.sourceFileId?'笔记原页':'附件预览'} · 第 ${page} 页`;expand.className='btn btn-icon btn-ghost';expand.title='打开原页与批注';expand.setAttribute('aria-label',expand.title);expand.innerHTML='<i class="fas fa-expand"></i>';expand.onclick=()=>openViewer(owner.id,index,{page});header.append(heading,expand);status.className='detail-preview-status';status.textContent='正在读取原页…';surface.hidden=true;surface.setAttribute('aria-label',file.name+' · 批注原页');section.append(header,surface,status);
        const texts=pageMarks(file,page).filter(mark=>mark.text);if(texts.length){const list=document.createElement('div');list.className='detail-page-annotations';for(const mark of texts){const row=document.createElement('p');row.textContent=mark.text;list.append(row);}section.append(list);}
        try{const result=await MediaPreview.image(file,page,{signal:controller.signal});try{if(request!==inlineGeneration||!section.isConnected)return;const latest=findRecord(owner.id);if(!latest||!StudyLibrary.files(latest).some(item=>item.id===file.id))return;const edit=copy(latest.fileEdits?.[file.id+':'+page]||defaults());await prepareText(edit);if(request!==inlineGeneration||!section.isConnected)return;MediaPreview.paint(surface,result.image,edit,pageMarks(file,page));surface.hidden=false;status.textContent='';}finally{result.image.close();}}
        catch(error){if(error.name!=='AbortError'&&section.isConnected){delete section.dataset.signature;status.textContent=error.name==='TypeError'?'电脑连接暂时中断':error.message;const retry=document.createElement('button');retry.className='btn btn-ghost btn-sm';retry.innerHTML='<i class="fas fa-rotate-right"></i> 重试读取';retry.onclick=async()=>{retry.disabled=true;await MediaPreview.invalidate(file,page);if(section.isConnected)renderInline(findRecord(record.id)||record,true);};section.append(retry);}}
    }
    function setup(){
        const dialog=document.createElement('dialog');dialog.id='mediaDialog';dialog.className='media-workspace';
        dialog.innerHTML='<header class="media-header"><button class="btn btn-icon btn-ghost" id="mediaClose" title="返回" aria-label="返回"><i class="fas fa-arrow-left"></i></button><div><strong id="mediaFilename"></strong><span id="mediaGroup"></span></div><button class="btn btn-icon btn-ghost" id="mediaFullscreen" title="全屏" aria-label="全屏"><i class="fas fa-expand"></i></button></header><div class="media-toolbar"><div class="media-tool-modes" role="group" aria-label="批注工具"><button data-tool="pen" class="btn btn-icon" title="画笔" aria-label="画笔" aria-pressed="true"><i class="fas fa-pen"></i></button><button data-tool="highlight" class="btn btn-icon" title="高亮" aria-label="高亮"><i class="fas fa-highlighter"></i></button><button data-tool="circle" class="btn btn-icon" title="圈画" aria-label="圈画"><i class="far fa-circle"></i></button><button data-tool="crop" class="btn btn-icon" title="裁剪" aria-label="裁剪"><i class="fas fa-crop-simple"></i></button></div><input id="mediaColor" type="color" value="#df5262" title="笔画颜色" aria-label="笔画颜色" /><input id="mediaWidth" type="range" min="1" max="20" value="4" title="笔画粗细" aria-label="笔画粗细" /><button id="mediaApplyCrop" class="btn btn-icon btn-ghost" title="应用裁剪" aria-label="应用裁剪" disabled><i class="fas fa-check"></i></button><button id="mediaRotate" class="btn btn-icon btn-ghost" title="顺时针旋转" aria-label="顺时针旋转"><i class="fas fa-rotate-right"></i></button><button id="mediaUndo" class="btn btn-icon btn-ghost" title="撤销" aria-label="撤销"><i class="fas fa-arrow-rotate-left"></i></button><button id="mediaRedo" class="btn btn-icon btn-ghost" title="重做" aria-label="重做"><i class="fas fa-arrow-rotate-right"></i></button><button id="mediaReset" class="btn btn-icon btn-ghost" title="恢复原件视图" aria-label="恢复原件视图"><i class="fas fa-eraser"></i></button><button id="mediaOCR" class="btn btn-icon btn-ghost" title="识别文字" aria-label="识别文字"><i class="fas fa-font"></i></button></div><div class="media-stage" id="mediaStage"><button id="mediaPreviousFile" class="btn media-file-prev" title="上一附件" aria-label="上一附件">&lt;</button><div id="mediaCanvasWrap"><canvas id="mediaCanvas" aria-label="附件批注画布"></canvas></div><p id="mediaStatus" role="status"></p><button id="mediaNextFile" class="btn media-file-next" title="下一附件" aria-label="下一附件">&gt;</button></div><footer class="media-footer"><div id="mediaPDFPages"><button class="btn btn-icon btn-ghost" id="mediaPreviousPage" title="上一页" aria-label="上一页"><i class="fas fa-chevron-left"></i></button><input type="number" id="mediaPage" min="1" value="1" aria-label="PDF 页码" /><span id="mediaPageCount"></span><button class="btn btn-icon btn-ghost" id="mediaNextPage" title="下一页" aria-label="下一页"><i class="fas fa-chevron-right"></i></button></div><label class="media-zoom"><i class="fas fa-magnifying-glass"></i><input type="range" id="mediaZoom" min="50" max="250" step="25" value="100" title="缩放" aria-label="缩放" /></label><select id="mediaExportFormat" aria-label="导出格式"><option value="png">当前页图片</option><option value="pdf">完整 PDF</option></select><button class="btn btn-icon btn-ghost" id="mediaDownload" title="下载批注副本" aria-label="下载批注副本"><i class="fas fa-download"></i></button><button class="btn btn-primary" id="mediaSave"><i class="fas fa-check"></i> 保存</button></footer><section id="mediaOCRResult" class="media-ocr-result" hidden><label>识别文字（待校对）<textarea id="mediaOCRText" rows="6"></textarea></label><div><button class="btn btn-ghost" id="mediaOCRCancel">退回</button><button class="btn btn-primary" id="mediaOCRConfirm">确认文字并加入记录</button></div></section>';
        document.body.append(dialog);
        const textButton=document.createElement('button');textButton.className='btn btn-icon';textButton.dataset.tool='text';textButton.title='文字批注 (T)';textButton.setAttribute('aria-label','文字批注');textButton.innerHTML='<i class="fas fa-font"></i>';dialog.querySelector('[data-tool=crop]').before(textButton);
        const fontSelect=document.createElement('select');fontSelect.id='mediaFontSize';fontSelect.setAttribute('aria-label','文字字号');fontSelect.title='文字字号';fontSelect.innerHTML=[12,16,20,24,32,40,48,64].map(n=>`<option value="${n}" ${n===24?'selected':''}>${n}</option>`).join('');el('mediaApplyCrop').before(fontSelect);
        const selectTool=document.createElement('button');selectTool.className='btn btn-icon';selectTool.dataset.tool='view';selectTool.title='选择与移动 (V)';selectTool.setAttribute('aria-label','选择与移动');selectTool.innerHTML='<i class="fas fa-arrow-pointer"></i>';dialog.querySelector('[data-tool=pen]').before(selectTool);
        const deleteText=document.createElement('button');deleteText.id='mediaDeleteText';deleteText.className='btn btn-icon btn-ghost';deleteText.title='删除选中批注 (Delete)';deleteText.setAttribute('aria-label','删除选中批注');deleteText.innerHTML='<i class="fas fa-trash"></i>';deleteText.disabled=true;deleteText.onclick=()=>deleteSelectedText(active);el('mediaUndo').before(deleteText);bindTextStyle('media',()=>active);
        el('mediaClose').onclick=close;dialog.oncancel=event=>{event.preventDefault();close();};dialog.onclose=()=>{finishText(false);generation++;cancelAnimationFrame(drawFrame);drawFrame=0;active?.image?.close?.();active=null;};
        el('mediaFullscreen').onclick=()=>dialog.classList.toggle('is-fullscreen');el('mediaSave').onclick=save;el('mediaDownload').onclick=download;el('mediaOCR').onclick=ocr;
        el('mediaPreviousFile').onclick=()=>moveFile(-1);el('mediaNextFile').onclick=()=>moveFile(1);el('mediaPreviousPage').onclick=()=>movePage(-1);el('mediaNextPage').onclick=()=>movePage(1);el('mediaPage').onchange=event=>movePage(Number(event.target.value)-active.page);el('mediaZoom').oninput=fit;
        dialog.querySelectorAll('[data-tool]').forEach(button=>button.onclick=()=>{if(!active)return;finishText();active.tool=button.dataset.tool;if(!['text','view'].includes(active.tool))selectText(active,null);dialog.querySelectorAll('[data-tool]').forEach(item=>item.setAttribute('aria-pressed',String(item===button)));draw();});
        el('mediaRotate').onclick=()=>{if(!active?.image)return;checkpoint();active.edit.rotation=(active.edit.rotation+90)%360;cropSelection=null;mark();};
        el('mediaApplyCrop').onclick=()=>{if(cropSelection){checkpoint();active.edit.crop=cropSelection;cropSelection=null;mark();}};
        el('mediaReset').onclick=()=>{if(active?.image&&confirm('清除本页裁剪、旋转和画板批注？阅读批注保留，原件不会改变。')){checkpoint();active.edit=defaults();cropSelection=null;mark();}};
        el('mediaUndo').onclick=()=>{if(active.undo.length){active.redo.push(copy(active.edit));active.edit=active.undo.pop();cropSelection=null;mark();}};el('mediaRedo').onclick=()=>{if(active.redo.length){active.undo.push(copy(active.edit));active.edit=active.redo.pop();mark();}};
        canvas().tabIndex=0;canvas().onpointerdown=event=>{
            if(!active?.image||event.button!==0)return;event.preventDefault();canvas().focus({preventScroll:true});finishText();const p=point(event);canvas().setPointerCapture(event.pointerId);
            const text=['view','text'].includes(active.tool)&&textAt(active.edit,p,active.image);selectText(active,text||null);
            if(text){mediaPointer={text,start:p,origin:[...text.points[0]],box:AnnotationSelection.bounds(text,active.image),client:[event.clientX,event.clientY],moved:false};draw();return;}
            if(active.tool==='text'){mediaText(event,p);return;}
            if(active.tool==='view'){mediaPointer={pan:[event.clientX,event.clientY,el('mediaStage').scrollLeft,el('mediaStage').scrollTop]};draw();return;}
            down=true;if(active.tool==='crop'){pendingStroke={points:[p,p]};cropSelection=null;}else{checkpoint();pendingStroke={tool:active.tool,color:el('mediaColor').value,width:Number(el('mediaWidth').value)/1000,points:[p]};active.edit.strokes.push(pendingStroke);active.dirty=true;}draw();
        };
        canvas().onpointermove=event=>{
            if(!active?.image)return;
            if(mediaPointer?.pan){const [x,y,left,top]=mediaPointer.pan;el('mediaStage').scrollLeft=left+x-event.clientX;el('mediaStage').scrollTop=top+y-event.clientY;return;}
            const p=point(event);
            if(mediaPointer?.text){if(!mediaPointer.moved){if(Math.hypot(event.clientX-mediaPointer.client[0],event.clientY-mediaPointer.client[1])<3)return;checkpoint();mediaPointer.moved=true;}mediaPointer.text.points[0]=AnnotationSelection.moved(mediaPointer.origin,mediaPointer.start,p,mediaPointer.box,active.edit.crop);active.dirty=true;queueDraw();return;}
            if(!down)return;event.preventDefault();if(active.tool==='crop'){const [x,y]=pendingStroke.points[0],w=Math.abs(x-p[0]),h=Math.abs(y-p[1]);cropSelection=w>=.01&&h>=.01?[Math.min(x,p[0]),Math.min(y,p[1]),w,h]:null;}else if(pendingStroke.tool==='circle')pendingStroke.points[1]=p;else if(pendingStroke.points.length<10000)pendingStroke.points.push(p);queueDraw();
        };
        canvas().onpointerup=canvas().onpointercancel=()=>{down=false;pendingStroke=null;mediaPointer=null;if(active?.image)draw();};
        canvas().ondblclick=event=>{if(active?.image&&['view','text'].includes(active.tool)){const p=point(event),text=textAt(active.edit,p,active.image);if(text)mediaText(event,p,text);}};
        el('mediaOCRCancel').onclick=()=>el('mediaOCRResult').hidden=true;el('mediaOCRConfirm').onclick=()=>{const record=findRecord(el('mediaOCRResult').dataset.record);if(!record)return;const text=el('mediaOCRText').value.trim();if(!text)return;record.content=(record.content?record.content+'\n\n':'')+text;saveState();el('mediaOCRResult').hidden=true;dispatchEvent(new CustomEvent('workspace-change'));PrepLocal.toast('已保存校对后的文字');PrepLocal.flush().catch(error=>PrepLocal.toast(error.message));};
        const order=document.createElement('dialog');order.id='mediaOrderDialog';order.innerHTML='<header class="dialog-header"><h3>附件顺序</h3><button class="btn btn-icon" title="返回" aria-label="返回"><i class="fas fa-xmark"></i></button></header><div class="dialog-body"><div id="mediaOrderList"></div><div class="dialog-actions"><button class="btn btn-ghost" id="mediaOrderCancel">退回</button><button class="btn btn-primary" id="mediaOrderSave">继续并保存</button></div></div>';document.body.append(order);order.querySelector('header button').onclick=el('mediaOrderCancel').onclick=()=>order.close();el('mediaOrderSave').onclick=()=>order._save?.();
        for(const [form,input] of [['recordForm','files'],['paperForm','file']])draftCamera(el(form),el(form).elements[input]);
        addEventListener('study-detail',event=>{const record=findRecord(event.detail.id);if(!record)return;const section=el('studyConnections')||el('studyDetailBody');if(!el('studyMediaActions')){const actions=document.createElement('div');actions.id='studyMediaActions';actions.className='study-media-actions';const sort=document.createElement('button');sort.className='btn btn-ghost btn-sm';sort.innerHTML='<i class="fas fa-sort"></i> 附件排序';sort.onclick=()=>organize(record.id);actions.append(sort);StudyLibrary.files(record).forEach((file,index)=>{if(!supported(file))return;const button=document.createElement('button');button.className='btn btn-ghost btn-sm';button.innerHTML='<i class="fas fa-highlighter"></i> '+esc(file.name);button.onclick=()=>open(record.id,index);actions.append(button);});if(StudyLibrary.files(record).length)section.append(actions);}});
        viewerControls();
        addEventListener('study-detail',event=>{const record=findRecord(event.detail.id);if(record)renderInline(record,true);});
        const refreshInline=()=>{const section=el('studyPDFPreview'),record=section&&findRecord(section.dataset.record);if(record)renderInline(record);};
        addEventListener('workspace-change',refreshInline);addEventListener('yantu-sync',refreshInline);el('studyDetail').addEventListener('close',()=>{inlineGeneration++;inlineController?.abort();});
        const previousMedia=renderViewerMedia;renderViewerMedia=async(options={})=>{
            const record=findRecord(currentViewer.id),file=record&&StudyLibrary.files(record)[currentViewer.idx];
            if(viewing&&(viewing.id!==record?.id||viewing.file.id!==file?.id)&&!await saveViewerDraft()){currentViewer.id=viewing.id;currentViewer.idx=StudyLibrary.files(findRecord(viewing.id)).findIndex(item=>item.id===viewing.file.id);return;}
            const request=++viewerGeneration;PrepLocal.invalidateViewer();let button=el('viewerAnnotate');if(!button){button=document.createElement('button');button.id='viewerAnnotate';button.className='btn btn-icon btn-ghost';button.title='独立裁剪与批注窗口';button.setAttribute('aria-label',button.title);button.innerHTML='<i class="fas fa-highlighter"></i>';el('viewerDialog').querySelector('.viewer-actions').prepend(button);}
            button.hidden=!supported(file);button.onclick=async()=>{if(await saveViewerDraft())open(currentViewer.id,currentViewer.idx,viewing?.page||1);};
            if(supported(file))return previewViewer(record,file,options,request);
            rememberViewer();viewing?.controller?.abort();viewing?.prefetch?.abort();viewing?.image?.close?.();viewing=null;el('viewerDocumentControls').hidden=true;el('viewerReadingTools').hidden=true;el('viewerEditingTools').hidden=true;el('viewerOCRPanel').hidden=true;el('viewerAnnotationPanel').hidden=true;return previousMedia();
        };
        const originalPage=viewerPage;viewerPage=step=>{if(viewing&&pdf(viewing.file))goViewerPage(viewing.page+step);else originalPage(step);};
        const originalZoom=viewerZoom;viewerZoom=step=>{if(!viewing?.image)return originalZoom(step);currentViewer.zoom=Math.max(.25,Math.min(5,currentViewer.zoom+step));fitViewer();saveViewerPosition();};
        const refreshViewer=()=>{if(!el('viewerDialog').open||!viewing)return;updateEditControls();const record=findRecord(viewing.id),file=record&&StudyLibrary.files(record)[currentViewer.idx];if(!file||viewerSignature(record,file,viewing.page)!==viewing.signature){if(viewing.draft?.dirty){fillViewerNotes(record||{},viewing.file,viewing.page);return;}renderViewerMedia({preservePage:true});}};
        addEventListener('workspace-change',refreshViewer);addEventListener('yantu-sync',refreshViewer);
        el('viewerStage').addEventListener('wheel',event=>{if(event.ctrlKey){event.preventDefault();viewerZoom(event.deltaY<0?.1:-.1);}},{passive:false});
        document.addEventListener('keydown',event=>{
            if(event.isComposing)return;const media=el('mediaDialog').open,host=media?el('mediaDialog'):el('viewerDialog');if(!host.open||!host.contains(event.target))return;
            const typing=event.target.isContentEditable||/INPUT|TEXTAREA|SELECT/.test(event.target.tagName),mod=event.ctrlKey||event.metaKey,key=event.key.toLowerCase();let action;
            if(mod&&key==='s')action=media?save:()=>saveViewerDraft(true);
            else if(!typing&&!mod&&['Delete','Backspace'].includes(event.key)&&selectedText(media?active:viewing))action=()=>deleteSelectedText(media?active:viewing);
            else if(!typing&&event.key==='Escape'&&selectedText(media?active:viewing))action=()=>{const owner=media?active:viewing;selectText(owner,null);if(media)draw();else paintViewer();};
            else if(!typing&&mod&&key==='z')action=()=>el(media?(event.shiftKey?'mediaRedo':'mediaUndo'):(event.shiftKey?'viewerRedo':'viewerUndo')).click();
            else if(!typing&&mod&&key==='y')action=()=>el(media?'mediaRedo':'viewerRedo').click();
            else if(!typing&&!media&&((mod&&event.shiftKey&&key==='h')||(mod&&event.altKey&&key==='a')))action=()=>setViewerTool(key==='h'?'highlight':'pen');
            else if(!typing&&!mod&&!event.altKey){
                const tool={v:'view',p:'pen',h:'highlight',o:'circle',t:'text',c:'crop'}[key];
                if(tool)action=media?()=>host.querySelector(`[data-tool="${tool}"]`)?.click():()=>setViewerTool(tool);
                else if(!media)action={PageUp:()=>viewerPage(-1),PageDown:()=>viewerPage(1),Home:()=>goViewerPage(1),End:()=>goViewerPage(viewing?.pages),'+':()=>viewerZoom(.1),'=':()=>viewerZoom(.1),'-':()=>viewerZoom(-.1)}[event.key];
            }
            if(action){event.preventDefault();event.stopImmediatePropagation();action();}
        },true);
        const closeViewer=async()=>{
            if(await saveViewerDraft()){el('viewerDialog').close();return;}
            if(!viewing||!confirm('当前批注未能合并保存。将草稿单独保留在本机并关闭？'))return;
            const saved=viewing;try{await DeviceStore.put('viewer-conflict-'+saved.file.id+':'+saved.page,{edit:copy(saved.draft.edit),base:saved.draft.base,updatedAt:PrepLocal.nowDate().toISOString()});if(viewing===saved){saved.draft.dirty=false;el('viewerDialog').close();}}
            catch(error){PrepLocal.toast('草稿暂未保留，窗口不会关闭：'+error.message);}
        };el('viewerDialog').querySelector('.viewer-actions .btn-primary').onclick=closeViewer;
        el('viewerDialog').addEventListener('cancel',event=>{event.preventDefault();if(textEditor){finishText(false);return;}closeViewer();});
        addEventListener('beforeunload',event=>{if(viewing?.draft?.dirty||textEditor){event.preventDefault();event.returnValue='';}});
        addEventListener('resize',()=>{fit();fitViewer();});el('viewerDialog').addEventListener('close',()=>{finishText(false);viewerGeneration++;rememberViewer();clearTimeout(prefetchTimer);cancelAnimationFrame(viewerDrawFrame);viewerDrawFrame=0;viewerPointer=null;viewing?.controller?.abort();viewing?.prefetch?.abort();viewing?.image?.close?.();viewing=null;el('viewerDocumentControls').hidden=true;el('viewerEditingTools').hidden=true;el('viewerOCRPanel').hidden=true;el('viewerAnnotationPanel').hidden=true;});
    }
    const previousReady=PrepLocal.ready;PrepLocal.ready=()=>{previousReady();setup();};
    window.MediaTools={open,organize,save,download,moveFile,movePage,draw,preview:(id,index,page)=>openViewer(id,index,{page})};
})();
