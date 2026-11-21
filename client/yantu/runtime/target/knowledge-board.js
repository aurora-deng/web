(() => {
    const W = Workspace, {el, esc, clone, button, icon} = W;
    const colors = {'1':'#fbe3e1','2':'#f8edc8','3':'#dceee4','4':'#dcecf5','5':'#ede5f3','6':'#e7ecee'};
    let active, graph, observer, loading, generation = 0, renderGeneration = 0, selected = '', history = [], draftTail = Promise.resolve(), saving = false, nodeEditing = false;
    let boardLimit = 40, boardQuery = '', boardSearchTimer, sourceSearchTimer;
    const safeId = value => typeof value === 'string' && /^[A-Za-z0-9_.-]{1,100}$/.test(value);
    const stamp = () => PrepLocal.nowDate().toISOString();
    function zoomStatus(){if(graph&&el('boardZoom'))el('boardZoom').textContent=Math.round(graph.zoom()*100)+'%';}
    function zoomAt(factor,position){
        if(!graph||!active||!Number.isFinite(factor))return;
        const surface=el('boardGraph'),level=Math.max(graph.minZoom(),Math.min(graph.maxZoom(),graph.zoom()*factor));
        graph.zoom({level,renderedPosition:position||{x:surface.clientWidth/2,y:surface.clientHeight/2}});
    }
    function wheelZoom(event){
        if(!event.ctrlKey||!graph||!active)return;
        event.preventDefault();event.stopImmediatePropagation();
        const bounds=el('boardGraph').getBoundingClientRect(),unit=event.deltaMode===1?16:event.deltaMode===2?bounds.height:1;
        const delta=Math.max(-240,Math.min(240,event.deltaY*unit));
        zoomAt(Math.exp(-delta*.004),{x:event.clientX-bounds.left,y:event.clientY-bounds.top});
    }
    function focusNode(id=selected){
        const node=graph?.getElementById(id);if(!node?.length)return;
        graph.zoom(Math.max(graph.zoom(),1.5));graph.center(node);
    }
    function renderSelection(){
        for(const row of el('boardNodes').children)row.setAttribute('aria-pressed',String(row.dataset.nodeId===selected));
        if(el('boardFocus'))el('boardFocus').disabled=!selected;
    }
    function webAddress(value){if(!value)return '';const address=new URL(value);if(!['http:','https:'].includes(address.protocol)||address.username||address.password||value.length>2048||/[\x00-\x20]/.test(value))throw new Error('网页地址需为不含账号密码的 HTTP 或 HTTPS 网址');return address.href;}
    function visibleNodes(){const hidden=new Set();for(const root of active.board.nodes.filter(node=>node.collapsed)){const seen=new Set([root.id]),queue=[root.id];while(queue.length){const id=queue.shift();for(const edge of active.board.edges.filter(edge=>edge.source===id))if(!seen.has(edge.target)){seen.add(edge.target);hidden.add(edge.target);queue.push(edge.target);}}}return active.board.nodes.filter(node=>node.collapsed||!hidden.has(node.id));}

    function validate(value) {
        if (!value || !Array.isArray(value.nodes) || !Array.isArray(value.edges) || value.nodes.length > 500 || value.edges.length > 1000) throw new Error('白板最多 500 个节点和 1000 条连线');
        const ids = new Set();
        for (const node of value.nodes) {
            if (!safeId(node.id) || ids.has(node.id) || typeof node.label !== 'string' || node.label.length > 300 || typeof (node.text || '') !== 'string' || (node.text || '').length > 20000 || ![node.x,node.y].every(n => typeof n === 'number' && Number.isFinite(n) && Math.abs(n) <= 1000000)) throw new Error('白板节点文字、编号或位置无效');
            for (const key of ['recordId','anchorId']) if (node[key] && !safeId(node[key])) throw new Error('白板资料关联编号无效');
            if(node.url)webAddress(node.url);
            if(node.collapsed!==undefined&&typeof node.collapsed!=='boolean')throw new Error('节点折叠状态无效');
            ids.add(node.id);
        }
        const edges = new Set();
        for (const edge of value.edges) {
            if (!safeId(edge.id) || ids.has(edge.id) || edges.has(edge.id) || !ids.has(edge.source) || !ids.has(edge.target) || edge.source === edge.target || typeof (edge.label || '') !== 'string' || (edge.label || '').length > 300) throw new Error('白板连线无效或指向不存在的节点');
            edges.add(edge.id);
        }
        return value;
    }

    async function library() {
        if (window.cytoscape) return window.cytoscape;
        if (!loading) loading = new Promise((resolve,reject) => {
            const script = document.createElement('script');
            script.src = YantuEnvironment.pathname('/vendor/cytoscape-3.34.3/cytoscape.min.js');
            script.onload = () => resolve(window.cytoscape);
            script.onerror = () => { loading = null; reject(new Error('白板组件未缓存，请连接电脑后重试')); };
            document.head.append(script);
        });
        return loading;
    }

    function cacheDraft() {
        if (!active || saving || active.preserveDraft) return draftTail;
        if (JSON.stringify(active.board) === JSON.stringify(active.base)) return draftTail;
        const value = {board:clone(active.board),base:clone(active.base),updatedAt:stamp()}, key = 'board-draft-' + active.board.id;
        draftTail = draftTail.catch(() => {}).then(() => DeviceStore.put(key,value));
        draftTail.catch(error => PrepLocal.toast('白板草稿未能保存：' + error.message));
        el('boardSaveStatus').textContent = '本机草稿 · 尚未归档';
        return draftTail;
    }

    function remember() {
        active.preserveDraft = false;
        history.push(clone(active.board));
        if (history.length > 50) history.shift();
        el('boardUndo').disabled = false;
    }

    function graphElements() {
        const nodes=visibleNodes(),visible=new Set(nodes.map(node=>node.id));
        return [
            ...nodes.map(node => ({data:{id:node.id,label:node.label+(node.collapsed?' · 已折叠':''),color:colors[node.color] || colors['6'],linked:Boolean(node.recordId || node.anchorId || node.url)},position:{x:node.x,y:node.y}})),
            ...active.board.edges.filter(edge=>visible.has(edge.source)&&visible.has(edge.target)).map(edge => ({data:{id:edge.id,source:edge.source,target:edge.target,label:edge.label || ''}}))
        ];
    }

    function nodeOptions() { return active.board.nodes.map(node => `<option value="${esc(node.id)}">${esc(node.label || '未命名节点')}</option>`).join(''); }

    function renderNodes() {
        if (!active) return;
        el('boardTitle').value = active.board.title;
        const list = el('boardNodes'); list.replaceChildren();
        for (const node of active.board.nodes) {
            const row = button(node.label || '未命名节点',node.recordId || node.anchorId ? 'link' : 'note-sticky',() => selectNode(node.id));
            row.dataset.nodeId=node.id;
            row.classList.add('workspace-linked-row'); row.setAttribute('aria-pressed',String(node.id === selected)); list.append(row);
        }
        if (!active.board.nodes.length) list.textContent = '暂无节点';
        for (const id of ['boardEdgeFrom','boardEdgeTo']) { const previous=el(id).value; el(id).innerHTML=nodeOptions(); if(active.board.nodes.some(node=>node.id===previous))el(id).value=previous; }
        const edges = el('boardEdges'); edges.replaceChildren();
        for (const edge of active.board.edges) {
            const row = document.createElement('div'); row.className = 'board-edge-row';
            const names = [edge.source,edge.target].map(id=>active.board.nodes.find(node=>node.id===id)?.label || '节点');
            const label = document.createElement('span'); label.textContent = names.join(' → ') + (edge.label?' · '+edge.label:'');
            row.append(label,icon('删除连线','xmark',()=>{remember();active.board.edges=active.board.edges.filter(item=>item.id!==edge.id);redraw();cacheDraft();})); edges.append(row);
        }
        el('boardUndo').disabled = !history.length;
        renderSelection();
    }

    function redraw(fit=false) {
        if (!graph || !active) return;
        graph.elements().remove(); graph.add(graphElements()); graph.layout({name:'preset'}).run();
        if (selected) graph.getElementById(selected).select();
        if (fit && active.board.nodes.length) graph.fit(undefined,40);
        renderNodes();
    }

    function selectNode(id) {
        if (!active) return;
        selected=id;nodeEditing=false;const node=active.board.nodes.find(item=>item.id===id),form=el('boardNodeForm');
        form.hidden=!node;el('boardSelectionEmpty').hidden=Boolean(node);if(!node)return;
        for (const key of ['label','text','color']) form.elements[key].value=node[key] || (key==='color'?'6':'');
        form.elements.url.value=node.url||'';el('boardOpenWeb').hidden=!node.url;el('boardFoldNode').querySelector('span').textContent=node.collapsed?'展开后续节点':'折叠后续节点';
        form.querySelectorAll('.board-swatches button').forEach(item=>item.setAttribute('aria-pressed',String(item.dataset.color===form.elements.color.value)));
        sourceChoices(node);
        form.elements.recordId.querySelector('[data-missing-source]')?.remove();
        form.elements.recordId.value=node.anchorId||node.recordId||'';
        const missing=(node.anchorId||node.recordId)&&!W.record(node.anchorId||node.recordId);
        if(missing){const option=document.createElement('option');option.value=node.anchorId||node.recordId;option.textContent='关联记录已删除';option.dataset.missingSource='true';form.elements.recordId.append(option);form.elements.recordId.value=option.value;}
        el('boardSourceStatus').textContent=missing?'来源已删除，可从回收站恢复；白板文字仍保留':'';
        el('boardOpenSource').hidden=!(node.anchorId||node.recordId);
        graph?.nodes().unselect();graph?.getElementById(id).select();renderSelection();
    }

    function sourceChoices(node) {
        const select=el('boardNodeForm').elements.recordId,query=el('boardSourceSearch').value.trim().toLocaleLowerCase(),identity=node.anchorId||node.recordId||'';
        if(active.sourceChoicesState!==state||active.sourceChoicesQuery!==query){
            const choices=[],terms=query.split(/\s+/).filter(Boolean);
            for(const {collection,record}of W.allRecords()){
                if(record.id===active.board.id||!['notes','resources','mistakes','papers','mindmaps','knowledgeTopics','sourceAnchors','flashcards'].includes(collection))continue;
                const label=(record.title||record.name||'第 '+record.page+' 页引用')+' · '+W.collectionLabel(collection);
                if(!terms.every(term=>label.toLocaleLowerCase().includes(term)))continue;
                choices.push(`<option value="${esc(record.id)}">${esc(label)}</option>`);if(choices.length===80)break;
            }
            select.innerHTML='<option value="">无关联</option>'+choices.join('');active.sourceChoicesState=state;active.sourceChoicesQuery=query;
        }
        select.querySelector('[data-extra-source]')?.remove();
        if(identity&&![...select.options].some(option=>option.value===identity)){
            const found=W.record(identity);if(found){const option=new Option(found.record.title||found.record.name||'已关联资料',identity);option.dataset.extraSource='true';select.append(option);}
        }
    }

    async function open(id) {
        const request=++generation, base=state.boards.find(board=>board.id===id)||null;
        const board=base?clone(base):{id:id||uid(),title:'新白板',nodes:[],edges:[],knowledgeIds:[]};
        const draft=await DeviceStore.get('board-draft-'+board.id),cy=await library();if(request!==generation)return;
        active={board,base:clone(base),preserveDraft:Boolean(draft)};if(draft&&confirm('发现本机白板草稿，继续编辑？取消则打开已保存版本，草稿仍保留。'))active={board:validate(clone(draft.board)),base:clone(draft.base)};
        el('boardSourceSearch').value='';
        validate(active.board);selected='';history=[];el('boardNodeForm').hidden=true;el('boardSelectionEmpty').hidden=false;el('boardSaveStatus').textContent=draft?'本机草稿已保留':'';
        el('boardDialog').showModal();observer?.disconnect();graph?.destroy();graph=cy({container:el('boardGraph'),elements:graphElements(),layout:{name:'preset'},minZoom:.01,maxZoom:6,wheelSensitivity:.15,boxSelectionEnabled:false,style:[
            {selector:'node',style:{shape:'roundrectangle',width:180,height:72,'background-color':'data(color)',label:'data(label)','font-size':15,'text-wrap':'wrap','text-max-width':154,'text-valign':'center','text-halign':'center',color:'#21383e','border-width':1,'border-color':'#a4b7bc'}},
            {selector:'node:selected',style:{'border-width':3,'border-color':'#167ca2'}},
            {selector:'edge',style:{width:2,'line-color':'#8ca5ab','target-arrow-color':'#8ca5ab','target-arrow-shape':'triangle','curve-style':'bezier',label:'data(label)','font-size':12,'text-background-color':'#ffffff','text-background-opacity':1,'text-background-padding':3,color:'#35545f'}}
        ]});
        graph.on('tap','node',event=>selectNode(event.target.id()));
        graph.on('dbltap','node',event=>{selectNode(event.target.id());focusNode(event.target.id());});
        graph.on('grab','node',remember);graph.on('dragfree','node',event=>{const node=active?.board.nodes.find(item=>item.id===event.target.id());if(node){Object.assign(node,event.target.position());cacheDraft();}});
        graph.on('zoom pan',()=>{if(active){active.board.viewport={zoom:graph.zoom(),pan:graph.pan()};zoomStatus();}});
        let width=el('boardGraph').clientWidth,height=el('boardGraph').clientHeight;
        observer=new ResizeObserver(()=>{const canvas=el('boardGraph'),changed=Math.abs(canvas.clientWidth-width)>2||Math.abs(canvas.clientHeight-height)>2,zoom=graph?.zoom(),pan=graph?.pan(),center=pan?{x:(width/2-pan.x)/zoom,y:(height/2-pan.y)/zoom}:null;width=canvas.clientWidth;height=canvas.clientHeight;if(changed&&graph&&active){graph.resize();if(center)graph.pan({x:width/2-center.x*zoom,y:height/2-center.y*zoom});}});observer.observe(el('boardGraph'));
        renderNodes();requestAnimationFrame(()=>{if(request!==generation)return;graph.resize();const view=active.board.viewport;if(view&&Number.isFinite(view.zoom)&&Number.isFinite(view.pan?.x)&&Number.isFinite(view.pan?.y)){graph.zoom(view.zoom);graph.pan(view.pan);}else if(board.nodes.length)graph.fit(undefined,40);zoomStatus();});
    }

    async function save() {
        if(!active||saving)return;const title=el('boardTitle').value.trim();if(!title){el('boardTitle').reportValidity();return;}
        const latest=state.boards.find(board=>board.id===active.board.id);if(JSON.stringify(latest||null)!==JSON.stringify(active.base))throw new Error('白板已在其他位置修改，请导出当前白板，退回重新打开后比较；未覆盖资料');
        active.board.title=title;validate(active.board);saving=true;
        try{await draftTail.catch(()=>{});const board={...clone(active.board),updatedAt:stamp()};await W.put('boards',board);active.base=clone(board);active.board=board;await DeviceStore.put('board-draft-'+board.id,undefined);el('boardSaveStatus').textContent=PrepLocal.hasPending()?'已保存到本机，等待同步':'已同步到电脑';render();}finally{saving=false;}
    }

    function addNode() {
        if(!active)return;if(active.board.nodes.length>=500)throw new Error('单张白板最多 500 个节点');remember();
        const center={x:(el('boardGraph').clientWidth/2-graph.pan().x)/graph.zoom(),y:(el('boardGraph').clientHeight/2-graph.pan().y)/graph.zoom()},node={id:uid(),label:'新节点',text:'',x:center.x,y:center.y,color:'6'};
        active.board.nodes.push(node);redraw();selectNode(node.id);el('boardNodeForm').elements.label.focus();cacheDraft();
    }

    function applyNode(form) {
        const node=active?.board.nodes.find(item=>item.id===selected);if(!node)return;
        if(!nodeEditing){remember();nodeEditing=true;}const source=W.record(form.elements.recordId.value);
        Object.assign(node,{label:form.elements.label.value.trim(),text:form.elements.text.value,color:form.elements.color.value,url:form.elements.url.value.trim()});delete node.recordId;delete node.anchorId;
        if(form.elements.recordId.value)node[source?.collection==='sourceAnchors'?'anchorId':'recordId']=form.elements.recordId.value;
        cacheDraft();
    }

    function updateNode(event) {
        event.preventDefault();applyNode(event.target);const node=active?.board.nodes.find(item=>item.id===selected);if(!node)return;
        redraw();selectNode(node.id);cacheDraft();
    }

    function exportCanvas() {
        if(!active)return;const board=active.board;
        const value={nodes:board.nodes.map(node=>({id:node.id,type:node.url?'link':'text',...(node.url?{url:node.url}:{}),x:node.x-90,y:node.y-36,width:180,height:72,color:node.color||'6',text:node.label+(node.text?'\n\n'+node.text:''),yantu:{recordId:node.recordId||'',anchorId:node.anchorId||'',label:node.label,text:node.text||'',url:node.url||'',collapsed:Boolean(node.collapsed)}})),edges:board.edges.map(edge=>({id:edge.id,fromNode:edge.source,toNode:edge.target,fromSide:'right',toSide:'left',label:edge.label||''})),yantu:{format:1,title:el('boardTitle').value,knowledgeIds:board.knowledgeIds||[]}};
        downloadBlob((board.title||'研途白板')+'.canvas',new Blob([JSON.stringify(value,null,2)],{type:'application/json'}));return value;
    }

    function fromCanvas(value) {
        if(!value||!Array.isArray(value.nodes)||!Array.isArray(value.edges))throw new Error('不是有效的 JSON Canvas 白板');
        const nodes=value.nodes.map(node=>{if(!['text','file','link','group'].includes(node.type))throw new Error('白板包含不支持的节点类型');const text=node.text||node.file||node.url||node.label||'',own=node.yantu,url=own?.url||node.url||'';return {id:node.id,label:own?.label||String(text).split('\n')[0].slice(0,300)||'节点',text:typeof own?.text==='string'?own.text:String(text),x:Number(node.x)+Number(node.width||180)/2,y:Number(node.y)+Number(node.height||72)/2,color:String(node.color||'6'),...(url?{url:webAddress(url)}:{}),...(own?.collapsed?{collapsed:true}:{}),...(own?.recordId?{recordId:own.recordId}:{}),...(own?.anchorId?{anchorId:own.anchorId}:{})};});
        const edges=value.edges.map(edge=>({id:edge.id,source:edge.fromNode,target:edge.toNode,label:edge.label||''}));
        return validate({id:uid(),title:String(value.yantu?.title||'导入白板').slice(0,300),nodes,edges,knowledgeIds:[]});
    }

    async function importFile(file) {
        if(!file)return;if(file.size>2*1024**2)throw new Error('白板文件超过 2 MB，请拆分后导入');
        const parsed=JSON.parse(await file.text(),(key,value)=>{if(['__proto__','prototype','constructor'].includes(key))throw new Error('白板包含不安全字段');return value;}),board=fromCanvas(parsed);
        if(!confirm(`将 ${board.nodes.length} 个节点、${board.edges.length} 条连线作为新白板草稿打开？不会覆盖已有白板，也不会自动打开外部网址或本地路径。`))return;
        await open(board.id);if(active?.board.id!==board.id)return;active.board=board;redraw(true);cacheDraft();
    }

    function render() {
        const request=++renderGeneration,list=el('boardList'),query=el('boardSearch').value.toLocaleLowerCase();list.replaceChildren();
        if(query!==boardQuery){boardLimit=40;boardQuery=query;}
        const matches=state.boards.filter(item=>!query||[item.title,...(item.nodes||[]).map(node=>node.label)].join(' ').toLocaleLowerCase().includes(query));
        for(const board of matches.slice(0,boardLimit)){const wrapper=document.createElement('div');wrapper.className='board-list-row';const row=button(board.title||'白板','diagram-project',()=>open(board.id));row.classList.add('workspace-linked-row');const small=document.createElement('small');small.textContent=`${board.nodes.length} 个节点 · ${board.edges.length} 条连线`;row.append(small);const menu=document.createElement('details'),summary=document.createElement('summary');summary.textContent='更多';menu.append(summary,button('删除白板','trash',async()=>{if(await W.remove('boards',board.id)){await DeviceStore.put('board-draft-'+board.id,undefined);render();}},'btn-danger'));wrapper.append(row,menu);list.append(wrapper);}
        if(matches.length>boardLimit)list.append(button('继续显示 · 余下 '+(matches.length-boardLimit)+' 张','chevron-down',()=>{boardLimit+=40;render();}));
        if(!list.children.length)list.textContent='暂无匹配白板';
        DeviceStore.entries('board-draft-').then(entries=>{if(request!==renderGeneration)return;for(const[key,draft]of entries){const board=draft?.board;if(!board||state.boards.some(item=>item.id===board.id)||!String(board.title||'新白板').toLocaleLowerCase().includes(query))continue;if(!list.children.length)list.replaceChildren();const wrapper=document.createElement('div');wrapper.className='board-list-row';wrapper.append(button((board.title||'新白板')+' · 本机草稿','pen',()=>open(board.id)),icon('放弃本机白板草稿','trash',async()=>{if(!confirm('放弃这份尚未归档的本机草稿？'))return;await DeviceStore.put(key,undefined);render();}));list.append(wrapper);}}).catch(error=>PrepLocal.toast(error.message));
    }

    function setup() {
        const section=W.view('boards','关联白板','diagram-project',render);
        section.querySelector('.workspace-actions').append(button('新建白板','plus',()=>open(),'btn-primary'),button('导入白板','file-import',()=>el('boardImport').click()),button('已上传导图','image',()=>PrepPush.openView('mindmaps')));
        section.querySelector('.workspace-content').innerHTML='<input id="boardSearch" type="search" aria-label="搜索白板" placeholder="搜索白板与节点" /><input id="boardImport" type="file" accept=".canvas,.json" hidden /><div id="boardList" class="workspace-list"></div>';
        const dialog=W.dialog('boardDialog','关联白板','<div class="board-toolbar"><input id="boardTitle" aria-label="白板标题" maxlength="300" required /><div id="boardTools" class="workspace-actions"></div><span id="boardSaveStatus" role="status"></span></div><div class="board-layout"><div id="boardGraph" role="img" aria-label="关联白板图"></div><aside id="boardInspector"><details open><summary>节点</summary><div id="boardNodes" class="workspace-list"></div><p id="boardSelectionEmpty">尚未选择节点</p><form id="boardNodeForm" hidden><label>名称<input name="label" required maxlength="300" /></label><label>内容<textarea name="text" rows="5" maxlength="20000"></textarea></label><label>关联资料<select name="recordId"></select></label><fieldset class="board-swatches"><legend>颜色</legend></fieldset><input name="color" type="hidden" value="6" /><p id="boardSourceStatus"></p><div class="workspace-actions"><button type="submit" class="btn btn-primary">应用节点</button><button id="boardOpenSource" type="button" class="btn btn-ghost">打开来源</button><button id="boardRemoveNode" type="button" class="btn btn-icon btn-danger" aria-label="删除节点" title="删除节点"><i class="fas fa-trash"></i></button></div><div id="boardMove" class="workspace-actions" role="group" aria-label="移动选中节点"></div></form></details><details><summary>连线</summary><form id="boardEdgeForm"><label>起点<select id="boardEdgeFrom"></select></label><label>终点<select id="boardEdgeTo"></select></label><label>关系<input name="label" maxlength="300" /></label><button type="submit" class="btn btn-ghost">添加连线</button></form><div id="boardEdges"></div></details></aside></div>');
        dialog.classList.add('board-workspace');
        el('boardGraph').addEventListener('wheel',wheelZoom,{passive:false,capture:true});
        el('boardGraph').title='Ctrl + 鼠标滚轮缩放；双击节点定位';
        addEventListener('workspace-change',()=>{if(active)active.sourceChoicesState=null;});
        const sourceLabel=document.createElement('label');sourceLabel.textContent='查找关联资料';const sourceSearch=document.createElement('input');sourceSearch.type='search';sourceSearch.id='boardSourceSearch';sourceSearch.placeholder='搜索名称';sourceLabel.append(sourceSearch);el('boardNodeForm').elements.recordId.closest('label').before(sourceLabel);
        sourceSearch.oninput=event=>{event.stopPropagation();clearTimeout(sourceSearchTimer);sourceSearchTimer=setTimeout(()=>{if(active&&selected)selectNode(selected);},120);};
        const address=document.createElement('label');address.textContent='关联网页';const input=document.createElement('input');input.type='url';input.name='url';input.maxLength=2048;address.append(input);el('boardNodeForm').querySelector('label:nth-child(2)').after(address);
        const web=button('打开网页','arrow-up-right-from-square',()=>{const node=active?.board.nodes.find(node=>node.id===selected);if(!node?.url)return;const url=webAddress(node.url);if(confirm('打开 '+new URL(url).hostname+' 的网页？'))window.open(url,'_blank','noopener,noreferrer');});web.id='boardOpenWeb';web.hidden=true;
        const fold=button('折叠后续节点','chevrons-right',()=>{const node=active?.board.nodes.find(node=>node.id===selected);if(!node)return;remember();node.collapsed=!node.collapsed;redraw(true);selectNode(node.id);cacheDraft();});fold.id='boardFoldNode';el('boardNodeForm').querySelector('.workspace-actions').append(web,fold);
        el('boardTools').append(icon('展开全部节点','chevrons-down',()=>{if(!active)return;remember();active.board.nodes.forEach(node=>node.collapsed=false);redraw(true);cacheDraft();}));
        const zoomValue=document.createElement('output');zoomValue.id='boardZoom';zoomValue.setAttribute('aria-label','白板缩放比例');zoomValue.textContent='100%';
        const focus=icon('定位选中知识点','crosshairs',()=>focusNode());focus.id='boardFocus';focus.disabled=true;
        el('boardTools').append(icon('缩小','magnifying-glass-minus',()=>zoomAt(1/1.3)),zoomValue,icon('放大','magnifying-glass-plus',()=>zoomAt(1.3)),focus);
        const undo=icon('撤销本机修改','rotate-left',()=>{if(!history.length)return;active.board=history.pop();selected='';el('boardNodeForm').hidden=true;redraw();cacheDraft();});undo.id='boardUndo';
        el('boardTools').append(icon('添加节点','plus',addNode),undo,icon('适合窗口','expand',()=>graph?.fit(undefined,40)),icon('整理布局','sitemap',()=>{if(!active?.board.nodes.length)return;remember();graph.layout({name:'breadthfirst',directed:true,animate:false,padding:40}).run();for(const node of active.board.nodes)Object.assign(node,graph.getElementById(node.id).position());cacheDraft();}),icon('全屏','maximize',()=>{dialog.classList.toggle('is-fullscreen');graph?.resize();}),icon('导出白板文件','file-export',exportCanvas),icon('导出图片','image',()=>{if(graph&&active?.board.nodes.length){const image=graph.png({output:'blob',bg:'#ffffff',scale:2,full:true,maxWidth:4000,maxHeight:4000});downloadBlob(active.board.title+'.png',image);}}),button('保存白板','check',save,'btn-primary'));
        const swatches=el('boardNodeForm').querySelector('.board-swatches');for(const[key,color]of Object.entries(colors)){const swatch=icon({'1':'红色','2':'黄色','3':'绿色','4':'蓝色','5':'淡紫色','6':'灰色'}[key],'circle',()=>{el('boardNodeForm').elements.color.value=key;swatches.querySelectorAll('button').forEach(item=>item.setAttribute('aria-pressed',String(item===swatch)));applyNode(el('boardNodeForm'));});swatch.dataset.color=key;swatch.style.color=color;swatch.style.backgroundColor=color;swatch.setAttribute('aria-pressed','false');swatches.append(swatch);}
        for(const[name,dx,dy,arrow]of[['向左',-20,0,'arrow-left'],['向右',20,0,'arrow-right'],['向上',0,-20,'arrow-up'],['向下',0,20,'arrow-down']])el('boardMove').append(icon(name,arrow,()=>{const node=active?.board.nodes.find(node=>node.id===selected);if(!node)return;remember();node.x+=dx;node.y+=dy;redraw();cacheDraft();}));
        el('boardTitle').oninput=()=>{if(active){active.preserveDraft=false;active.board.title=el('boardTitle').value;cacheDraft();}};el('boardSearch').oninput=()=>{clearTimeout(boardSearchTimer);boardSearchTimer=setTimeout(render,120);};el('boardNodeForm').onsubmit=updateNode;el('boardNodeForm').oninput=event=>applyNode(event.currentTarget);
        el('boardRemoveNode').onclick=()=>{if(!selected||!confirm('删除这个节点及相连线？关联的学习资料不删除。'))return;remember();active.board.nodes=active.board.nodes.filter(node=>node.id!==selected);active.board.edges=active.board.edges.filter(edge=>edge.source!==selected&&edge.target!==selected);selected='';el('boardNodeForm').hidden=true;el('boardSelectionEmpty').hidden=false;redraw();cacheDraft();};
        el('boardOpenSource').onclick=async()=>{const node=active?.board.nodes.find(node=>node.id===selected);if(!node)return;await cacheDraft();dialog.close();if(node.anchorId)await ReaderWorkspace.openAnchor(node.anchorId);else W.openRecord(node.recordId);};
        el('boardEdgeForm').onsubmit=event=>{event.preventDefault();if(!active)return;const source=el('boardEdgeFrom').value,target=el('boardEdgeTo').value;if(!source||!target||source===target){PrepLocal.toast('请选择两个不同节点');return;}if(active.board.edges.length>=1000){PrepLocal.toast('单张白板最多 1000 条连线');return;}remember();active.board.edges.push({id:uid(),source,target,label:event.target.elements.label.value});redraw();cacheDraft();};
        el('boardImport').onchange=event=>{const file=event.target.files[0];event.target.value='';importFile(file).catch(error=>PrepLocal.toast(error.message));};
        const close=dialog.close.bind(dialog);dialog.close=()=>{if(saving)return;cacheDraft();generation++;observer?.disconnect();graph?.destroy();graph=null;active=null;selected='';close();};dialog.oncancel=event=>{event.preventDefault();dialog.close();};
        addEventListener('workspace-open-record',event=>{if(event.detail.collection==='boards'){W.navigate('boards');open(event.detail.record.id).catch(error=>PrepLocal.toast(error.message));}});
        addEventListener('study-detail',event=>el('studyConnections').append(button('关联到新白板','diagram-project',async()=>{const source=W.record(event.detail.id)?.record;if(!source)return;await open();const node={id:uid(),label:source.title||source.name||'学习资料',text:'',recordId:source.id,x:160,y:100,color:'4'};active.board.nodes.push(node);redraw(true);selectNode(node.id);cacheDraft();})));
    }
    addEventListener('workspace-ready',setup,{once:true});window.KnowledgeBoard={open,save,addNode,exportCanvas,fromCanvas,importFile,validate,debug:()=>active?{board:clone(active.board),graph}:null};
})();
