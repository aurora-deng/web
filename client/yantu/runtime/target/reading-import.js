(() => {
    const W=Workspace,{el,clone,button}=W;let preview,busy=false,generation=0;
    const same=(existing,item)=>Object.entries(item).every(([key,value])=>key==='id'||JSON.stringify(existing[key])===JSON.stringify(value));
    const sha=async value=>[...new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(JSON.stringify(value))))].map(n=>n.toString(16).padStart(2,'0')).join('');
    const coordinate=value=>typeof value==='number'&&Number.isFinite(value)&&value>=0&&value<=1;
    function rectangles(value){if(!Array.isArray(value)||value.length>50||value.some(item=>!Array.isArray(item)||item.length!==4||!item.every(coordinate)||item[2]<=0||item[3]<=0||item[0]+item[2]>1.000001||item[1]+item[3]>1.000001))throw new Error('批注区域无效或超出原页');return clone(value);}
    function pageNumber(value,pages){if(!Number.isInteger(value)||value<1||value>pages)throw new Error('批注页码超出当前原件');return value;}
    async function prepare(file){
        if(busy)throw new Error('批注正在保存');const request=++generation,source=ReaderWorkspace.debug();if(!source)throw new Error('请先打开相应原件');if(file.size>12*1024**2)throw new Error('开放批注文件上限 12 MB');
        const value=JSON.parse(await file.text(),(key,value)=>{if(['__proto__','prototype','constructor'].includes(key))throw new Error('批注包含不安全字段');return value;});
        if(value?.format!=='yantu-reading-v1'||!Array.isArray(value.marks)||!Array.isArray(value.anchors)||value.marks.length+value.anchors.length>5000)throw new Error('不是受支持的开放批注文件');
        if(!source.fingerprint||value.file?.fingerprint!==source.fingerprint)throw new Error('原件版本不同，未迁移或覆盖任何批注；请打开导出时的原件');
        const additions=[];
        for(const original of value.marks){
            if(!original||!['bookmark','ink','highlight'].includes(original.kind))throw new Error('批注类型无效');
            const item={fileId:source.file.id,sourceId:source.id,fingerprint:source.fingerprint,page:pageNumber(original.page,source.pages),kind:original.kind,text:String(original.text||'').slice(0,100000)};
            if(original.fingerprint&&original.fingerprint!==source.fingerprint)throw new Error('批注内部原件版本不一致');
            if(original.kind==='ink'){if(!Array.isArray(original.points)||!original.points.length||original.points.length>10000||original.points.some(point=>!Array.isArray(point)||point.length!==2||!point.every(coordinate)))throw new Error('笔画坐标无效');item.points=clone(original.points);item.width=typeof original.width==='number'&&original.width>0&&original.width<=.1?original.width:.003;}
            if(original.kind==='highlight')item.regions=rectangles(original.regions);
            if(original.color&&/^#[a-f0-9]{6}$/i.test(original.color))item.color=original.color;
            const existing=state.readingMarks.find(mark=>same(mark,item));additions.push({collection:'readingMarks',record:{...item,id:existing?.id||'ri-'+(await sha(item)).slice(0,40)}});
        }
        for(const original of value.anchors){
            if(!original||typeof original!=='object')throw new Error('题目引用格式无效');if(original.fingerprint&&original.fingerprint!==source.fingerprint)throw new Error('题目引用原件版本不一致');
            const item={fileId:source.file.id,sourceId:source.id,fileName:source.file.name,fingerprint:source.fingerprint,page:pageNumber(original.page,source.pages),regions:rectangles(original.regions),questionNumber:String(original.questionNumber||'').slice(0,300),title:String(original.title||'导入的题目引用').slice(0,300)};
            const existing=state.sourceAnchors.find(anchor=>same(anchor,item));additions.push({collection:'sourceAnchors',record:{...item,id:existing?.id||'ri-'+(await sha(item)).slice(0,40)}});
        }
        if(request!==generation)return;const current=ReaderWorkspace.debug();if(current?.id!==source.id||current?.file.id!==source.file.id||current?.fingerprint!==source.fingerprint)throw new Error('当前原件已切换，请重新选择批注文件');
        const unique=[...new Map(additions.map(item=>[item.collection+':'+item.record.id,item])).values()];preview={source,additions:unique};
        const fresh=unique.filter(item=>!state[item.collection].some(record=>record.id===item.record.id));el('readingImportSummary').textContent='当前原件：'+source.file.name+'。新增 '+fresh.length+' 条，已存在 '+(unique.length-fresh.length)+' 条；保留全部已有批注和原件。页码：'+[...new Set(unique.map(item=>item.record.page))].sort((a,b)=>a-b).join('、');if(!el('readingImportDialog').open)el('readingImportDialog').showModal();return clone(preview);
    }
    async function apply(){
        if(!preview||busy)return;busy=true;el('readingImportApply').disabled=true;try{
            const current=ReaderWorkspace.debug();if(current?.id!==preview.source.id||current.file.id!==preview.source.file.id||current.fingerprint!==preview.source.fingerprint)throw new Error('原件已切换，请退回后重新预览');
            const fresh=[];for(const item of preview.additions){const existing=state[item.collection].find(record=>record.id===item.record.id);if(existing&&!same(existing,item.record))throw new Error('同编号批注后来已修改，没有覆盖；请先保留两份开放批注文件再比较');if(!existing)fresh.push(item);}
            for(const item of fresh)state[item.collection].push(clone(item.record));await W.persist();preview=null;await ReaderWorkspace.render();el('readingImportDialog').close();PrepLocal.toast(PrepLocal.hasPending()?'批注已保留在本机，等待同步':'批注已归档');
        }finally{busy=false;el('readingImportApply').disabled=false;}
    }
    function setup(){
        const host=el('readerExportJSON')?.parentElement;if(!host)return;const input=document.createElement('input');input.type='file';input.accept='.json,application/json';input.hidden=true;host.append(input,button('导入开放批注','file-import',()=>input.click()));input.onchange=()=>{const file=input.files[0];input.value='';if(file)prepare(file).catch(error=>PrepLocal.toast(error.message));};
        const dialog=W.dialog('readingImportDialog','核对开放批注','<p id="readingImportSummary"></p><div class="dialog-actions"><button id="readingImportBack" class="btn btn-ghost">退回</button><button id="readingImportApply" class="btn btn-primary">继续并添加批注</button></div>');el('readingImportBack').onclick=()=>{if(!busy){preview=null;dialog.close();}};dialog.querySelector('header button').onclick=el('readingImportBack').onclick;dialog.oncancel=event=>{event.preventDefault();el('readingImportBack').click();};el('readingImportApply').onclick=()=>apply().catch(error=>PrepLocal.toast(error.message));
    }
    addEventListener('workspace-ready',setup,{once:true});window.ReaderImports={prepare,apply,debug:()=>clone(preview)};
})();
