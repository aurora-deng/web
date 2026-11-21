(() => {
    const W=window.Workspace,{el,esc,clone,button,icon}=W;
    let active,library,rendering=0,opening=0,pointer,selection=[],draftTimer,positionTimer,zoomTimer;
    const draftWrites=new Map(),MAX_PIXELS=4*1024*1024,MAX_SIDE=8192;
    const isPDF=file=>file.type==='application/pdf'||/\.pdf$/i.test(file.name);
    const forward=([x,y],rotation)=>rotation===90?[1-y,x]:rotation===180?[1-x,1-y]:rotation===270?[y,1-x]:[x,y];
    const inverse=(point,rotation)=>forward(point,(360-rotation)%360);
    function rectangle(a,b){return [Math.min(a[0],b[0]),Math.min(a[1],b[1]),Math.abs(a[0]-b[0]),Math.abs(a[1]-b[1])];}
    function mappedRect(region,rotation){const [x,y,w,h]=region;return rectangle(forward([x,y],rotation),forward([x+w,y+h],rotation));}
    function point(event){const bounds=el('readerOverlay').getBoundingClientRect();return inverse([Math.max(0,Math.min(1,(event.clientX-bounds.left)/bounds.width)),Math.max(0,Math.min(1,(event.clientY-bounds.top)/bounds.height))],active.rotation);}
    const matchingMark=(mark,file)=>!file.sha256||!mark.fingerprint||String(file.sha256).toLowerCase()===String(mark.fingerprint).toLowerCase();
    function marks(){return(state.readingMarks||[]).filter(mark=>mark.fileId===active.file.id&&mark.page===active.page&&matchingMark(mark,active.file)).map(mark=>pointer?.preview?.id===mark.id?pointer.preview:mark);}
    function regionBox(target,region,color='#167ca2'){const [x,y,w,h]=mappedRect(region,active.rotation),box=document.createElement('div');box.className='reader-region';Object.assign(box.style,{left:x*100+'%',top:y*100+'%',width:w*100+'%',height:h*100+'%',borderColor:color});target.append(box);}
    function fileEditOverlay(canvas){
        const source=W.record(active.id)?.record||active.source,strokes=source.fileEdits?.[active.file.id+':'+active.page]?.strokes;
        if(!strokes?.length||!window.MediaPreview?.paint||!canvas.width||!canvas.height)return;
        if(window.AnnotationText&&!AnnotationText.ready({strokes})){
            const saved=active;AnnotationText.prepareEdit({strokes}).then(()=>{if(active===saved&&canvas.isConnected)overlay();}).catch(error=>{if(active===saved)el('readerStatus').textContent=error.message;});
        }
        const image=document.createElement('canvas'),sideways=active.rotation%180!==0,ctx=canvas.getContext('2d');
        image.width=sideways?canvas.height:canvas.width;image.height=sideways?canvas.width:canvas.height;
        // Shared paint fills the page white first; omit only that fill on this transparent overlay.
        let background=true;
        const transparent=new Proxy(ctx,{get(target,name){
            if(name==='fillRect')return(...args)=>{if(background){background=false;return;}target.fillRect(...args);};
            const value=Reflect.get(target,name,target);return typeof value==='function'?value.bind(target):value;
        },set:(target,name,value)=>Reflect.set(target,name,value,target)});
        const surface={get width(){return canvas.width;},set width(value){canvas.width=value;},get height(){return canvas.height;},set height(value){canvas.height=value;},getContext:()=>transparent};
        ctx.save();try{window.MediaPreview.paint(surface,image,{crop:[0,0,1,1],rotation:active.rotation,strokes});}finally{ctx.restore();}
    }
    function overlay(){
        if(!active)return;const host=el('readerOverlay');host.replaceChildren();host.classList.toggle('reader-drawing',active.tool!=='select');
        host.style.pointerEvents=active.tool==='select'?'none':'auto';el('readerText').style.pointerEvents=active.tool==='select'?'auto':'none';
        const canvas=document.createElement('canvas');canvas.width=el('readerCanvas').width;canvas.height=el('readerCanvas').height;canvas.style.width='100%';canvas.style.height='100%';host.append(canvas);const ctx=canvas.getContext('2d');
        if(!active.original)fileEditOverlay(canvas);
        if(!active.original)for(const mark of marks()){
            const selected=active.selected===mark.id;ctx.strokeStyle=mark.color||'#d8465b';ctx.fillStyle=mark.color||'#e0bb20';ctx.lineWidth=(mark.width||.003)*Math.min(canvas.width,canvas.height);ctx.lineCap='round';ctx.lineJoin='round';
            if(mark.kind==='ink'){const points=mark.points.map(value=>forward(value,active.rotation));ctx.beginPath();points.forEach(([x,y],i)=>i?ctx.lineTo(x*canvas.width,y*canvas.height):ctx.moveTo(x*canvas.width,y*canvas.height));if(points.length===1){const [x,y]=points[0];ctx.lineTo(x*canvas.width+.2,y*canvas.height+.2);}ctx.stroke();}
            else for(const region of mark.regions||[]){const [x,y,w,h]=mappedRect(region,active.rotation);ctx.save();ctx.globalAlpha=.25;ctx.fillRect(x*canvas.width,y*canvas.height,w*canvas.width,h*canvas.height);ctx.restore();}
            if(selected){const points=mark.kind==='ink'?mark.points:(mark.regions||[]).flatMap(([x,y,w,h])=>[[x,y],[x+w,y+h]]);if(points.length)regionBox(host,rectangle([Math.min(...points.map(p=>p[0])),Math.min(...points.map(p=>p[1]))],[Math.max(...points.map(p=>p[0])),Math.max(...points.map(p=>p[1]))]),'#d8465b');}
        }
        for(const entry of selection.filter(entry=>entry.page===active.page))for(const region of entry.regions)regionBox(host,region);
        if(active.anchor)for(const region of active.anchor.regions||[])regionBox(host,region,'#2d9b62');
        const list=el('readerMarkList');list.replaceChildren();for(const mark of marks()){const row=button(mark.text||({ink:'笔画',highlight:'文字高亮'}[mark.kind]||'批注'),'pen',()=>{active.selected=mark.id;el('readerMarkText').value=mark.text||'';el('readerMarkColor').value=mark.color||'#d8465b';overlay();});row.classList.add('workspace-linked-row');row.setAttribute('aria-pressed',String(active.selected===mark.id));list.append(row);}el('readerDeleteMark').disabled=el('readerSaveMark').disabled=!active.selected;
    }
    async function pdfLibrary(){if(!library){library=await import(YantuEnvironment.pathname('/vendor/pdfjs-5.6.205/build/pdf.min.mjs'));library.GlobalWorkerOptions.workerSrc=YantuEnvironment.pathname('/vendor/pdfjs-5.6.205/build/pdf.worker.min.mjs');}return library;}
    const aborted=()=>Object.assign(new Error('原件读取已取消'),{name:'AbortException'});
    function stop(saved){
        if(!saved||saved.cancelled)return;saved.cancelled=true;saved.cancelMetadata?.();saved.cancelLoad?.();saved.renderTask?.cancel();saved.textTask?.cancel();
        for(const observer of saved.observers||[])observer.disconnect();for(const task of saved.thumbnailTasks||[])task.cancel();
        const task=saved.loadingTask||saved.pdf;try{Promise.resolve(task?.destroy()).catch(()=>{});}catch(_){}saved.image?.close?.();
    }
    const reliableMetadata=file=>Number.isSafeInteger(file?.size)&&file.size>0&&/^[a-f0-9]{64}$/i.test(file.sha256||'');
    function httpOriginal(file){
        if(!isPDF(file)||file.pendingUpload||navigator.onLine===false)return false;
        try{return /^https?:$/.test(new URL(PrepTransport.base?.()||location.href).protocol);}catch(_){return false;}
    }
    const rangeAvailable=file=>httpOriginal(file)&&reliableMetadata(file);
    async function pdfMetadata(file,owner){
        if(file.sha256||!httpOriginal(file))return file;
        const cached=await DeviceStore.get('file-meta-'+file.id).catch(()=>null);if(owner.cancelled)throw aborted();
        if(cached?.id===file.id&&reliableMetadata(cached))return {...file,size:cached.size,sha256:cached.sha256.toLowerCase()};
        const controller=new AbortController();let interrupt;
        const interrupted=new Promise((_,reject)=>{interrupt=reject;}),timeout=setTimeout(()=>{controller.abort();interrupt(new Error('原件元数据读取超时'));},8000);
        owner.cancelMetadata=()=>{clearTimeout(timeout);controller.abort();interrupt(aborted());};
        try{
            const response=await Promise.race([PrepTransport.fetch('/api/files/'+encodeURIComponent(file.id),{method:'HEAD',credentials:'same-origin',signal:controller.signal}),interrupted]);
            if(owner.cancelled)throw aborted();const sha256=response.headers.get('X-File-SHA256'),length=response.headers.get('Content-Length'),metadata={sha256,size:/^\d+$/.test(length||'')?Number(length):0};
            if(response.status===200&&response.headers.get('Accept-Ranges')?.toLowerCase()==='bytes'&&reliableMetadata(metadata))return {...file,...metadata,sha256:sha256.toLowerCase()};
        }catch(error){if(owner.cancelled)throw aborted();}finally{clearTimeout(timeout);owner.cancelMetadata=null;}
        return file;
    }
    async function loadPDF(file,bytes,owner,range=false){
        const lib=await pdfLibrary();if(owner.cancelled)throw aborted();
        const options={cMapUrl:YantuEnvironment.pathname('/vendor/pdfjs-5.6.205/cmaps/'),cMapPacked:true,standardFontDataUrl:YantuEnvironment.pathname('/vendor/pdfjs-5.6.205/standard_fonts/'),wasmUrl:YantuEnvironment.pathname('/vendor/pdfjs-5.6.205/wasm/'),isEvalSupported:false};
        if(!range){owner.loadingTask=lib.getDocument({...options,data:new Uint8Array(bytes)});return owner.loadingTask.promise;}
        const controllers=new Map();let task,failure,ended=false;
        const failed=new Promise((_,reject)=>{failure=reject;});
        failed.catch(()=>{});
        const cancel=()=>{for(const [controller,timeout]of controllers){clearTimeout(timeout);controller.abort();}controllers.clear();};
        const fail=error=>{if(ended)return;ended=true;cancel();failure(error);if(active===owner&&!owner.cancelled)el('readerStatus').textContent=error.message;try{Promise.resolve(task?.destroy()).catch(()=>{});}catch(_){}};
        owner.cancelLoad=()=>{if(!ended){ended=true;failure(aborted());}cancel();};
        class ResourceRange extends lib.PDFDataRangeTransport{
            constructor(){super(file.size,new Uint8Array(),true,file.name);}
            requestDataRange(begin,end){
                if(ended||owner.cancelled)return;
                if(!Number.isSafeInteger(begin)||!Number.isSafeInteger(end)||begin<0||end<=begin||end>file.size){fail(new Error('原件分段范围无效'));return;}
                const controller=new AbortController(),timeout=setTimeout(()=>fail(new Error('原件连接超时，请确认电脑在线后重试')),45000);controllers.set(controller,timeout);
                PrepTransport.fetch('/api/files/'+encodeURIComponent(file.id),{credentials:'same-origin',headers:{Range:`bytes=${begin}-${end-1}`},signal:controller.signal}).then(async response=>{
                    if(ended||owner.cancelled)return;
                    const length=response.headers.get('Content-Length'),encoding=response.headers.get('Content-Encoding');
                    if(response.status!==206||response.headers.get('Content-Range')!==`bytes ${begin}-${end-1}/${file.size}`||(length!==null&&(!/^\d+$/.test(length)||Number(length)!==end-begin))||(encoding&&encoding!=='identity'))throw new Error('原件分段响应校验失败，请检查电脑连接');
                    const chunk=new Uint8Array(await response.arrayBuffer());if(chunk.length!==end-begin)throw new Error('原件分段不完整，请重新连接');if(!ended&&!owner.cancelled)this.onDataRange(begin,chunk);
                }).catch(error=>{if(!ended&&!owner.cancelled)fail(error);}).finally(()=>{clearTimeout(timeout);controllers.delete(controller);});
            }
            abort(){cancel();}
        }
        task=owner.loadingTask=lib.getDocument({...options,range:new ResourceRange(),rangeChunkSize:262144,disableAutoFetch:true,disableStream:true});
        return Promise.race([task.promise,failed]);
    }
    async function localOriginal(file,owner,cachedOnly=false){
        const blob=cachedOnly?await DeviceStore.get('file-'+file.id):await getFile(file.id);if(owner.cancelled)throw aborted();if(!blob)throw new Error('此原件尚未离线缓存');
        const bytes=await blob.arrayBuffer();if(owner.cancelled)throw aborted();
        const fingerprint=[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(value=>value.toString(16).padStart(2,'0')).join('');if(owner.cancelled)throw aborted();
        if(/^[a-f0-9]{64}$/i.test(file.sha256||'')&&(fingerprint!==file.sha256.toLowerCase()||(Number.isSafeInteger(file.size)&&file.size>0&&file.size!==bytes.byteLength)))throw new Error('本机原件与已保存版本不一致，请保留备份并重新核对');
        Object.assign(owner,{fingerprint,fingerprintSource:'sha256',hashVerified:true});return {blob,bytes};
    }
    async function original(file,owner){
        file=await pdfMetadata(file,owner);if(owner.cancelled)throw aborted();owner.file=file;
        if(rangeAvailable(file)){
            Object.assign(owner,{fingerprint:file.sha256.toLowerCase(),fingerprintSource:'metadata',hashVerified:false});
            try{return {pdf:await loadPDF(file,null,owner,true)};}catch(error){
                owner.cancelLoad?.();owner.cancelLoad=null;try{Promise.resolve(owner.loadingTask?.destroy()).catch(()=>{});}catch(_){}owner.loadingTask=null;
                if(owner.cancelled)throw error;const cached=await DeviceStore.get('file-'+file.id).catch(()=>null);if(!cached)throw error;
            }
            const {bytes}=await localOriginal(file,owner,true);return {pdf:await loadPDF(file,bytes,owner)};
        }
        const value=await localOriginal(file,owner);return isPDF(file)?{pdf:await loadPDF(file,value.bytes,owner)}:{image:await createImageBitmap(value.blob)};
    }
    async function canonicalPage(file,number=1){
        const key=`page-${file.id}-${number}`,cached=await DeviceStore.get(key);
        if(cached?.canonical&&(!file.sha256||file.sha256===cached.fingerprint))return createImageBitmap(cached.blob);
        const owner={};try{const {pdf,image}=await original(file,owner);if(image)return image;owner.pdf=pdf;
            if(number<1||number>pdf.numPages)throw new Error('卡片来源页码超出原件');const page=await pdf.getPage(number),base=page.getViewport({scale:1}),viewport=page.getViewport({scale:Math.min(2,1600/Math.max(base.width,base.height))}),canvas=document.createElement('canvas');canvas.width=Math.ceil(viewport.width);canvas.height=Math.ceil(viewport.height);owner.renderTask=page.render({canvasContext:canvas.getContext('2d'),viewport});await owner.renderTask.promise;const imageBlob=await new Promise(resolve=>canvas.toBlob(resolve));if(!imageBlob)throw new Error('原页图像生成失败');DeviceStore.put(key,{blob:imageBlob,pages:pdf.numPages,canonical:true,...fingerprintInfo(owner)}).catch(()=>{});return await createImageBitmap(imageBlob);
        }finally{stop(owner);}
    }
    async function previewPage(file,number=1,{signal}={}){
        const owner={};let timedOut=false;
        const cancelled=()=>{if(signal?.aborted)throw Object.assign(new Error('页面读取已取消'),{name:'AbortError'});if(timedOut)throw new Error('原文备用读取超时，请确认电脑连接后重试');if(owner.cancelled)throw aborted();};
        const cancel=()=>stop(owner),timer=setTimeout(()=>{timedOut=true;stop(owner);},35000);signal?.addEventListener('abort',cancel,{once:true});
        try{
            cancelled();file=await pdfMetadata(file,owner);cancelled();
            // The fallback must not pull a whole large textbook into memory.
            if(!rangeAvailable(file)&&!(await DeviceStore.get('file-'+file.id).catch(()=>null))&&(!file.size||file.size>8*1024*1024))throw new Error('原文暂不支持分段读取，请确认电脑在线后重试');
            const value=await original(file,owner);cancelled();owner.pdf=value.pdf;owner.image=value.image;
            if(!value.pdf)throw new Error('此附件不是 PDF');
            if(!Number.isInteger(number)||number<1||number>value.pdf.numPages)throw new Error('页码超出 PDF 范围');
            const page=await value.pdf.getPage(number);cancelled();const base=page.getViewport({scale:1});
            const scale=Math.min(2,2200/Math.max(base.width,base.height),Math.sqrt(MAX_PIXELS/(base.width*base.height))),viewport=page.getViewport({scale});
            const surface=document.createElement('canvas');surface.width=Math.ceil(viewport.width);surface.height=Math.ceil(viewport.height);
            owner.renderTask=page.render({canvasContext:surface.getContext('2d'),viewport});await owner.renderTask.promise;cancelled();
            const blob=await new Promise(resolve=>surface.toBlob(resolve,'image/png'));cancelled();if(!blob)throw new Error('原页图像生成失败');
            return {blob,pages:value.pdf.numPages,canonical:true,...fingerprintInfo(owner)};
        }catch(error){cancelled();if(error.name==='PasswordException')throw new Error('这份 PDF 需要密码，请先使用原文阅读确认文件权限');throw error;}
        finally{clearTimeout(timer);signal?.removeEventListener('abort',cancel);stop(owner);}
    }
    function fingerprintInfo(saved){return {fingerprint:saved.fingerprint,fingerprintSource:saved.fingerprintSource,hashVerified:saved.hashVerified,fileId:saved.file?.id,sourceSize:saved.file?.size};}
    async function textLayer(saved,page,viewport,number,current){
        try{const text=await page.getTextContent();if(!current())return;const layer=el('readerText');el('readerPaper').style.setProperty('--total-scale-factor',viewport.scale);saved.textTask=new library.TextLayer({textContentSource:text,container:layer,viewport});await saved.textTask.render();if(!current())return;
            const textValue=text.items.filter(item=>typeof item.str==='string').map(item=>item.str).join(' ');DeviceStore.put(`reader-text-${saved.file.id}-${number}`,{text:textValue,fileId:saved.file.id,page:number,...fingerprintInfo(saved)}).catch(()=>{});
        }catch(error){if(current()&&error.name!=='AbortException')el('readerText').title='文字层暂不可用，原页及批注仍保留';}
    }
    async function render(){
        clearTimeout(zoomTimer);const request=++rendering;if(!active||(!active.pdf&&!active.image))return;const saved=active,number=saved.page,rotation=saved.rotation,zoom=saved.zoom,current=()=>request===rendering&&active===saved&&!saved.cancelled;el('readerStatus').textContent='正在打开第 '+number+' 页…';saved.renderTask?.cancel();saved.textTask?.cancel();
        try{
            const pdf=saved.pdf,page=pdf?await pdf.getPage(number):null,base=page?page.getViewport({scale:1}):{width:saved.image.width,height:saved.image.height};
            const sideways=rotation%180!==0,baseWidth=sideways?base.height:base.width,available=Math.max(220,el('readerStage').clientWidth-40),scale=Math.min(2.5,available/baseWidth)*zoom/100;
            const viewport=page?page.getViewport({scale,rotation:(page.rotate+rotation)%360}):{width:(sideways?base.height:base.width)*scale,height:(sideways?base.width:base.height)*scale,scale};
            if(!current())return;const canvas=el('readerCanvas'),dpr=Math.min(2,devicePixelRatio||1,Math.sqrt(MAX_PIXELS/(viewport.width*viewport.height)),MAX_SIDE/viewport.width,MAX_SIDE/viewport.height),wrap=el('readerPaper');canvas.width=Math.max(1,Math.floor(viewport.width*dpr));canvas.height=Math.max(1,Math.floor(viewport.height*dpr));wrap.style.width=viewport.width+'px';wrap.style.height=viewport.height+'px';canvas.style.width=viewport.width+'px';canvas.style.height=viewport.height+'px';
            const ctx=canvas.getContext('2d');ctx.fillStyle='#fff';ctx.fillRect(0,0,canvas.width,canvas.height);el('readerText').replaceChildren();el('readerText').title='';
            if(page){saved.renderTask=page.render({canvasContext:ctx,viewport,transform:[dpr,0,0,dpr,0,0]});await saved.renderTask.promise;}
            else{ctx.save();ctx.translate(canvas.width/2,canvas.height/2);ctx.rotate(rotation*Math.PI/180);ctx.drawImage(saved.image,-base.width*scale*dpr/2,-base.height*scale*dpr/2,base.width*scale*dpr,base.height*scale*dpr);ctx.restore();}
            if(!current())return;el('readerPage').value=number;el('readerPageCount').textContent='/ '+saved.pages;el('readerPrevious').disabled=number===1;el('readerNext').disabled=number===saved.pages;el('readerStatus').textContent=saved.versionWarning||'';overlay();savePosition();renderBookmarks();
            if(page)textLayer(saved,page,viewport,number,current);
            if(rotation===0)canvas.toBlob(blob=>{if(blob&&current())DeviceStore.put(`page-${saved.file.id}-${number}`,{blob,pages:saved.pages,canonical:true,...fingerprintInfo(saved)}).catch(()=>{});});
        }catch(error){if(request===rendering&&error.name!=='RenderingCancelledException'&&error.name!=='AbortException')el('readerStatus').textContent=error.message+'；可返回下载原件';}
    }
    async function persistPosition(saved=active){if(!saved?.pages)return;let position=(state.readingPositions||[]).find(item=>item.fileId===saved.file.id);if(position&&position.sourceId===saved.id&&position.page===saved.page&&position.zoom===saved.zoom&&position.rotation===saved.rotation)return;position={...position,id:position?.id||uid(),fileId:saved.file.id,sourceId:saved.id,page:saved.page,zoom:saved.zoom,rotation:saved.rotation,updatedAt:PrepLocal.nowDate().toISOString()};await W.put('readingPositions',position,{background:true});}
    function savePosition(){clearTimeout(positionTimer);const saved=active;if(!saved)return;positionTimer=setTimeout(()=>{if(active===saved)persistPosition(saved).catch(error=>PrepLocal.toast(error.message));},400);}
    async function open(id,index=0,page,anchor,snapshot){
        const source=snapshot||W.record(id)?.record,file=source&&StudyLibrary.files(source)[index];if(!file)return;const request=++opening,previous=active;if(previous){saveNote().catch(error=>PrepLocal.toast(error.message));persistPosition(previous).catch(error=>PrepLocal.toast(error.message));}rendering++;clearTimeout(positionTimer);clearTimeout(draftTimer);clearTimeout(zoomTimer);stop(previous);pointer=null;
        const position=(state.readingPositions||[]).filter(item=>item.fileId===file.id).sort((a,b)=>String(b.updatedAt).localeCompare(String(a.updatedAt)))[0];const saved=active={id,file,index,source:clone(source),page:sourcePage(page)||sourcePage(position?.page)||1,rotation:[0,90,180,270].includes(position?.rotation)?position.rotation:0,zoom:Math.max(50,Math.min(250,Number(position?.zoom)||100)),tool:'select',anchor,selected:null,original:false};selection=[];
        const dialog=el('readerDialog');if(!dialog.open)dialog.showModal();el('readerTitle').textContent=source.title+' · '+file.name;el('readerStatus').textContent='读取原件…';el('readerNotes').value='';el('readerZoom').value=active.zoom;
        el('readerCanvas').width=el('readerCanvas').height=0;el('readerText').replaceChildren();el('readerOverlay').replaceChildren();el('readerMarkList').replaceChildren();el('readerThumbnails').replaceChildren();el('readerThumbnails').hidden=true;
        initializeNote(saved);saved.notesReady=restoreNote(saved).catch(()=>{saved.draftUnreadable=true;noteConflict(saved);PrepLocal.toast('本机草稿读取失败，原草稿未覆盖；当前输入将单独保留');});
        DeviceStore.get('reader-region-draft-'+file.id).then(regions=>{if(active===saved&&!selection.length&&Array.isArray(regions)){selection=regions;if(saved.pages){overlay();renderBookmarks();}}}).catch(()=>{});
        try{const value=await original(file,saved);if(active!==saved||request!==opening){value.image?.close?.();stop(saved);return;}Object.assign(saved,value);saved.pages=value.pdf?value.pdf.numPages:1;
            if(anchor?.fingerprint&&anchor.fingerprint!==saved.fingerprint)saved.versionWarning='当前文件版本与引用不一致，原引用未移动；请核对原始备份';
            saved.page=Math.min(saved.pages,Math.max(1,saved.page));StudyLibrary.touch(id);await render();
        }catch(error){if(active===saved)el('readerStatus').textContent=error.message;}
    }
    async function move(step){if(!active?.pages||!Number.isFinite(step))return;active.page=Math.max(1,Math.min(active.pages,Math.trunc(active.page+step)));active.anchor=null;active.selected=null;pointer=null;await render();}
    const linkedNote=(note,saved)=>note?.sourceId===saved.id&&note?.sourceFileId===saved.file.id;
    const noteVersion=note=>JSON.stringify(note||null);
    const sourcePage=value=>Number.isSafeInteger(value)&&value>0?value:undefined;
    function initializeNote(saved){
        const note=(state.notes||[]).filter(item=>linkedNote(item,saved)).sort((a,b)=>String(b.updatedAt||'').localeCompare(String(a.updatedAt||'')))[0];
        saved.noteBase=note?clone(note):null;saved.noteId=note?.id||'';saved.notePage=sourcePage(note?.sourcePage);saved.noteRevision=0;el('readerNotes').value=note?.content||'';
    }
    function noteConflict(saved){saved.noteConflict=true;if(!saved.conflictWarned){saved.conflictWarned=true;PrepLocal.toast('原笔记已变化或草稿来源不明确，本机草稿已保留；请在学习笔记中核对后再编辑，未覆盖原笔记');}}
    async function restoreNote(saved){
        const draft=await DeviceStore.get('reader-note-draft-'+saved.file.id);if(!draft||typeof draft.content!=='string')return;
        const current=saved.noteBase,draftNote=(state.notes||[]).find(item=>item.id===draft.noteId);
        const foreign=(draft.sourceId&&draft.sourceId!==saved.id)||(draft.sourceFileId&&draft.sourceFileId!==saved.file.id)||(draftNote&&!linkedNote(draftNote,saved));
        if(foreign){saved.conflictDraft=clone(draft);noteConflict(saved);return;}
        const same=Boolean(current&&(!draft.noteId||draft.noteId===current.id)&&draft.content.trim()===String(current.content||'').trim());
        const safe=(!draft.noteId||Boolean(draftNote&&current&&draftNote.id===current.id))&&(same||(Object.hasOwn(draft,'base')?noteVersion(draft.base)===noteVersion(current):!draft.noteId&&!current));
        if(!safe){saved.conflictDraft=clone(draft);noteConflict(saved);}
        if(active!==saved||saved.noteRevision)return;
        el('readerNotes').value=draft.content;saved.notePage=sourcePage(current?.sourcePage)||sourcePage(draft.sourcePage);
        if(!same&&Object.hasOwn(draft,'base'))saved.noteBase=clone(draft.base);
        if(draft.noteId&&!same)saved.noteId=draft.noteId;
    }
    function cacheNoteDraft(saved,content,page=saved.notePage){
        const key='reader-note-draft-'+saved.file.id,base=clone(saved.noteBase),noteId=saved.noteId;
        const value={content,noteId,sourceId:saved.id,sourceFileId:saved.file.id,sourcePage:page,base,updatedAt:PrepLocal.nowDate().toISOString()};
        const write=(draftWrites.get(key)||Promise.resolve()).catch(()=>{}).then(async()=>{
            await saved.notesReady;
            if(saved.draftUnreadable){await DeviceStore.put('rescue-draft-conflict-reader-'+uid(),{key,current:null,incoming:value,createdAt:PrepLocal.nowDate().toISOString(),resolved:''});return;}
            if(saved.conflictDraft&&!saved.draftArchived){await DeviceStore.put('rescue-draft-conflict-reader-'+uid(),{key,current:clone(saved.conflictDraft),incoming:value,createdAt:PrepLocal.nowDate().toISOString(),resolved:''});saved.draftArchived=true;}
            await DeviceStore.put(key,value);
        });draftWrites.set(key,write);write.finally(()=>{if(draftWrites.get(key)===write)draftWrites.delete(key);}).catch(()=>{});return write;
    }
    function saveNote(){
        if(!active)return Promise.resolve();const saved=active,content=el('readerNotes').value.trim();if(!content)return Promise.resolve();
        saved.notePage||=saved.page;const page=saved.notePage,revision=saved.noteRevision;
        const saving=(saved.noteSaving||Promise.resolve()).catch(()=>{}).then(async()=>{
            await saved.notesReady;
            if(saved.noteRevision!==revision)return;
            const existing=(state.notes||[]).find(item=>item.id===saved.noteId),linked=(state.notes||[]).filter(item=>linkedNote(item,saved));
            if(saved.noteConflict||noteVersion(existing)!==noteVersion(saved.noteBase)||(!saved.noteId&&linked.length)){
                noteConflict(saved);await cacheNoteDraft(saved,content,page);return;
            }
            if(existing?.content===content&&sourcePage(existing.sourcePage))return;
            const source=W.record(saved.id)?.record||saved.source,note={...existing,id:existing?.id||uid(),title:existing?.title||source?.title+' · 阅读笔记',content,path:existing?.path||source?.path||source?.subject||'阅读',date:existing?.date||PrepLocal.dateText(PrepLocal.nowDate()),sourceId:saved.id,sourceFileId:saved.file.id,sourcePage:sourcePage(existing?.sourcePage)||saved.notePage||page,knowledgeIds:existing?.knowledgeIds||source?.knowledgeIds||[],updatedAt:PrepLocal.nowDate().toISOString()};
            await W.put('notes',note,{background:true});saved.noteId=note.id;saved.noteBase=clone(note);saved.notePage=note.sourcePage;
            // Keep the recovery draft even after local commit; remote synchronization is independent.
            if(saved.noteRevision===revision&&!saved.cancelled)cacheNoteDraft(saved,content,note.sourcePage).catch(()=>PrepLocal.toast('本机草稿保存失败，请保留当前笔记'));
        });saved.noteSaving=saving;return saving;
    }
    function saveMark(mark){const saving=W.put('readingMarks',mark,{background:true});overlay();renderBookmarks();return saving.catch(error=>PrepLocal.toast(error.message));}
    function deleteMark(id){if(!confirm('将这条记录移入回收站？'))return;state.readingMarks=(state.readingMarks||[]).filter(mark=>mark.id!==id);if(active?.selected===id)active.selected=null;const saving=W.persist({background:true});overlay();renderBookmarks();return saving.catch(error=>PrepLocal.toast(error.message));}
    function setTool(tool){active.tool=tool;el('readerDialog').querySelectorAll('[data-reader-tool]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.readerTool===tool)));overlay();}
    async function saveRegions(){await DeviceStore.put('reader-region-draft-'+active.file.id,clone(selection));el('readerRegionCount').textContent=selection.reduce((sum,item)=>sum+item.regions.length,0)+' 个题目区域';}
    function selectedText(){const chosen=getSelection();if(!chosen?.rangeCount||!el('readerText').contains(chosen.anchorNode)||!el('readerText').contains(chosen.focusNode))throw new Error('请先在 PDF 文字中选择需要高亮的原文');const bounds=el('readerPaper').getBoundingClientRect(),regions=[];for(const box of chosen.getRangeAt(0).getClientRects()){const a=inverse([Math.max(0,(box.left-bounds.left)/bounds.width),Math.max(0,(box.top-bounds.top)/bounds.height)],active.rotation),b=inverse([Math.min(1,(box.right-bounds.left)/bounds.width),Math.min(1,(box.bottom-bounds.top)/bounds.height)],active.rotation),region=rectangle(a,b);if(region[2]>.001&&region[3]>.001)regions.push(region);}return {text:chosen.toString(),regions};}
    async function highlight(){try{const selected=selectedText();if(!selected.regions.length)throw new Error('未取得选中文字区域');const saving=saveMark({id:uid(),fileId:active.file.id,sourceId:active.id,page:active.page,...fingerprintInfo(active),kind:'highlight',color:el('readerMarkColor').value,...selected});getSelection().removeAllRanges();await saving;}catch(error){PrepLocal.toast(error.message);}}
    function bookmark(){const existing=(state.readingMarks||[]).find(mark=>mark.fileId===active.file.id&&mark.page===active.page&&mark.kind==='bookmark'&&matchingMark(mark,active.file));return existing?deleteMark(existing.id):saveMark({id:uid(),fileId:active.file.id,sourceId:active.id,page:active.page,kind:'bookmark',text:'第 '+active.page+' 页',...fingerprintInfo(active)});}
    function renderBookmarks(){const target=el('readerBookmarks');target.replaceChildren();for(const mark of(state.readingMarks||[]).filter(mark=>mark.fileId===active.file.id&&mark.kind==='bookmark'&&matchingMark(mark,active.file)))target.append(button(mark.text,'bookmark',async()=>{active.page=mark.page;await render();}));el('readerRegionCount').textContent=selection.reduce((sum,item)=>sum+item.regions.length,0)+' 个题目区域';}
    async function thumbnails(){const target=el('readerThumbnails');target.replaceChildren();if(!active.pdf){target.hidden=true;return;}const saved=active;for(const observer of saved.observers||[])observer.disconnect();for(const task of saved.thumbnailTasks||[])task.cancel();saved.observers=new Set();saved.thumbnailTasks=new Set();target.hidden=!target.hidden;if(target.hidden)return;
        for(let number=1;number<=Math.min(saved.pages,120);number++){const thumb=button(String(number),'file',async()=>{if(active!==saved)return;active.page=number;await render();});const image=document.createElement('canvas');image.width=72;image.height=100;thumb.prepend(image);target.append(thumb);const observer=new IntersectionObserver(async entries=>{if(!entries.some(item=>item.isIntersecting))return;observer.disconnect();saved.observers.delete(observer);let task;try{if(active!==saved)return;const page=await saved.pdf.getPage(number);if(active!==saved||target.hidden)return;const base=page.getViewport({scale:1}),viewport=page.getViewport({scale:Math.min(72/base.width,100/base.height)});image.width=Math.ceil(viewport.width);image.height=Math.ceil(viewport.height);task=page.render({canvasContext:image.getContext('2d'),viewport});saved.thumbnailTasks.add(task);await task.promise;}catch(_){}finally{saved.thumbnailTasks.delete(task);}},{root:target});saved.observers.add(observer);observer.observe(thumb);}if(saved.pages>120){const more=document.createElement('p');more.textContent='前 120 页缩略图，其他页可用页码直达';target.append(more);}}
    function createPreview(){if(!selection.length){PrepLocal.toast('请先使用框选工具选择题目区域，可跨页多选');return;}el('readerQuestionForm').reset();el('readerQuestionTitle').value=(W.record(active.id)?.record.title||active.source.title)+' · 题目';el('readerQuestionPreview').textContent=selection.map(item=>'第 '+item.page+' 页：'+item.regions.length+' 个区域').join('；');el('readerQuestionDialog').showModal();}
    async function createQuestion(event){event.preventDefault();const form=event.target,source=W.record(active.id)||{collection:'examSessions',record:active.source},id=uid(),anchors=selection.map(entry=>({id:uid(),sourceId:active.id,sourceCollection:source.collection,fileId:active.file.id,fileName:active.file.name,...fingerprintInfo(active),page:entry.page,regions:clone(entry.regions),questionNumber:form.elements.number.value,title:form.elements.title.value}));
        const mistake={id,title:form.elements.title.value,content:form.elements.content.value,date:PrepLocal.dateText(PrepLocal.nowDate()),path:(source.record.path||source.record.subject||'未分类')+' > 题目',sourcePaperId:source.collection==='papers'?source.record.id:'',questionNumber:form.elements.number.value,anchorIds:anchors.map(item=>item.id),knowledgeIds:source.record.knowledgeIds||[],knowledgePoints:source.record.knowledgePoints||[],files:[],mastery:'new'};(state.sourceAnchors||=[]).push(...anchors);(state.mistakes||=[]).unshift(mistake);await W.persist();selection=[];await saveRegions();el('readerQuestionDialog').close();overlay();PrepLocal.toast(PrepLocal.hasPending()?'题卡已保存在本机，等待同步':'题卡和来源定位已保存');}
    async function openAnchor(id){const anchor=(state.sourceAnchors||[]).find(item=>item.id===id),source=anchor&&W.record(anchor.sourceId);if(!source){PrepLocal.toast('来源记录已删除，请从回收站恢复；引用版本仍保留');return;}const index=StudyLibrary.files(source.record).findIndex(file=>file.id===anchor.fileId);if(index<0){PrepLocal.toast('来源已更换附件，旧引用未自动迁移；请从历史版本恢复对应原件');return;}await open(anchor.sourceId,index,anchor.page,anchor);}
    async function exportMarks(){try{await saveNote();await PrepLocal.flush();if(PrepLocal.hasPending())throw new Error('请先连接电脑完成同步；离线时可导出开放批注文件');const result=await PrepTransport.fetch('/api/workspace/reading-export',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({fileId:active.file.id})});if(!result.ok)throw new Error((await result.json()).error);downloadBlob(active.file.name.replace(/\.pdf$/i,'')+'-矢量批注.pdf',await result.blob());}catch(error){PrepLocal.toast(error.message);}}
    function exportJSON(){const value={format:'yantu-reading-v1',file:{...active.file,...fingerprintInfo(active)},marks:(state.readingMarks||[]).filter(item=>item.fileId===active.file.id),anchors:(state.sourceAnchors||[]).filter(item=>item.fileId===active.file.id)};downloadBlob(active.file.name+'-批注.json',new Blob([JSON.stringify(value,null,2)],{type:'application/json'}));}
    function noteSource(note){
        if(!note.sourceId||!note.sourceFileId)return null;const source=W.record(note.sourceId)?.record;if(!source)return null;
        const files=StudyLibrary.files(source),index=files.findIndex(file=>file.id===note.sourceFileId&&isPDF(file));return index<0?null:{source,index};
    }
    function preview(id,index,page){return window.MediaTools?.preview?window.MediaTools.preview(id,index,page):window.ReaderWorkspace.open(id,index,page);}
    function noteConnections(note,target){
        if(!note.sourceId&&!note.sourceFileId)return;const linked=noteSource(note);
        if(!linked){const warning=document.createElement('p');warning.textContent='来源记录已删除或对应 PDF 附件不可用，原笔记仍保留；可从回收站或备份恢复来源。';target.append(warning);return;}
        const page=sourcePage(note.sourcePage)||1;
        target.append(button('来源 PDF · 第 '+page+' 页','book-open',()=>{const latest=noteSource(note);if(!latest){PrepLocal.toast('来源记录或 PDF 附件已不可用，原笔记仍保留');return;}return preview(note.sourceId,latest.index,page);}));
        if(document.getElementById('studyPDFPreview'))return;
        const file=StudyLibrary.files(linked.source)[linked.index],savedMarks=(state.readingMarks||[]).filter(mark=>mark.fileId===note.sourceFileId&&(!mark.sourceId||mark.sourceId===note.sourceId)&&mark.page===page&&matchingMark(mark,file)),summary=document.createElement('p');
        summary.textContent='来源第 '+page+' 页批注：'+(savedMarks.length?savedMarks.map(mark=>String(mark.text||({ink:'笔画',highlight:'文字高亮',bookmark:'书签'}[mark.kind]||'批注')).slice(0,160)).join('；'):'暂无已保存批注');target.append(summary);
    }
    function setup(){
        const dialog=document.createElement('dialog');dialog.id='readerDialog';dialog.className='reader-workspace';dialog.innerHTML='<header class="reader-header"><button id="readerClose" class="btn btn-icon" title="返回" aria-label="返回"><i class="fas fa-arrow-left"></i></button><h3 id="readerTitle"></h3><button id="readerFullscreen" class="btn btn-icon" title="全屏" aria-label="全屏"><i class="fas fa-expand"></i></button></header><div class="reader-toolbar"><div role="group" aria-label="阅读模式"><button data-reader-tool="select" title="选择文字" aria-label="选择文字" class="btn btn-icon" aria-pressed="true"><i class="fas fa-arrow-pointer"></i></button><button data-reader-tool="region" title="框选题目区域" aria-label="框选题目区域" class="btn btn-icon"><i class="fas fa-vector-square"></i></button><button data-reader-tool="pen" title="画笔" aria-label="画笔" class="btn btn-icon"><i class="fas fa-pen"></i></button><button data-reader-tool="move" title="移动选中批注" aria-label="移动选中批注" class="btn btn-icon"><i class="fas fa-up-down-left-right"></i></button><button data-reader-tool="erase" title="擦除笔画" aria-label="擦除笔画" class="btn btn-icon"><i class="fas fa-eraser"></i></button></div><button id="readerHighlight" class="btn btn-icon" title="高亮选中文字" aria-label="高亮选中文字"><i class="fas fa-highlighter"></i></button><button id="readerRotate" class="btn btn-icon" title="旋转" aria-label="旋转"><i class="fas fa-rotate-right"></i></button><button id="readerOriginal" class="btn btn-icon" title="对比原件" aria-label="对比原件" aria-pressed="false"><i class="fas fa-eye"></i></button><button id="readerBookmark" class="btn btn-icon" title="切换本页书签" aria-label="切换本页书签"><i class="far fa-bookmark"></i></button><button id="readerThumbs" class="btn btn-icon" title="页缩略图" aria-label="页缩略图"><i class="fas fa-table-cells"></i></button><label class="reader-zoom"><i class="fas fa-magnifying-glass"></i><input id="readerZoom" type="range" min="50" max="250" value="100" step="10" aria-label="页面缩放" /></label><button id="readerMobileTab" class="btn btn-ghost">笔记</button></div><div class="reader-layout"><div class="reader-document"><div id="readerThumbnails" class="reader-thumbnails" hidden></div><div id="readerStage" class="reader-stage"><div id="readerPaper" class="reader-paper"><canvas id="readerCanvas"></canvas><div id="readerText" class="textLayer"></div><div id="readerOverlay"></div></div></div><p id="readerStatus" role="status"></p><nav class="reader-page-controls"><button id="readerPrevious" class="btn btn-icon" title="上一页" aria-label="上一页">&lt;</button><input id="readerPage" type="number" min="1" value="1" aria-label="PDF 页码" /><span id="readerPageCount"></span><button id="readerNext" class="btn btn-icon" title="下一页" aria-label="下一页">&gt;</button><span id="readerRegionCount"></span><button id="readerClearRegions" class="btn btn-icon" title="清除待建题卡区域" aria-label="清除待建题卡区域"><i class="fas fa-xmark"></i></button></nav></div><aside class="reader-notes"><section><h4>阅读笔记</h4><textarea id="readerNotes" rows="10" aria-label="阅读笔记"></textarea><button id="readerSaveNote" class="btn btn-primary"><i class="fas fa-check"></i> 保存笔记</button></section><section><h4>题目引用</h4><button id="readerCreate" class="btn btn-ghost"><i class="fas fa-plus"></i> 从框选区域建题卡</button></section><section><h4>本页批注</h4><div id="readerMarkList"></div><textarea id="readerMarkText" rows="3" aria-label="选中批注的文字"></textarea><div class="reader-mark-actions"><input id="readerMarkColor" type="color" value="#d8465b" aria-label="批注颜色" /><button id="readerSaveMark" class="btn btn-icon" title="保存选中批注" aria-label="保存选中批注"><i class="fas fa-check"></i></button><button id="readerDeleteMark" class="btn btn-icon" title="删除选中批注" aria-label="删除选中批注"><i class="fas fa-trash"></i></button></div></section><section><h4>书签</h4><div id="readerBookmarks"></div></section><section><h4>导出副本</h4><button id="readerExportPDF" class="btn btn-ghost"><i class="fas fa-file-pdf"></i> PDF 矢量批注</button><button id="readerExportJSON" class="btn btn-ghost"><i class="fas fa-download"></i> 开放批注文件</button></section></aside></div>';document.body.append(dialog);
        W.dialog('readerQuestionDialog','建立题卡','<form id="readerQuestionForm"><p id="readerQuestionPreview"></p><label>标题<input id="readerQuestionTitle" name="title" required /></label><label>题号<input name="number" /></label><label>题目文字与记录<textarea name="content" rows="5"></textarea></label><div class="dialog-actions"><button type="button" id="readerQuestionCancel" class="btn btn-ghost">退回</button><button type="submit" class="btn btn-primary">继续并保存题卡</button></div></form>');
        el('readerQuestionCancel').onclick=()=>el('readerQuestionDialog').close();el('readerQuestionForm').onsubmit=createQuestion;
        const nativeClose=dialog.close.bind(dialog);
        dialog.close=(...args)=>{const saved=active;saveNote().catch(error=>PrepLocal.toast(error.message));persistPosition(saved).catch(error=>PrepLocal.toast(error.message));clearTimeout(positionTimer);clearTimeout(draftTimer);clearTimeout(zoomTimer);opening++;rendering++;stop(saved);pointer=null;active=null;el('readerCanvas').width=el('readerCanvas').height=0;el('readerOverlay').replaceChildren();el('readerText').replaceChildren();nativeClose(...args);};
        const editPage=button('','pen-to-square',()=>{
            const saved=active;if(!saved)return;if(!window.MediaTools?.preview){PrepLocal.toast('页面批注编辑暂不可用');return;}
            const source=W.record(saved.id)?.record,index=source?StudyLibrary.files(source).findIndex(file=>file.id===saved.file.id):saved.index;
            if(index<0){PrepLocal.toast('来源附件已不可用，原批注仍保留');return;}
            dialog.close();return window.MediaTools.preview(saved.id,index,saved.page);
        });editPage.id='readerEditPage';editPage.className='btn btn-icon';editPage.title='编辑页面批注';editPage.setAttribute('aria-label','编辑页面批注');el('readerHighlight').after(editPage);
        el('readerClose').onclick=()=>dialog.close();dialog.oncancel=event=>{event.preventDefault();dialog.close();};
        el('readerFullscreen').onclick=()=>dialog.classList.toggle('is-fullscreen');el('readerPrevious').onclick=()=>move(-1);el('readerNext').onclick=()=>move(1);el('readerPage').onchange=event=>move(Number(event.target.value)-active.page);el('readerZoom').oninput=event=>{if(!active)return;active.zoom=Math.max(50,Math.min(250,Number(event.target.value)||100));clearTimeout(zoomTimer);zoomTimer=setTimeout(()=>{if(active)render();},80);};el('readerRotate').onclick=()=>{active.rotation=(active.rotation+90)%360;render();};el('readerOriginal').onclick=event=>{active.original=!active.original;event.currentTarget.setAttribute('aria-pressed',String(active.original));overlay();};dialog.querySelectorAll('[data-reader-tool]').forEach(button=>button.onclick=()=>setTool(button.dataset.readerTool));el('readerHighlight').onclick=highlight;el('readerBookmark').onclick=bookmark;el('readerThumbs').onclick=thumbnails;el('readerCreate').onclick=createPreview;el('readerClearRegions').onclick=async()=>{selection=[];overlay();await saveRegions();};el('readerSaveNote').onclick=()=>saveNote().catch(error=>PrepLocal.toast(error.message));
        el('readerNotes').oninput=()=>{if(!active)return;clearTimeout(draftTimer);const current=active;current.noteRevision++;current.notePage||=current.page;cacheNoteDraft(current,el('readerNotes').value).catch(()=>PrepLocal.toast('本机草稿保存失败，请立即导出或保持连接'));draftTimer=setTimeout(()=>{if(active===current)saveNote().catch(error=>PrepLocal.toast(error.message));},1200);};
        el('readerStage').onwheel=event=>{if(!active||!event.ctrlKey||!event.deltaY)return;event.preventDefault();el('readerZoom').value=Math.max(50,Math.min(250,active.zoom+(event.deltaY<0?10:-10)));el('readerZoom').oninput({target:el('readerZoom')});};
        el('readerDeleteMark').onclick=()=>deleteMark(active.selected);el('readerSaveMark').onclick=()=>{const mark=(state.readingMarks||[]).find(item=>item.id===active.selected);if(mark)return saveMark({...mark,text:el('readerMarkText').value,color:el('readerMarkColor').value});};el('readerExportPDF').onclick=exportMarks;el('readerExportJSON').onclick=exportJSON;el('readerMobileTab').onclick=()=>{dialog.classList.toggle('show-notes');el('readerMobileTab').textContent=dialog.classList.contains('show-notes')?'原文':'笔记';};
        const surface=el('readerOverlay');surface.onpointerdown=event=>{if(!active||active.tool==='select')return;event.preventDefault();surface.setPointerCapture(event.pointerId);const start=point(event);pointer={start,points:[start],before:null};if(['erase','move'].includes(active.tool)){const candidates=marks().filter(mark=>mark.kind!=='bookmark');let nearest;let distance=.04;for(const mark of candidates){const points=mark.kind==='ink'?mark.points:(mark.regions||[]).flatMap(([x,y,w,h])=>[[x,y],[x+w,y+h],[(x+x+w)/2,(y+y+h)/2]]);const value=Math.min(...points.map(([x,y])=>Math.hypot(x-start[0],y-start[1])));if(value<distance){distance=value;nearest=mark;}}if(nearest){active.selected=nearest.id;pointer.before=clone(nearest);if(active.tool==='erase'){pointer=null;deleteMark(nearest.id);return;}}else pointer=null;}overlay();};
        surface.onpointermove=event=>{if(!pointer||!active)return;const current=point(event);if(active.tool==='pen'){pointer.points.push(current);const canvas=surface.querySelector('canvas'),ctx=canvas.getContext('2d'),a=forward(pointer.points.at(-2),active.rotation),b=forward(current,active.rotation);ctx.strokeStyle=el('readerMarkColor').value;ctx.lineWidth=.003*Math.min(canvas.width,canvas.height);ctx.beginPath();ctx.moveTo(a[0]*canvas.width,a[1]*canvas.height);ctx.lineTo(b[0]*canvas.width,b[1]*canvas.height);ctx.stroke();}else if(active.tool==='region'){surface.querySelector('.reader-pending-region')?.remove();const host=document.createElement('div');host.className='reader-pending-region';regionBox(host,rectangle(pointer.start,current));surface.append(host);}else if(active.tool==='move'&&pointer.before){const dx=current[0]-pointer.start[0],dy=current[1]-pointer.start[1],mark=pointer.preview=clone(pointer.before),bound=value=>Math.max(0,Math.min(1,value));if(mark.kind==='ink')mark.points=mark.points.map(([x,y])=>[bound(x+dx),bound(y+dy)]);else mark.regions=mark.regions.map(([x,y,w,h])=>[Math.max(0,Math.min(1-w,x+dx)),Math.max(0,Math.min(1-h,y+dy)),w,h]);overlay();}};
        surface.onpointerup=async event=>{if(!pointer||!active)return;const pending=pointer;pointer=null;try{if(active.tool==='region'){const region=rectangle(pending.start,point(event));if(region[2]>.005&&region[3]>.005){let group=selection.find(item=>item.page===active.page);if(!group)selection.push(group={page:active.page,regions:[]});group.regions.push(region);overlay();await saveRegions();}}else if(active.tool==='pen')await saveMark({id:uid(),fileId:active.file.id,sourceId:active.id,page:active.page,...fingerprintInfo(active),kind:'ink',color:el('readerMarkColor').value,width:.003,points:pending.points,text:''});else if(active.tool==='move'&&pending.preview){const mark=(state.readingMarks||[]).find(item=>item.id===pending.before.id);if(noteVersion(mark)===noteVersion(pending.before))await saveMark(pending.preview);else PrepLocal.toast('该批注已变化，未覆盖最新批注');}}catch(error){PrepLocal.toast(error.message);}overlay();};surface.onpointercancel=()=>{pointer=null;overlay();};
        addEventListener('study-detail',event=>{const value=W.record(event.detail.id)?.record;if(!value)return;const target=el('studyConnections')||el('studyDetailBody');if(!target)return;for(const [index,file]of StudyLibrary.files(value).entries())if(isPDF(file)||file.type?.startsWith('image/'))target.append(button('阅读 '+file.name,'book-open',()=>preview(value.id,index)));for(const id of value.anchorIds||[]){const anchor=(state.sourceAnchors||[]).find(item=>item.id===id);if(anchor)target.append(button('来源第 '+anchor.page+' 页'+(anchor.questionNumber?' · 第 '+anchor.questionNumber+' 题':''),'location-dot',()=>openAnchor(id)));}if(event.detail.col==='notes'||(state.notes||[]).some(note=>note.id===value.id))noteConnections(value,target);});
        const refreshMarks=()=>{if(active?.pages){overlay();renderBookmarks();}};addEventListener('workspace-change',refreshMarks);addEventListener('yantu-sync',refreshMarks);
        let resize;addEventListener('resize',()=>{clearTimeout(resize);resize=setTimeout(()=>{if(active)render();},200);});
    }
    addEventListener('workspace-ready',setup,{once:true});
    addEventListener('workspace-open-record',event=>{if(event.detail.collection==='sourceAnchors')openAnchor(event.detail.record.id);});
    window.ReaderWorkspace={open,openSnapshot:(source,index=0,page,anchor)=>open(source.id,index,page,anchor,source),move,openAnchor,forward,inverse,mappedRect,render,highlight,exportJSON,canonicalPage,previewPage,debug:()=>active?{id:active.id,file:clone(active.file),page:active.page,rotation:active.rotation,pages:active.pages,selection:clone(selection),...fingerprintInfo(active)}:null};
})();
