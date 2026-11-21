(() => {
    const configs = {mistakes:['mistakeList','mistakeFilter','错题'],notes:['noteList','noteFilter','笔记'],resources:['resourceList','resourceFilter','资料'],mindmaps:['mindmapList','mindmapFilter','导图'],papers:['paperList','paperFilter','试卷']};
    const el = id => document.getElementById(id);
    const esc = value => String(value ?? '').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const tags = record => Array.isArray(record.tags) ? record.tags.map(String) : String(record.tags || '').split(/[,，;；]/).map(value=>value.trim()).filter(Boolean);
    const files = record => [...(record.files || []),...(record.fileId && !(record.files || []).some(file=>file.id===record.fileId) ? [{id:record.fileId,name:record.fileName || `${record.title || '试卷'}.pdf`,type:'application/pdf'}] : [])];
    const parts = record => String(record.path || record.subject || '未分类').split(/\s*(?:->|>|\/|\\|→)\s*/).filter(Boolean);
    const pathKey = record => parts(record).join(' > ');
    const mastery = record => ['new','review','mastered'].includes(record.mastery) ? record.mastery : 'new';
    const label = record => ({new:'待复习',review:'仍需练习',mastered:'已掌握'}[mastery(record)]);
    const today = () => PrepLocal.dateText(PrepLocal.nowDate());
    const due = (record,day) => mastery(record)!=='mastered' && (!record.nextReview || record.nextReview<=(day||today()));
    const order = ResourceOrder, compareTitle = order.compare;
    const pageSizes=[24,48,96];
    let preferences = {}, resourceTab = 'notes', detail = null, searchTimer;
    try { preferences=JSON.parse(YantuEnvironment.storage.getItem('yantu-library-views-v1') || '{}'); resourceTab=preferences.resourceTab==='resources'?'resources':'notes'; } catch (_) {}
    const views = new Map();
    function view(col) {
        if (!views.has(col)) {
            const settings={...preferences[col]};
            if(!preferences.orderVersion&&settings.sort==='recent')settings.sort=col==='papers'?'year-desc':'used';
            views.set(col,{q:'',folder:'',subject:'all',status:'',tag:'',type:'',role:'',year:'',month:'',favorite:false,sort:col==='papers'?'year-desc':'used',group:col==='papers'?'year':col==='resources'?'kind':'',layout:'gallery',...settings,pageSize:pageSizes.includes(Number(settings.pageSize))?Number(settings.pageSize):24,page:1,selected:new Set(),results:[]});
        }
        return views.get(col);
    }
    function remember() {
        const saved={resourceTab,orderVersion:2};
        for(const [col,ui] of views){const {selected,results,page,metadata,...settings}=ui;saved[col]=settings;}
        try {YantuEnvironment.storage.setItem('yantu-library-views-v1',JSON.stringify(saved));} catch (_) {}
    }
    function options(values, selected, allLabel) { return `<option value="">${allLabel}</option>` + values.map(value=>`<option value="${esc(value)}" ${value===selected?'selected':''}>${esc(value)}</option>`).join('')+(selected&&!values.includes(selected)?`<option value="${esc(selected)}" selected>${esc(selected)} (0)</option>`:''); }
    function pageControls(){return `<div class="library-page-controls"><button class="btn btn-icon btn-ghost" data-action="first" title="首页" aria-label="首页"><i class="fas fa-angles-left"></i></button><button class="btn btn-icon btn-ghost" data-action="previous" title="上一页" aria-label="上一页"><i class="fas fa-chevron-left"></i></button><select data-page-jump aria-label="跳转页码"></select><button class="btn btn-icon btn-ghost" data-action="next" title="下一页" aria-label="下一页"><i class="fas fa-chevron-right"></i></button><button class="btn btn-icon btn-ghost" data-action="last" title="末页" aria-label="末页"><i class="fas fa-angles-right"></i></button></div><select data-page-size aria-label="每页显示条数">${pageSizes.map(size=>`<option value="${size}">每页 ${size} 条</option>`).join('')}</select>`;}
    function mount(col) {
        const list=el(configs[col][0]);
        let root=list.closest('.library-shell');
        if(root)return root;
        root=document.createElement('div');root.className='library-shell';root.dataset.col=col;
        root.innerHTML=`<div class="library-toolbar">
            <label class="library-search"><i class="fas fa-search" aria-hidden="true"></i><input type="search" data-field="q" placeholder="搜索" aria-label="搜索${configs[col][2]}" title="搜索标题、内容、标签和附件名称" /></label>
            <select data-field="sort" aria-label="排序"><option value="used">最近使用</option><option value="year-desc">资料年份：从新到旧</option><option value="year-asc">资料年份：从旧到新</option><option value="recent">记录日期：从新到旧</option><option value="oldest">记录日期：从旧到新</option><option value="kind">资料类型</option><option value="title">名称与编号</option></select>
            <select data-field="group" aria-label="分组"><option value="">不分组</option><option value="kind">按资料类型分组</option><option value="subject">按科目分组</option><option value="year">按资料年份分组</option><option value="month">按记录月份分组</option><option value="format">按文件格式分组</option></select>
            <div class="library-layouts" role="group" aria-label="显示方式"><button class="btn btn-icon btn-ghost" data-action="layout" data-layout="gallery" title="卡片视图" aria-label="卡片视图"><i class="fas fa-table-cells-large"></i></button><button class="btn btn-icon btn-ghost" data-action="layout" data-layout="table" title="列表视图" aria-label="列表视图"><i class="fas fa-list"></i></button></div></div>
            <div class="library-facets"><select data-field="role" aria-label="资料类型"></select><select data-field="type" aria-label="文件格式"></select><select data-field="year" aria-label="资料年份"></select><select data-field="month" aria-label="记录月份"></select><select data-field="tag" aria-label="标签"></select>
            ${col==='mistakes'?'<select data-field="status" aria-label="掌握情况"><option value="">所有错题</option><option value="due">待复习</option><option value="new">未标记</option><option value="review">仍需练习</option><option value="mastered">已掌握</option></select><button class="btn btn-ghost btn-sm" data-action="review"><i class="fas fa-layer-group"></i> 复习这一组</button>':''}
            ${col==='papers'?'<select data-field="status" aria-label="试卷状态"><option value="">所有状态</option><option value="todo">未开始</option><option value="doing">进行中</option><option value="done">已完成</option></select>':''}
            <label class="library-favorite-filter"><input type="checkbox" data-field="favorite" /><i class="fas fa-star" aria-hidden="true"></i> 收藏</label><button class="btn btn-icon btn-ghost" data-action="reset" title="清除筛选" aria-label="清除筛选"><i class="fas fa-filter-circle-xmark"></i></button></div>
            <div class="library-summary" role="status"></div>
            <nav class="library-page-top" aria-label="顶部翻页"><span class="library-page-range"></span>${pageControls()}</nav>
            <div class="library-bulk" hidden><span></span><button class="btn btn-ghost btn-sm" data-action="move"><i class="fas fa-folder-open"></i> 移动</button><button class="btn btn-ghost btn-sm" data-action="classify"><i class="fas fa-layer-group"></i> 资料类型</button><button class="btn btn-ghost btn-sm" data-action="tags"><i class="fas fa-tags"></i> 标签</button><button class="btn btn-ghost btn-sm" data-action="favorite"><i class="fas fa-star"></i> 收藏</button><button class="btn btn-danger btn-sm" data-action="delete"><i class="fas fa-trash"></i> 删除</button><button class="btn btn-icon btn-ghost" data-action="clear" title="取消选择" aria-label="取消选择"><i class="fas fa-xmark"></i></button></div>
            <div class="library-body"><aside class="library-folders" aria-label="分类目录"></aside><div class="library-results"></div></div><footer class="library-pager" aria-label="底部翻页"><span class="library-page-range"></span>${pageControls()}<button class="btn btn-ghost btn-sm" data-action="select-page">选择本页</button></footer>`;
        list.before(root);const results=root.querySelector('.library-results');results.append(el(configs[col][1]),list);
        const folderSelect=document.createElement('select');folderSelect.className='library-mobile-folders';folderSelect.dataset.field='folder';folderSelect.setAttribute('aria-label','分类目录');root.querySelector('.library-facets').prepend(folderSelect);
        root.addEventListener('input',event=>{
            if(event.target.dataset.field!=='q')return;
            const ui=view(col);ui.q=event.target.value;ui.page=1;
            clearTimeout(searchTimer);searchTimer=setTimeout(()=>{remember();refreshRecords(col);},150);
        });
        root.addEventListener('change',event=>{
            if(event.target.hasAttribute('data-page-jump')){const ui=view(col);ui.page=Math.max(1,Math.min(Math.ceil(ui.results.length/ui.pageSize)||1,Number(event.target.value)||1));refreshRecords(col);return;}
            if(event.target.hasAttribute('data-page-size')){const ui=view(col),size=Number(event.target.value);if(!pageSizes.includes(size))return;const first=(ui.page-1)*ui.pageSize;ui.pageSize=size;ui.page=Math.floor(first/size)+1;remember();refreshRecords(col);return;}
            const field=event.target.dataset.field;
            if(field && field!=='q'){view(col)[field]=event.target.type==='checkbox'?event.target.checked:event.target.value;if(field==='folder')view(col).subject='all';if(field==='sort'&&event.target.value==='used')view(col).group='';view(col).page=1;remember();refreshRecords(col);}
            const id=event.target.dataset.recordSelect;
            if(id){event.target.checked?view(col).selected.add(id):view(col).selected.delete(id);renderBulk(col);}
        });
        root.addEventListener('click',event=>{
            const button=event.target.closest('[data-action]');if(!button)return;
            const ui=view(col),action=button.dataset.action;
            if(action==='folder'){ui.folder=button.dataset.folder;ui.subject='all';ui.page=1;}
            else if(action==='layout')ui.layout=button.dataset.layout;
            else if(action==='reset')Object.assign(ui,{q:'',folder:'',subject:'all',status:'',tag:'',type:'',role:'',year:'',month:'',favorite:false,page:1});
            else if(action==='previous')ui.page=Math.max(1,ui.page-1);
            else if(action==='next')ui.page++;
            else if(action==='first')ui.page=1;
            else if(action==='last')ui.page=Math.max(1,Math.ceil(ui.results.length/ui.pageSize));
            else if(action==='select-page')ui.results.slice((ui.page-1)*ui.pageSize,ui.page*ui.pageSize).forEach(record=>ui.selected.add(record.id));
            else if(action==='clear')ui.selected.clear();
            else if(['move','tags','classify'].includes(action)){bulkDialog(col,action);return;}
            else if(action==='favorite'){for(const record of state[col])if(ui.selected.has(record.id))record.favorite=true;persist(col);return;}
            else if(action==='delete'){removeSelected(col);return;}
            else if(action==='review'){review(col);return;}
            remember();refreshRecords(col);
        });
        return root;
    }
    function filter(col, records) {
        const ui=view(col), terms=ui.q.trim().toLocaleLowerCase().split(/\s+/).filter(Boolean), day=today(),prepared=order.prepare(records,col,ui,state.readingPositions);
        ui.metadata=prepared.metadata;
        return records.filter(record=>{
            const recordFiles=files(record), folder=pathKey(record),meta=ui.metadata.get(record);
            const text=terms.length?[record.title,record.path,record.subject,record.content,record.remark,record.cause,record.solution,record.questionNumber,...(record.knowledgePoints||[]),...tags(record),...recordFiles.map(file=>file.name)].join(' ').toLocaleLowerCase():'';
            return terms.every(term=>text.includes(term)) && (!ui.folder||folder===ui.folder||folder.startsWith(ui.folder+' > ')) && (ui.subject==='all'||extractSubject(record.path||record.subject)===ui.subject) && (!ui.favorite||record.favorite===true) && (!ui.tag||tags(record).includes(ui.tag)) && (!ui.role||meta.kind===ui.role) && (!ui.month||meta.month===ui.month) && (!ui.year||(ui.year==='$unknown'?!meta.years.length:meta.years.includes(Number(ui.year)))) && (!ui.type||meta.formats.includes(ui.type)) && (!ui.status||(col==='mistakes'?(ui.status==='due'?due(record,day):mastery(record)===ui.status):(record.practiceStatus||'todo')===ui.status));
        }).sort(prepared.comparator);
    }
    function renderFolders(col,root) {
        const counts=new Map();
        for(const record of state[col]||[]){const segments=parts(record);for(let i=1;i<=segments.length;i++){const key=segments.slice(0,i).join(' > ');counts.set(key,(counts.get(key)||0)+1);}}
        const keys=[...counts.keys()].sort(compareTitle),signature=JSON.stringify([view(col).folder,(state[col]||[]).length,keys.map(key=>[key,counts.get(key)])]);
        if(root._folderSignature===signature)return;root._folderSignature=signature;
        root.querySelector('.library-folders').innerHTML=`<button data-action="folder" data-folder="" class="library-folder ${!view(col).folder?'active':''}"><i class="fas fa-folder-open"></i><span>全部分类</span><small>${(state[col]||[]).length}</small></button>`+keys.map(key=>`<button data-action="folder" data-folder="${esc(key)}" class="library-folder ${view(col).folder===key?'active':''}" style="--folder-depth:${Math.min(4,key.split(' > ').length-1)}" title="${esc(key)}"><i class="fas fa-folder"></i><span>${esc(key.split(' > ').at(-1))}</span><small>${counts.get(key)}</small></button>`).join('');
        root.querySelector('.library-mobile-folders').innerHTML=options(keys,view(col).folder,'全部分类');
    }
    function facet(input,counts,selected,label,labels={},keys=[...counts.keys()].sort(compareTitle)) {
        const html=`<option value="">${label}</option>`+keys.map(key=>`<option value="${esc(key)}">${esc(labels[key]||key)} (${counts.get(key)})</option>`).join('')+(selected&&!counts.has(selected)?`<option value="${esc(selected)}">${esc(labels[selected]||selected)} (0)</option>`:'');
        if(input._choices!==html){input.innerHTML=html;input._choices=html;}
        input.value=selected;
    }
    function facets(records,ui) {
        const counts={role:new Map(),type:new Map(),year:new Map(),month:new Map(),tag:new Map()},add=(field,value)=>counts[field].set(value,(counts[field].get(value)||0)+1);
        for(const record of records){const meta=ui.metadata.get(record);add('role',meta.kind);for(const format of meta.formats)add('type',format);for(const year of meta.years.length?meta.years.map(String):['$unknown'])add('year',year);if(meta.month)add('month',meta.month);for(const tag of new Set(tags(record)))add('tag',tag);}
        return counts;
    }
    function groupedPage(records,col,render,tableMode=false) {
        const ui=view(col);if(!ui.group)return render(records);
        const groups=[],counts=new Map();
        for(const record of ui.results){const key=order.group(record,ui.metadata.get(record),ui.group).key;counts.set(key,(counts.get(key)||0)+1);}
        for(const record of records){const group=order.group(record,ui.metadata.get(record),ui.group);if(groups.at(-1)?.key!==group.key)groups.push({...group,records:[]});groups.at(-1).records.push(record);}
        const previous=ui.results[(ui.page-1)*ui.pageSize-1];
        return groups.map((group,index)=>{
            const continued=index===0&&previous&&order.group(previous,ui.metadata.get(previous),ui.group).key===group.key;
            const title=`${esc(group.label)} <span>${counts.get(group.key)} 条${continued?' · 续':''}</span>`;
            return tableMode?`<tr class="library-group-row"><th colspan="6">${title}</th></tr>${render(group.records)}`:`<h4 class="library-group-heading">${title}</h4>${render(group.records)}`;
        }).join('');
    }
    const usedDate = new Intl.DateTimeFormat('zh-CN',{month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit'});
    function attributes(record,col) {
        const meta=view(col).metadata?.get(record)||order.describe(record,col);
        return `<span class="library-kind">${esc(order.kinds[meta.kind])}</span>${meta.yearLabel?`<span>${esc(meta.yearLabel)}${meta.years.length>1?' 合集':''}</span>`:''}${meta.used?`<time title="最近使用">${esc(usedDate.format(meta.used))}</time>`:''}`;
    }
    const usageDirty=new Set();let usageSave;
    function saveUsage(){if(!usageSave)return;clearTimeout(usageSave);usageSave=null;saveState();}
    function touch(id) {
        const {record,col}=findRecordWithCol(id);if(!record||!configs[col])return;
        const now=PrepLocal.nowDate();if(+now-order.timestamp(record.lastUsedAt)<60000)return;
        record.lastUsedAt=now.toISOString();usageDirty.add(col);
        if(!usageSave)usageSave=setTimeout(saveUsage,250);
    }
    const previousCard=renderRecordCard;
    renderRecordCard=(record,subject)=>{
        const template=document.createElement('template');template.innerHTML=previousCard(record,subject);
        const article=template.content.firstElementChild, col=findRecordWithCol(record.id).col;
        if(!configs[col])return article.outerHTML;
        const checkbox=document.createElement('input');checkbox.type='checkbox';checkbox.className='library-select';checkbox.dataset.recordSelect=record.id;checkbox.checked=view(col).selected.has(record.id);checkbox.setAttribute('aria-label','选择 '+(record.title||'记录'));article.prepend(checkbox);
        const heading=article.querySelector('h4'),badge=heading.querySelector('.badge'),title=document.createElement('button');title.type='button';title.className='library-record-title';title.textContent=record.title||`${record.year||''}年${record.subject||''}试卷`;title.setAttribute('onclick',`StudyLibrary.open(${JSON.stringify(record.id)})`);heading.replaceChildren(badge,title);
        const star=document.createElement('button');star.className='btn btn-icon btn-ghost library-star';star.title=record.favorite?'取消收藏':'收藏';star.setAttribute('aria-label',star.title);star.setAttribute('aria-pressed',String(Boolean(record.favorite)));star.setAttribute('onclick',`StudyLibrary.favorite(${JSON.stringify(record.id)})`);star.innerHTML=`<i class="${record.favorite?'fas':'far'} fa-star"></i>`;article.querySelector('.record-heading .record-toolbar').prepend(star);
        const meta=document.createElement('div');meta.className='library-record-metadata';
        meta.innerHTML=attributes(record,col)+(col==='mistakes'?`<span class="mastery-${mastery(record)}">${label(record)}</span>${record.nextReview?`<time>${esc(record.nextReview)}</time>`:''}`:col==='papers'?`<span>${{todo:'未开始',doing:'进行中',done:'已完成'}[record.practiceStatus||'todo']||'未开始'}</span><span>${(state.scores||[]).filter(score=>score.paperId===record.id).length} 次成绩</span>`:'')+tags(record).filter(tag=>tag!==order.kinds[order.kind(record,col)]).map(tag=>`<span class="record-tag">${esc(tag)}</span>`).join('');
        article.querySelector('.record-body').append(meta);
        let actions=article.querySelector('.record-body > .record-toolbar');
        if(!actions){actions=document.createElement('div');actions.className='record-toolbar';article.querySelector('.record-body').append(actions);}
        const detailButton=document.createElement('button');detailButton.className='btn btn-ghost btn-sm';detailButton.innerHTML='<i class="fas fa-book-open"></i> 详情';detailButton.setAttribute('onclick',`StudyLibrary.open(${JSON.stringify(record.id)})`);actions.prepend(detailButton);
        if(col==='papers'){const score=document.createElement('button');score.className='btn btn-ghost btn-sm';score.innerHTML='<i class="fas fa-chart-line"></i> 录入成绩';score.setAttribute('onclick',`GradeBook.forPaper(${JSON.stringify(record.id)})`);actions.append(score);}
        return article.outerHTML;
    };
    function table(records,col){return `<table class="library-table"><thead><tr><th></th><th>名称</th><th class="library-path-cell">分类</th><th class="library-date-cell">记录日期</th><th>附件</th><th></th></tr></thead><tbody>${groupedPage(records,col,rows=>rows.map(record=>`<tr><td><input type="checkbox" data-record-select="${esc(record.id)}" ${view(col).selected.has(record.id)?'checked':''} aria-label="选择 ${esc(record.title)}" /></td><td><button class="library-record-title" onclick="StudyLibrary.open('${record.id}')">${esc(record.title)}</button><div class="library-record-metadata">${attributes(record,col)}</div>${col==='mistakes'?`<small class="mastery-${mastery(record)}">${label(record)}</small>`:''}</td><td class="library-path-cell">${esc(pathKey(record))}</td><td class="library-date-cell">${esc(record.date)}</td><td>${files(record).length}</td><td><button class="btn btn-icon btn-ghost" onclick="editRecord('${record.id}')" title="编辑" aria-label="编辑"><i class="fas fa-pen"></i></button></td></tr>`).join(''),true)}</tbody></table>`;}
    renderRecordList=(container, records)=>{
        const col=container.dataset.collection;if(!configs[col])return;
        const root=mount(col),ui=view(col);ui.results=filter(col,records);
        const existing=new Set(records.map(record=>record.id));for(const id of ui.selected)if(!existing.has(id))ui.selected.delete(id);
        const pages=Math.max(1,Math.ceil(ui.results.length/ui.pageSize));ui.page=Math.min(ui.page,pages);
        const filterBar=el(configs[col][1]);filterBar.dataset.activeFilter=ui.subject;filterBar.querySelectorAll('button').forEach((button,index)=>button.classList.toggle('active',ui.subject==='all'?index===0:button.textContent.startsWith(ui.subject)));
        const counts=facets(records,ui);
        for(const input of root.querySelectorAll('[data-field]')){
            const field=input.dataset.field;
            if(counts[field])facet(input,counts[field],ui[field],{role:'所有资料类型',type:'所有文件格式',year:'所有资料年份',month:'所有记录月份',tag:'所有标签'}[field],field==='role'?order.kinds:field==='type'?order.formats:field==='year'?{'$unknown':'年份待核验'}:{},field==='year'?[...counts.year.keys()].filter(key=>key!=='$unknown').sort((a,b)=>Number(b)-Number(a)).concat(counts.year.has('$unknown')?['$unknown']:[]):field==='month'?[...counts.month.keys()].sort().reverse():undefined);
            if(document.activeElement!==input){if(input.type==='checkbox')input.checked=Boolean(ui[field]);else input.value=ui[field];}
        }
        root.querySelectorAll('[data-layout]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.layout===ui.layout)));
        const selected=ui.results.slice((ui.page-1)*ui.pageSize,ui.page*ui.pageSize);container.className=ui.layout==='table'?'library-list':'library-gallery';
        container.innerHTML=selected.length?(ui.layout==='table'?table(selected,col):groupedPage(selected,col,rows=>rows.map(record=>renderRecordCard(record,extractSubject(record.path||record.subject))).join(''))):`<div class="empty-state"><i class="fas fa-folder-open" aria-hidden="true"></i><p>${records.length?'没有符合筛选条件的记录':'还没有'+configs[col][2]}</p><button class="btn btn-ghost btn-sm" onclick="${col==='papers'?'openPaperDialog()':`openRecordDialog('${col}')`}"><i class="fas fa-plus"></i> 添加${configs[col][2]}</button></div>`;
        const day=today();root.querySelector('.library-summary').textContent=`${ui.folder||'全部分类'} · ${ui.results.length} / ${records.length} 条${col==='mistakes'?` · ${records.filter(record=>due(record,day)).length} 条待复习`:''}`;
        const pager=root.querySelector('.library-pager'),first=selected.length?(ui.page-1)*ui.pageSize+1:0,last=(ui.page-1)*ui.pageSize+selected.length;
        for(const range of root.querySelectorAll('.library-page-range'))range.textContent=`本页显示 ${first}-${selected.length?last:0} 条，共 ${ui.results.length} 条`;
        for(const jump of root.querySelectorAll('[data-page-jump]')){if(jump._pages!==pages){jump.innerHTML=Array.from({length:pages},(_,index)=>`<option value="${index+1}">第 ${index+1} / ${pages} 页</option>`).join('');jump._pages=pages;}jump.value=String(ui.page);jump.disabled=pages===1;}
        for(const size of root.querySelectorAll('[data-page-size]'))size.value=String(ui.pageSize);
        for(const controls of root.querySelectorAll('.library-pager,.library-page-top')){for(const action of ['first','previous'])controls.querySelector(`[data-action=${action}]`).disabled=ui.page===1;for(const action of ['next','last'])controls.querySelector(`[data-action=${action}]`).disabled=ui.page===pages;}
        pager.querySelector('[data-action=select-page]').disabled=!selected.length;
        const reviewButton=root.querySelector('[data-action=review]');if(reviewButton)reviewButton.disabled=!ui.results.length;
        renderFolders(col,root);renderBulk(col);updateResourceCounts();StudioUI.refreshPreviews();StudioUI.updateWorkspace();refreshDetail(col);
    };
    const previousFilter=filterRecords;
    filterRecords=(button,key)=>{
        const col=button.parentElement.closest('.library-shell')?.dataset.col;
        if(!col)return previousFilter(button,key);
        view(col).subject=key;view(col).page=1;remember();button.parentElement.dataset.activeFilter=key;refreshRecords(col);
    };
    function renderBulk(col){const root=mount(col),bar=root.querySelector('.library-bulk'),selected=view(col).selected;bar.hidden=!selected.size;bar.querySelector('span').textContent=`已选 ${selected.size} 条`;root.querySelectorAll('[data-record-select]').forEach(input=>input.checked=selected.has(input.dataset.recordSelect));}
    async function persist(col){saveState();refreshRecords(col);StudioUI.updateWorkspace();await PrepLocal.flush();}
    function saved(col,id){refreshDetail(col);const ui=view(col),index=ui.results.findIndex(record=>record.id===id);if(index<0)return false;ui.page=Math.floor(index/ui.pageSize)+1;refreshRecords(col);return true;}
    async function favorite(id){const {record,col}=findRecordWithCol(id);if(record){record.favorite=!record.favorite;await persist(col);if(detail?.ids[detail.index]===id)renderDetail();}}
    async function removeSelected(col){
        const selected=view(col).selected,records=(state[col]||[]).filter(record=>selected.has(record.id));
        if(!records.length||!confirm(`将所选 ${records.length} 条记录移入回收站？`)||!confirm('继续删除？记录及其附件可从回收站恢复。'))return;
        const attachments=new Set(records.flatMap(record=>files(record).map(file=>file.id)));
        state[col]=state[col].filter(record=>!selected.has(record.id));selected.clear();await persist(col);
        if(!PrepLocal.hasPending())for(const id of attachments)await deleteFile(id).catch(error=>{if(error.status!==409)PrepLocal.toast(error.message);});
        PrepLocal.toast(PrepLocal.hasPending()?'删除已保留，等待同步':'所选记录已删除');
    }
    function bulkDialog(col,mode){
        const dialog=el('libraryBulkDialog'),form=el('libraryBulkForm');form.reset();form.dataset.col=col;form.dataset.mode=mode;
        el('libraryBulkTitle').textContent=mode==='move'?'移动分类':mode==='classify'?'批量资料类型':'批量标签';
        el('libraryBulkFields').innerHTML=mode==='move'?`<label>目标分类${col==='papers'?`<select name="destination">${SUBJECTS.map(subject=>`<option>${subject}</option>`).join('')}</select>`:'<input name="destination" required list="libraryFolderOptions" />'}</label>`:mode==='classify'?classificationFields(false):'<label>标签<input name="tags" required /></label><label class="workflow-checkbox"><input type="checkbox" name="append" checked /> 追加到现有标签</label>';
        dialog.showModal();
    }
    function classificationFields(withYear=true) {
        return `<label>资料类型<select name="libraryRole"><option value="">自动归类</option>${Object.entries(order.kinds).map(([key,label])=>`<option value="${key}">${label}</option>`).join('')}</select></label>${withYear?'<label>资料年份<input name="libraryYear" placeholder="2025 / 1987-2023 / 留空待核验" /></label>':''}`;
    }
    function fillClassification(form,record={}) {
        const select=form.elements.libraryRole;select.querySelector('[data-unknown-role]')?.remove();
        if(record.role&&!Object.hasOwn(order.kinds,record.role)){const option=new Option(record.role,record.role);option.dataset.unknownRole='true';select.append(option);}
        select.value=record.role||'';
        if(form.elements.libraryYear)form.elements.libraryYear.value=order.yearText(record.year);
        else form.elements.year.value=order.yearText(record.year);
    }
    function classify(record,fd) {
        if(fd.has('libraryRole')){const role=String(fd.get('libraryRole')||'');if(role)record.role=role;else delete record.role;}
        const yearField=fd.has('libraryYear')?'libraryYear':fd.has('year')?'year':null;
        if(yearField){const year=order.parseYearInput(fd.get(yearField));if(year==='')delete record.year;else record.year=year;}
    }
    function updateResourceCounts(){for(const button of el('resourceLibraryTabs')?.querySelectorAll('[data-col]')||[]){const count=button.querySelector('.library-tab-count');if(count)count.textContent=String((state[button.dataset.col]||[]).length);}}
    function selectResourceTab(col){
        if(!['notes','resources'].includes(col))return;
        resourceTab=col;remember();
        const root=el('view-resources');root.querySelectorAll(':scope > .card').forEach(card=>card.hidden=card.querySelector('#'+configs[col][0])===null);
        el('resourceLibraryTabs')?.querySelectorAll('button').forEach(button=>{button.setAttribute('aria-selected',String(button.dataset.col===col));button.classList.toggle('active',button.dataset.col===col);});updateResourceCounts();StudioUI.refreshPreviews();
    }
    function paperAttachments(id){const record=state.papers.find(record=>record.id===id);el('existingPaperAttachments').innerHTML=files(record||{}).map(file=>`<div class="attachment-row"><span>${esc(file.name)}</span><button type="button" class="btn btn-icon btn-danger" onclick="PrepLocal.removeAttachment('${id}','${file.id}')" title="删除此附件" aria-label="删除此附件"><i class="fas fa-trash"></i></button></div>`).join('');}
    function fillExtras(form,record,col){
        form.elements.tags.value=tags(record||{}).join(', ');form.elements.favorite.checked=Boolean(record?.favorite);
        fillClassification(form,record||{});
        if(col==='mistakes'){form.elements.cause.value=record?.cause||'';form.elements.solution.value=record?.solution||'';form.elements.mastery.value=mastery(record||{});form.elements.nextReview.value=record?.nextReview||'';}
        el('mistakeFields').hidden=col!=='mistakes';
        el('libraryFolderOptions').innerHTML=[...new Set((state[col]||[]).map(pathKey))].map(path=>`<option value="${esc(path)}"></option>`).join('');
    }
    const previousOpen=openRecordDialog;
    openRecordDialog=(col,id)=>{previousOpen(col,id);selectResourceTab(col);fillExtras(el('recordForm'),state[col].find(record=>record.id===id),col);if(id)touch(id);if(!id&&view(col).folder)el('recordForm').elements.path.value=view(col).folder;};
    const previousPaper=openPaperDialog;
    openPaperDialog=id=>{previousPaper(id);PrepLocal.resetUploads('paperForm');const record=state.papers.find(record=>record.id===id),form=el('paperForm');form.elements.title.value=record?.title||'';form.elements.tags.value=tags(record||{}).join(', ');form.elements.favorite.checked=Boolean(record?.favorite);form.elements.practiceStatus.value=record?.practiceStatus||'todo';fillClassification(form,record||{});paperAttachments(id);if(id)touch(id);};
    function open(id,reviewMode=false){
        const {record,col}=findRecordWithCol(id);if(!record||!configs[col])return;
        const ids=view(col).results.map(record=>record.id);detail={col,ids:ids.includes(id)?ids:[id],index:Math.max(0,ids.indexOf(id)),review:reviewMode,marked:new Set(),showSolution:!reviewMode};renderDetail();el('studyDetail').showModal();
    }
    function review(col){const records=view(col).results;if(!records.length)return;detail={col,ids:records.map(record=>record.id),index:0,review:true,marked:new Set(),showSolution:false};renderDetail();el('studyDetail').showModal();}
    function refreshDetail(col){
        if(!detail||!el('studyDetail')?.open||(col&&col!==detail.col))return;
        const record=findRecord(detail.ids[detail.index]),snapshot=JSON.stringify(record);
        if(snapshot===detail.snapshot||(record&&detail.completed))return;
        const body=el('studyDetailBody'),scroll=body.scrollTop;renderDetail();body.scrollTop=scroll;
    }
    function renderDetail(){
        if(!detail)return;const record=findRecord(detail.ids[detail.index]);
        if(!record){detail.snapshot=undefined;detail.completed=false;el('studyDetailBody').innerHTML='<div class="empty-state"><p>这条记录已被删除</p></div>';el('studyDetailEdit').onclick=null;el('studyDetailDownload').hidden=true;el('studyReviewControls').hidden=true;return;}
        el('studyDetailTitle').textContent=record.title||'记录详情';el('studyDetailPosition').textContent=`${detail.index+1} / ${detail.ids.length}`;
        el('studyDetailPrevious').disabled=detail.index===0;el('studyDetailNext').disabled=detail.index===detail.ids.length-1;
        const metadata=tags(record).map(tag=>`<span class="record-tag">${esc(tag)}</span>`).join('');
        el('studyDetailBody').innerHTML=`<div class="detail-meta">${esc(pathKey(record))} · ${esc(record.date)} ${metadata}</div><section class="detail-text"><h4>${detail.col==='mistakes'?'题目与记录':'内容'}</h4><p>${esc(record.content||record.remark||'暂无文字内容')}</p></section>${files(record).length?`<section class="detail-attachments"><h4>附件 (${files(record).length})</h4>${files(record).map((file,index)=>`<button class="attachment-button" onclick="openViewer('${record.id}',${index})"><i class="fas ${file.type?.startsWith('image/')?'fa-image':'fa-file-lines'}"></i><span>${esc(file.name)}</span><i class="fas fa-expand"></i></button>`).join('')}</section>`:''}${detail.col==='mistakes'?`<section class="detail-text"><h4>错误原因</h4><p>${esc(record.cause||'未填写')}</p></section><section class="detail-text"><h4>解析与解题思路</h4>${detail.showSolution?`<p>${esc(record.solution||'未填写解析')}</p>`:'<button class="btn btn-ghost" onclick="StudyLibrary.reveal()"><i class="fas fa-eye"></i> 查看解析</button>'}</section>`:''}`;
        el('studyReviewControls').hidden=!detail.review;el('studyDetailEdit').onclick=()=>{el('studyDetail').close();editRecord(record.id);};el('studyDetailDownload').hidden=!files(record).length;el('studyDetailDownload').onclick=()=>downloadRecord(record.id);
        if(detail.currentId!==record.id){el('studyNextReview').value=record.nextReview||nextDate(1);el('studyReviewInterval').value=record.nextReview?'custom':'1';detail.currentId=record.id;touch(record.id);}
        detail.snapshot=JSON.stringify(record);
        window.dispatchEvent(new CustomEvent('study-detail',{detail:{id:record.id,col:detail.col,review:detail.review}}));
    }
    function nextDate(days){const date=new Date(today()+'T12:00:00Z');date.setUTCDate(date.getUTCDate()+Number(days));return date.toISOString().slice(0,10);}
    function moveDetail(step){if(!detail)return;detail.completed=false;detail.index=Math.min(detail.ids.length-1,Math.max(0,detail.index+step));detail.showSolution=!detail.review;renderDetail();}
    async function rate(value){
        const record=findRecord(detail?.ids[detail.index]);if(!record)return;
        record.mastery=value;record.reviewedAt=PrepLocal.nowDate().toISOString();record.nextReview=value==='mastered'?'':el('studyNextReview').value;detail.marked.add(record.id);await persist(detail.col);
        if(detail.index<detail.ids.length-1)moveDetail(1);else{detail.completed=true;el('studyDetailBody').innerHTML=`<div class="empty-state"><i class="fas fa-circle-check"></i><p>已到本组末尾</p><p>${detail.marked.size} / ${detail.ids.length} 条已标记</p><button class="btn btn-ghost" onclick="StudyLibrary.restartReview()">继续复习</button></div>`;el('studyReviewControls').hidden=true;}
    }
    function setup(){
        window.addEventListener('pagehide',saveUsage);
        document.addEventListener('visibilitychange',()=>{if(document.hidden)saveUsage();});
        document.addEventListener('close',event=>{
            if(!['studyDetail','viewerDialog','readerDialog','mediaDialog','richDialog','recordDialog','paperDialog'].includes(event.target.id))return;
            for(const col of usageDirty)if(document.body.dataset.view===(['notes','resources'].includes(col)?'resources':col)){usageDirty.delete(col);refreshRecords(col);}
        },true);
        const recordExtra=document.createElement('div');recordExtra.className='record-extra-fields';recordExtra.innerHTML='<label>标签<input name="tags" /></label><label class="workflow-checkbox"><input type="checkbox" name="favorite" /> 收藏</label><section id="mistakeFields" hidden><label>错误原因<input name="cause" list="mistakeCauseOptions" /></label><datalist id="mistakeCauseOptions"><option value="概念不清"></option><option value="计算失误"></option><option value="审题错误"></option><option value="知识遗忘"></option></datalist><label>解析与解题思路<textarea name="solution" rows="4"></textarea></label><div class="grid-2"><label>掌握情况<select name="mastery"><option value="new">待复习</option><option value="review">仍需练习</option><option value="mastered">已掌握</option></select></label><label>下次复习<input type="date" name="nextReview" /></label></div></section>';
        el('recordForm').querySelector('[name=files]').previousElementSibling.before(recordExtra);el('recordForm').elements.path.setAttribute('list','libraryFolderOptions');
        const classification=document.createElement('div');classification.className='grid-2';classification.innerHTML=classificationFields();recordExtra.prepend(classification);
        const paperExtra=document.createElement('div');paperExtra.className='record-extra-fields';paperExtra.innerHTML='<label>标签<input name="tags" /></label><div class="grid-2"><label>练习状态<select name="practiceStatus"><option value="todo">未开始</option><option value="doing">进行中</option><option value="done">已完成</option></select></label><label class="workflow-checkbox"><input type="checkbox" name="favorite" /> 收藏</label></div>';el('paperForm').querySelector('[name=file]').previousElementSibling.before(paperExtra);
        paperExtra.insertAdjacentHTML('afterbegin',classificationFields(false));el('paperForm').elements.year.required=false;el('paperForm').elements.year.placeholder='2025 / 1987-2023 / 留空待核验';
        const content=document.createElement('div');content.innerHTML='<datalist id="libraryFolderOptions"></datalist><dialog id="libraryBulkDialog"><div class="dialog-header"><h3 id="libraryBulkTitle"></h3><button class="btn btn-icon" onclick="this.closest(\'dialog\').close()" title="返回" aria-label="返回"><i class="fas fa-xmark"></i></button></div><form id="libraryBulkForm" class="dialog-body"><div id="libraryBulkFields"></div><div class="dialog-actions"><button class="btn btn-ghost" type="button" onclick="this.closest(\'dialog\').close()">退回</button><button class="btn btn-primary" type="submit">继续并保存</button></div></form></dialog><dialog id="studyDetail" class="study-detail"><header class="study-detail-header"><button class="btn btn-icon btn-ghost" onclick="this.closest(\'dialog\').close()" title="返回列表" aria-label="返回列表"><i class="fas fa-arrow-left"></i></button><h3 id="studyDetailTitle"></h3><button class="btn btn-icon btn-ghost" id="studyDetailEdit" title="编辑" aria-label="编辑"><i class="fas fa-pen"></i></button><button class="btn btn-icon btn-ghost" id="studyDetailDownload" title="下载附件" aria-label="下载附件"><i class="fas fa-download"></i></button><button class="btn btn-icon btn-ghost" onclick="StudyLibrary.fullscreen()" title="全屏阅读" aria-label="全屏阅读"><i class="fas fa-expand"></i></button></header><nav class="detail-pager"><button class="btn btn-icon btn-ghost" id="studyDetailPrevious" onclick="StudyLibrary.move(-1)" title="上一条" aria-label="上一条"><i class="fas fa-chevron-left"></i></button><span id="studyDetailPosition"></span><button class="btn btn-icon btn-ghost" id="studyDetailNext" onclick="StudyLibrary.move(1)" title="下一条" aria-label="下一条"><i class="fas fa-chevron-right"></i></button></nav><div id="studyDetailBody" class="study-detail-body"></div><footer id="studyReviewControls" class="study-review-controls" hidden><label>下次复习<select id="studyReviewInterval"><option value="1">明天</option><option value="3">3 天后</option><option value="7">7 天后</option><option value="custom">自定日期</option></select><input type="date" id="studyNextReview" /></label><div><button class="btn btn-ghost" onclick="StudyLibrary.move(1)">稍后处理</button><button class="btn btn-ghost" onclick="StudyLibrary.rate(\'review\')">仍需练习</button><button class="btn btn-primary" onclick="StudyLibrary.rate(\'mastered\')">已经掌握</button></div></footer></dialog>';
        document.body.append(...content.childNodes);
        const tabs=document.createElement('div');tabs.id='resourceLibraryTabs';tabs.className='resource-library-tabs';tabs.setAttribute('role','tablist');tabs.innerHTML='<button role="tab" data-col="notes" onclick="StudyLibrary.tab(\'notes\')"><i class="fas fa-book-open"></i> 学习笔记 <span class="library-tab-count"></span></button><button role="tab" data-col="resources" onclick="StudyLibrary.tab(\'resources\')"><i class="fas fa-folder-open"></i> 电子资料 <span class="library-tab-count"></span></button>';el('view-resources').prepend(tabs);
        el('studyReviewInterval').onchange=event=>{if(event.target.value!=='custom')el('studyNextReview').value=nextDate(event.target.value);};el('studyNextReview').oninput=()=>el('studyReviewInterval').value='custom';
        el('libraryBulkForm').onsubmit=async event=>{event.preventDefault();const form=event.target,fd=new FormData(form),col=form.dataset.col;for(const record of state[col])if(view(col).selected.has(record.id)){if(form.dataset.mode==='move')record[col==='papers'?'subject':'path']=fd.get('destination').trim();else if(form.dataset.mode==='classify')classify(record,fd);else{const next=String(fd.get('tags')).split(/[,，;；]/).map(tag=>tag.trim()).filter(Boolean);record.tags=[...new Set(fd.has('append')?[...tags(record),...next]:next)];}}await persist(col);el('libraryBulkDialog').close();};
    }
    const previousReady=PrepLocal.ready;
    PrepLocal.ready=()=>{setup();previousReady();selectResourceTab(resourceTab);let day=today();setInterval(()=>{const current=today();if(current!==day){day=current;if(document.body.dataset.view==='mistakes')refreshRecords('mistakes');}},60000);};
    const previousView=renderView;
    renderView=which=>{previousView(which);if(which==='resources')selectResourceTab(resourceTab);refreshDetail();};
    for(const event of ['workspace-change','yantu-sync'])addEventListener(event,()=>refreshDetail());
    window.StudyLibrary={open,favorite,saved,touch,classify,tab:selectResourceTab,move:moveDetail,rate,paperAttachments,reveal:()=>{detail.showSolution=true;renderDetail();},restartReview:()=>{detail.completed=false;detail.index=0;detail.marked.clear();detail.showSolution=false;el('studyReviewControls').hidden=false;renderDetail();},fullscreen:()=>el('studyDetail').classList.toggle('is-fullscreen'),refresh:col=>{refreshRecords(col);refreshDetail(col);},files};
})();
