(() => {
    const W=Workspace,{el,esc,clone}=W,collection='schoolCatalog';
    const compare=new Intl.Collator('zh-CN',{numeric:true}).compare;
    const normalize=value=>String(value||'').trim().replace(/[\s（）()·]/g,'').toLocaleLowerCase();
    const now=()=>PrepLocal.nowDate().toISOString();
    let seed=null,loading,editor,detail,initialized=false,searchTimer;
    let ui={tab:'targets',q:'',province:'',year:'',evidence:'',status:'',sort:'year',layout:matchMedia('(max-width:700px)').matches?'cards':'table',page:1,pageSize:24};
    try{const saved=JSON.parse(YantuEnvironment.storage.getItem('yantu-school-browser-v1')||'{}');for(const key of ['tab','sort','layout','pageSize'])if(key in saved)ui[key]=saved[key];}catch(_){}
    if(!['targets','catalog'].includes(ui.tab))ui.tab='targets';
    if(![24,48,96].includes(ui.pageSize))ui.pageSize=24;
    if(!['table','cards'].includes(ui.layout))ui.layout='table';
    if(!['year','name','province','updated'].includes(ui.sort))ui.sort='year';
    function remember(){try{YantuEnvironment.storage.setItem('yantu-school-browser-v1',JSON.stringify({tab:ui.tab,sort:ui.sort,layout:ui.layout,pageSize:ui.pageSize}));}catch(_) {}}
    function safeURL(value){try{const url=new URL(String(value).trim());return ['https:','http:'].includes(url.protocol)&&!url.username&&!url.password?url.href:'';}catch(_){return '';}}
    function sources(record){return (record.sources||[]).filter(item=>item&&safeURL(item.url)).map(item=>({...item,url:safeURL(item.url)}));}
    function withSources(record){
        const list=sources(record);let notes=String(record.formula||'').trim();
        for(const item of list)if(!notes.includes(item.url))notes+=(notes?'\n\n':'')+`信息来源：${item.title||'原始公告'}\n${item.url}`;
        return {...record,sources:list,formula:notes};
    }
    function records(){
        const map=new Map((seed?.schools||[]).map(item=>[item.id,item]));
        for(const item of state[collection]||[])map.set(item.id,{...map.get(item.id),...item});
        return [...map.values()];
    }
    const catalogRecord=id=>records().find(item=>item.id===id);
    function targetFor(record){const names=new Set([record.name,...record.aliases||[]].map(normalize));return (state.schools||[]).find(item=>item.catalogId===record.id||names.has(normalize(item.name)));}
    async function load(){
        if(seed)return seed;if(loading)return loading;
        loading=(async()=>{
            try{const response=await fetch(YantuEnvironment.pathname('/data/school-catalog.json'),{cache:'no-cache'});if(!response.ok)throw new Error('院校资料文件暂不可用');const value=await response.json();if(value.format!=='yantu-school-catalog-v1'||!Array.isArray(value.schools))throw new Error('院校库格式不正确');seed=value;DeviceStore.put('school-catalog-seed',value).catch(()=>{});}
            catch(error){const cached=await DeviceStore.get('school-catalog-seed');if(cached?.format==='yantu-school-catalog-v1'&&Array.isArray(cached.schools))seed=cached;else throw error;}
            return seed;
        })();try{return await loading;}finally{loading=null;}
    }
    async function persist(){saveState();await PrepLocal.flush();renderSchools();render();}
    function sourceMarkup(record){const list=sources(record);return list.length?`<div class="school-source-links">${list.map(item=>`<a href="${esc(item.url)}" target="_blank" rel="noopener noreferrer"><i class="fas fa-arrow-up-right-from-square" aria-hidden="true"></i> ${esc(item.title||'信息来源')}</a>`).join('')}</div>`:'';}
    function metricFacts(record){return `<dl class="school-facts school-metrics"><div><dt>复录比</dt><dd>${esc(record.ratio||'待核验')}</dd></div><div><dt>初试占比</dt><dd>${esc(record.preWeight||'待核验')}</dd></div><div><dt>复试占比</dt><dd>${esc(record.reWeight||'待核验')}</dd></div><div><dt>往年录取平均分</dt><dd>${esc(record.admissionAverage||'待核验')}</dd></div></dl>`;}
    function facts(record){return metricFacts(record)+`<dl class="school-facts"><div class="school-fact-wide"><dt>初试科目</dt><dd>${esc(record.preSubjects||'待按专业及年度补充')}</dd></div><div class="school-fact-wide"><dt>复试科目</dt><dd>${esc(record.reSubjects||'待按专业及年度补充')}</dd></div></dl>`;}
    function historyMarkup(record){
        const rows=Array.isArray(record.admissionHistory)?record.admissionHistory:[];if(!rows.length)return '';
        return `<section class="school-history"><h4>往年录取数据与口径</h4>${rows.map(item=>`<details><summary>${esc(item.year||'年度待核验')} 年 · ${esc(item.program||'专业待核验')}</summary>${metricFacts(item)}${item.plannedRatio?`<p class="school-full-notes"><strong>计划差额比例</strong> ${esc(item.plannedRatio)}</p>`:''}${item.formula?`<p class="school-full-notes"><strong>成绩公式</strong> ${esc(item.formula)}</p>`:''}<p class="school-full-notes">${esc(item.notes||'')}</p>${sourceMarkup(item)}</details>`).join('')}</section>`;
    }
    function location(record){return [...new Set([record.province,record.city].filter(Boolean))].join(' · ')||'地区待补充';}
    function scope(record){return [record.enrollmentYear?record.enrollmentYear+' 年招生':'招生年度未指定',record.program||'专业未指定'].join(' · ');}
    function evidenceLabel(record){return record.personalEdit?'个人修改 · 待复核':({directory:'目录 / 简章已核对',notice:'官方答疑已核对',adjustment:'调整公告 · 待最终目录'}[record.evidenceKind]||'个人补充 · 待核验');}
    function badges(record){return `<div class="school-badges">${record.listRound?`<span>双一流 · 第${esc(record.listRound)}轮</span>`:''}${record.examCode==='408'?'<span>408</span>':''}<span class="${record.evidenceKind==='adjustment'||record.personalEdit?'school-pending':''}">${esc(evidenceLabel(record))}</span>${record.enrollmentYear&&Number(record.enrollmentYear)<2027?'<span class="school-pending">往年参考</span>':''}${record.hidden?'<span>已隐藏</span>':''}</div>`;}
    function card(record,kind){
        const target=kind==='catalog'?targetFor(record):null;
        return `<article class="school-card school-record"><header><h4><button data-school-action="detail" data-kind="${kind}" data-id="${esc(record.id)}">${esc(record.name)}</button></h4><span class="school-location">${esc(location(record))}</span></header>${badges(record)}<p class="school-scope">${esc(scope(record))}</p>${record.disciplines?.length?`<p class="school-disciplines"><strong>建设学科</strong> ${esc(record.disciplines.join('、'))}</p>`:''}${facts(record)}${record.formula?`<p class="school-notes">${esc(record.formula)}</p>`:''}${sourceMarkup(record)}<div class="school-row-actions">${kind==='catalog'?`<button class="btn ${target?'btn-ghost':'btn-primary'} btn-sm" data-school-action="${target?'target':'add-target'}" data-id="${esc(record.id)}"><i class="fas fa-${target?'arrow-up-right-from-square':'plus'}"></i> ${target?'已在目标':'加入目标'}</button>`:''}<button class="btn btn-ghost btn-sm" data-school-action="edit" data-kind="${kind}" data-id="${esc(record.id)}"><i class="fas fa-pen"></i> 修改</button><button class="btn btn-ghost btn-sm" data-school-action="${record.hidden?'restore':'remove'}" data-kind="${kind}" data-id="${esc(record.id)}"><i class="fas fa-${record.hidden?'rotate-left':'trash'}"></i> ${record.hidden?'恢复':'删除'}</button></div></article>`;
    }
    function table(items){return `<div class="school-table-wrap"><table class="school-table"><thead><tr><th>院校 / 年度</th><th>408招生范围</th><th>往年录取参考</th><th>操作</th></tr></thead><tbody>${items.map(record=>{const target=targetFor(record);return `<tr><td><button class="school-name" data-school-action="detail" data-kind="catalog" data-id="${esc(record.id)}">${esc(record.name)}</button><small>${esc(location(record))} · ${esc(record.enrollmentYear||'待核验')} 年招生</small>${badges(record)}</td><td><p class="school-cell-disciplines">${esc(record.program||'待核验')}</p><small class="school-cell-subjects">${esc(record.preSubjects||'待核验')}</small></td><td>${metricFacts(record)}</td><td><div class="school-row-actions"><button class="btn ${target?'btn-ghost':'btn-primary'} btn-sm" data-school-action="${target?'target':'add-target'}" data-id="${esc(record.id)}"><i class="fas fa-${target?'arrow-up-right-from-square':'plus'}"></i> ${target?'已在目标':'加入目标'}</button><button class="btn btn-icon btn-ghost" data-school-action="edit" data-kind="catalog" data-id="${esc(record.id)}" title="修改院校资料" aria-label="修改 ${esc(record.name)}"><i class="fas fa-pen"></i></button><button class="btn btn-icon btn-ghost" data-school-action="${record.hidden?'restore':'remove'}" data-kind="catalog" data-id="${esc(record.id)}" title="${record.hidden?'恢复':'移出院校库'}" aria-label="${record.hidden?'恢复':'移出院校库'} ${esc(record.name)}"><i class="fas fa-${record.hidden?'rotate-left':'trash'}"></i></button></div></td></tr>`;}).join('')}</tbody></table></div>`;}
    function options(values,selected,all){return `<option value="">${all}</option>`+values.map(value=>`<option value="${esc(value)}" ${value===selected?'selected':''}>${esc(value)}</option>`).join('');}
    function results(){const terms=ui.q.trim().split(/\s+/).filter(Boolean).map(value=>normalize(value).replace(/英一/g,'英语一').replace(/英二/g,'英语二').replace(/数一/g,'数学一').replace(/数二/g,'数学二'));return records().filter(record=>{
        if(ui.status==='hidden'?!record.hidden:record.hidden)return false;
        if(ui.province&&record.province!==ui.province)return false;
        if(ui.year&&String(record.enrollmentYear)!==ui.year)return false;
        if(ui.evidence&&(record.personalEdit?'personal':record.evidenceKind||'personal')!==ui.evidence)return false;
        const target=targetFor(record);if(ui.status==='added'&&!target||ui.status==='unadded'&&target)return false;
        const text=terms.length?normalize([record.name,record.schoolCode,record.province,record.city,...record.aliases||[],...record.disciplines||[],record.program,record.preSubjects].join(' ')):'';
        return terms.every(term=>text.includes(term));
    }).sort((a,b)=>ui.sort==='province'?(compare(a.province||'',b.province||'')||compare(a.name,b.name)):ui.sort==='updated'?(String(b.updatedAt||'').localeCompare(String(a.updatedAt||''))||compare(a.name,b.name)):ui.sort==='year'?(compare(b.enrollmentYear||'',a.enrollmentYear||'')||compare(a.name,b.name)):compare(a.name,b.name));}
    function tabs(){
        el('schoolTargetCount').textContent=String((state.schools||[]).length);
        el('schoolCatalogCount').textContent=seed?String(records().filter(item=>!item.hidden).length):'…';
        for(const button of el('schoolBrowserTabs').querySelectorAll('[data-school-tab]')){const active=button.dataset.schoolTab===ui.tab;button.classList.toggle('active',active);button.setAttribute('aria-selected',String(active));}
        el('schoolTargetPanel').hidden=ui.tab!=='targets';el('schoolCatalogPanel').hidden=ui.tab!=='catalog';
    }
    async function selectTab(tab){ui.tab=tab==='catalog'?'catalog':'targets';remember();tabs();if(ui.tab==='catalog')await render();else renderSchools();}
    function pagerHTML(){return '<span class="library-page-range"></span><div class="library-page-controls"><button class="btn btn-icon btn-ghost" data-school-action="first" title="首页" aria-label="首页"><i class="fas fa-angles-left"></i></button><button class="btn btn-icon btn-ghost" data-school-action="previous" title="上一页" aria-label="上一页"><i class="fas fa-chevron-left"></i></button><select data-school-page aria-label="院校库页码"></select><button class="btn btn-icon btn-ghost" data-school-action="next" title="下一页" aria-label="下一页"><i class="fas fa-chevron-right"></i></button><button class="btn btn-icon btn-ghost" data-school-action="last" title="末页" aria-label="末页"><i class="fas fa-angles-right"></i></button></div><select data-school-size aria-label="院校库每页条数"><option value="24">每页 24 所</option><option value="48">每页 48 所</option><option value="96">每页 96 所</option></select>';}
    async function render(){
        if(!initialized||ui.tab!=='catalog')return;
        const host=el('schoolCatalogResults');
        if(!seed){host.innerHTML='<p class="school-muted" role="status">正在读取院校库…</p>';try{await load();}catch(error){host.innerHTML=`<p class="school-muted" role="alert">${esc(error.message)}</p><button class="btn btn-ghost" data-school-action="retry"><i class="fas fa-rotate-right"></i> 重试</button>`;return;}}
        if(ui.tab!=='catalog')return;
        tabs();
        const all=records(),items=results(),pages=Math.max(1,Math.ceil(items.length/ui.pageSize));ui.page=Math.max(1,Math.min(pages,ui.page));
        const page=items.slice((ui.page-1)*ui.pageSize,ui.page*ui.pageSize);
        el('schoolProvinceFilter').innerHTML=options([...new Set(all.map(item=>item.province).filter(Boolean))].sort(compare),ui.province,'所有地区');
        el('schoolYearFilter').innerHTML=options([...new Set(all.map(item=>String(item.enrollmentYear||'')).filter(Boolean))].sort(compare).reverse(),ui.year,'所有招生年度');
        el('schoolCatalogMeta').textContent=`已收录 ${all.filter(item=>!item.hidden).length} 所 · 当前 ${items.length} 所 · 核验 ${seed.checkedAt||'待核验'}`;
        el('schoolCatalogCoverage').textContent=[seed.coverage,seed.metricsNote].filter(Boolean).join(' ');
        for(const nav of el('schoolCatalogPanel').querySelectorAll('.school-pages')){
            nav.querySelector('.library-page-range').textContent=`本页 ${page.length?(ui.page-1)*ui.pageSize+1:0}-${page.length?(ui.page-1)*ui.pageSize+page.length:0} 所，共 ${items.length} 所`;
            const jump=nav.querySelector('[data-school-page]');jump.innerHTML=Array.from({length:pages},(_,i)=>`<option value="${i+1}">第 ${i+1} / ${pages} 页</option>`).join('');jump.value=String(ui.page);jump.disabled=pages===1;nav.querySelector('[data-school-size]').value=String(ui.pageSize);
            for(const action of ['first','previous'])nav.querySelector(`[data-school-action=${action}]`).disabled=ui.page===1;
            for(const action of ['next','last'])nav.querySelector(`[data-school-action=${action}]`).disabled=ui.page===pages;
        }
        host.className=ui.layout==='cards'?'school-grid':'school-results';host.innerHTML=page.length?(ui.layout==='cards'?page.map(item=>card(item,'catalog')).join(''):table(page)):'<p class="school-muted">没有符合条件的院校</p>';
        for(const button of el('schoolCatalogPanel').querySelectorAll('[data-school-layout]'))button.setAttribute('aria-pressed',String(button.dataset.schoolLayout===ui.layout));
    }
    function find(kind,id){return kind==='catalog'?catalogRecord(id):(state.schools||[]).find(item=>item.id===id);}
    function showDetail(kind,id){
        const record=find(kind,id);if(!record)return;detail={kind,id};el('schoolDetailTitle').textContent=record.name;
        el('schoolDetailContent').innerHTML=`<p class="school-location">${esc(location(record))}${record.schoolCode?' · 学校代码 '+esc(record.schoolCode):''}</p>${badges(record)}<p class="school-scope">${esc(scope(record))}</p><section><h4>招生信息</h4>${facts(record)}</section>${historyMarkup(record)}${record.disciplines?.length?`<section><h4>双一流建设学科</h4><p>${esc(record.disciplines.join('、'))}</p></section>`:''}<section class="school-history"><details><summary>完整备注与信息来源</summary><p class="school-full-notes">${esc(record.formula||'待补充')}</p>${sourceMarkup(record)}</details></section>`;
        const add=el('schoolDetailAdd');add.hidden=kind!=='catalog';add.textContent=targetFor(record)?'打开目标院校':'加入目标院校';add.onclick=()=>targetFor(record)?openTarget(record.id):addTarget(record.id);
        el('schoolDetailEdit').onclick=()=>{el('schoolDetailDialog').close();edit(kind,id);};el('schoolDetailDialog').showModal();
    }
    function populate(record){
        const form=el('schoolForm');for(const key of ['name','ratio','preWeight','reWeight','admissionAverage','preSubjects','reSubjects','formula','province','city','program','enrollmentYear'])form.elements[key].value=record[key]||'';
        form.elements.disciplines.value=(record.disciplines||[]).join('、');form.elements.sourceLinks.value=sources(record).map(item=>item.url).join('\n');
    }
    const previousOpen=openSchoolDialog;
    function edit(kind,id){
        const record=find(kind,id)||{};editor={kind,id:id||'',base:clone(record)};
        const dialog=el('schoolDialog');dialog.dataset.schoolMode=kind;previousOpen(kind==='targets'?id:undefined);populate(record);el('schoolForm').elements.id.value=id||'';
        dialog.querySelector('h3').textContent=kind==='catalog'?'院校库资料':'目标院校';el('schoolForm').querySelector('[type=submit]').textContent='继续并保存';
    }
    openSchoolDialog=id=>edit('targets',id);
    window.editSchool=id=>edit('targets',id);
    async function submit(event){
        event.preventDefault();if(!editor)return;const fd=new FormData(event.target),name=String(fd.get('name')||'').trim();if(!name)return;
        const current=editor.id?find(editor.kind,editor.id):{};if(JSON.stringify(current||{})!==JSON.stringify(editor.base)){PrepLocal.toast('该院校已在另一端变化，请重新打开后修改');return;}
        const links=String(fd.get('sourceLinks')||'').split(/\s+/).filter(Boolean),invalid=links.find(url=>!safeURL(url));if(invalid){PrepLocal.toast('来源链接只支持完整的 http 或 https 地址');return;}
        const oldSources=new Map(sources(editor.base).map(item=>[item.url,item]));
        let value={...editor.base,id:editor.id||uid(),name,updatedAt:now()};
        for(const key of ['ratio','preWeight','reWeight','admissionAverage','preSubjects','reSubjects','formula','province','city','program','enrollmentYear'])value[key]=String(fd.get(key)||'').trim();
        value.disciplines=String(fd.get('disciplines')||'').split(/[,，、;；\n]/).map(item=>item.trim()).filter(Boolean);
        const verificationFields=['name','enrollmentYear','program','preSubjects','reSubjects','ratio','preWeight','reWeight','admissionAverage','province','city'];
        if(verificationFields.some(key=>String(value[key]||'')!==String(editor.base[key]||''))||JSON.stringify(value.disciplines)!==JSON.stringify(editor.base.disciplines||[]))value.personalEdit=true;
        value.sources=[...new Set(links.map(safeURL))].map(url=>oldSources.get(url)||{title:'补充来源',url,checkedAt:PrepLocal.dateText(PrepLocal.nowDate())});
        if(JSON.stringify(value.sources.map(item=>item.url))!==JSON.stringify(sources(editor.base).map(item=>item.url)))value.personalEdit=true;
        value=withSources(value);
        if(editor.kind==='catalog'&&records().some(item=>item.id!==value.id&&!item.hidden&&normalize(item.name)===normalize(name))){PrepLocal.toast('院校库中已存在同名院校');return;}
        const target=editor.kind==='catalog'?(state[collection]||=[]):(state.schools||=[]),index=target.findIndex(item=>item.id===value.id);if(index<0)target.push(value);else target[index]=value;
        saveState();el('schoolDialog').close();await persist();PrepLocal.toast(PrepLocal.hasPending()?'修改已保留，等待同步':'院校资料已保存');
    }
    async function addTarget(id){
        const record=catalogRecord(id);if(!record||record.hidden)return;if(targetFor(record)){PrepLocal.toast('该院校已在目标列表中，没有重复添加');return;}
        const value=withSources({...clone(record),id:uid(),catalogId:record.id,catalogVersion:seed?.version||'',createdAt:now(),updatedAt:now()});delete value.hidden;(state.schools||=[]).push(value);saveState();await persist();
        if(detail?.id===id&&el('schoolDetailDialog').open){el('schoolDetailAdd').textContent='打开目标院校';el('schoolDetailAdd').onclick=()=>openTarget(id);}
        PrepLocal.toast(PrepLocal.hasPending()?'已加入目标，等待同步':'已加入目标院校');
    }
    function openTarget(id){const value=catalogRecord(id),target=value&&targetFor(value);if(!target)return;el('schoolDetailDialog').close();selectTab('targets');edit('targets',target.id);}
    async function remove(kind,id){
        const value=find(kind,id);if(!value)return;
        if(!confirm(kind==='catalog'?`将“${value.name}”移出院校库？可在“已隐藏”中恢复，已加入的目标不会删除。`:`将“${value.name}”移出目标院校？院校库资料不受影响。`))return;
        if(kind==='catalog'){const list=state[collection]||=[],existing=list.find(item=>item.id===id);if(existing){existing.hidden=true;existing.updatedAt=now();}else list.push({...clone(value),hidden:true,updatedAt:now()});}
        else state.schools=state.schools.filter(item=>item.id!==id);
        saveState();await persist();
    }
    async function restore(id){const value=(state[collection]||[]).find(item=>item.id===id);if(value){value.hidden=false;value.updatedAt=now();await persist();}}
    window.deleteSchool=id=>remove('targets',id);
    function exportCatalog(){const value={format:'yantu-school-catalog-v1',version:seed?.version||'',title:seed?.title||'个人院校库',publishedAt:seed?.publishedAt||'',checkedAt:seed?.checkedAt||'',coverage:seed?.coverage||'',metricsNote:seed?.metricsNote||'',sources:seed?.sources||[],schools:records().filter(item=>!item.hidden).map(withSources)};return downloadBlob('研途-院校库.json',new Blob([JSON.stringify(value,null,2)],{type:'application/json'}));}
    function action(event){
        const tab=event.target.closest('[data-school-tab]');if(tab){selectTab(tab.dataset.schoolTab).catch(error=>PrepLocal.toast(error.message));return;}
        const layout=event.target.closest('[data-school-layout]');if(layout){ui.layout=layout.dataset.schoolLayout;remember();render();return;}
        const button=event.target.closest('[data-school-action]');if(!button)return;const {schoolAction:command,kind,id}=button.dataset;
        Promise.resolve().then(()=>{
            if(command==='detail')showDetail(kind,id);else if(command==='edit')edit(kind,id);else if(command==='remove')return remove(kind,id);else if(command==='restore')return restore(id);else if(command==='add-target')return addTarget(id);else if(command==='target')openTarget(id);else if(command==='new')edit('catalog');else if(command==='export')exportCatalog();else if(command==='retry')return render();
            else if(command==='reset'){Object.assign(ui,{q:'',province:'',year:'',evidence:'',status:'',page:1});el('schoolCatalogSearch').value='';el('schoolCatalogStatus').value='';el('schoolEvidenceFilter').value='';return render();}
            else if(['first','previous','next','last'].includes(command)){const pages=Math.max(1,Math.ceil(results().length/ui.pageSize));ui.page=command==='first'?1:command==='last'?pages:Math.max(1,Math.min(pages,ui.page+(command==='next'?1:-1)));return render();}
        }).catch(error=>PrepLocal.toast(error.message));
    }
    renderSchools=()=>{
        if(!initialized)return;const query=el('schoolTargetSearch').value.trim().toLocaleLowerCase(),items=(state.schools||[]).filter(item=>!query||[item.name,item.province,item.city,item.program].join(' ').toLocaleLowerCase().includes(query));
        el('schoolList').className='school-grid';el('schoolList').innerHTML=items.length?items.map(item=>card(item,'targets')).join(''):'<p class="school-muted">暂无符合条件的目标院校</p>';tabs();window.StudioUI?.updateWorkspace();
    };
    function setup(){
        if(initialized)return;initialized=true;state[collection]||=[];
        const close=el('schoolDialog').querySelector('.dialog-header button');if(close){close.title='返回';close.setAttribute('aria-label','返回院校列表');}
        const section=el('view-schools'),target=section.querySelector('.card');target.id='schoolTargetPanel';
        const tabsElement=document.createElement('div');tabsElement.id='schoolBrowserTabs';tabsElement.className='resource-library-tabs school-browser-tabs';tabsElement.setAttribute('role','tablist');tabsElement.innerHTML='<button role="tab" data-school-tab="targets"><i class="fas fa-star"></i> 我的目标 <span id="schoolTargetCount" class="library-tab-count"></span></button><button role="tab" data-school-tab="catalog"><i class="fas fa-building-columns"></i> 院校库 <span id="schoolCatalogCount" class="library-tab-count"></span></button>';section.prepend(tabsElement);
        const search=document.createElement('label');search.className='library-search school-target-search';search.innerHTML='<i class="fas fa-search" aria-hidden="true"></i><input id="schoolTargetSearch" type="search" placeholder="搜索目标院校" aria-label="搜索目标院校" />';el('schoolList').before(search);search.addEventListener('input',()=>{clearTimeout(searchTimer);searchTimer=setTimeout(renderSchools,120);});
        const panel=document.createElement('div');panel.id='schoolCatalogPanel';panel.className='school-catalog-panel';
        panel.innerHTML=`<div class="school-catalog-heading"><h3>双一流 · 408院校库</h3><div class="school-row-actions"><button class="btn btn-ghost" data-school-action="export"><i class="fas fa-download"></i> 导出</button><button class="btn btn-primary" data-school-action="new"><i class="fas fa-plus"></i> 新增院校</button></div></div>
            <div class="school-catalog-toolbar"><label class="library-search"><i class="fas fa-search" aria-hidden="true"></i><input id="schoolCatalogSearch" type="search" placeholder="院校、专业或考试科目" aria-label="搜索院校库" /></label>
            <select id="schoolProvinceFilter" aria-label="院校所在地区"></select><select id="schoolYearFilter" aria-label="招生年度"></select>
            <select id="schoolEvidenceFilter" aria-label="信息核验状态"><option value="">所有资料状态</option><option value="directory">目录 / 简章已核对</option><option value="notice">官方答疑已核对</option><option value="adjustment">调整公告待目录</option><option value="personal">个人补充或修改</option></select>
            <select id="schoolCatalogStatus" aria-label="目标添加状态"><option value="">所有院校</option><option value="unadded">尚未加入目标</option><option value="added">已加入目标</option><option value="hidden">已隐藏</option></select>
            <select id="schoolCatalogSort" aria-label="院校排序"><option value="year">招生年度从新到旧</option><option value="name">按名称</option><option value="province">按地区</option><option value="updated">最近修改</option></select>
            <div class="library-layouts" role="group" aria-label="院校显示方式"><button class="btn btn-icon btn-ghost" data-school-layout="table" title="列表" aria-label="院校列表视图"><i class="fas fa-list"></i></button><button class="btn btn-icon btn-ghost" data-school-layout="cards" title="卡片" aria-label="院校卡片视图"><i class="fas fa-table-cells-large"></i></button></div><button class="btn btn-icon btn-ghost" data-school-action="reset" title="清除院校筛选" aria-label="清除院校筛选"><i class="fas fa-filter-circle-xmark"></i></button></div>
            <p id="schoolCatalogMeta" class="school-muted" role="status"></p><p id="schoolCatalogCoverage" class="school-coverage"></p><nav class="library-page-top school-pages" aria-label="院校库顶部翻页">${pagerHTML()}</nav><div id="schoolCatalogResults"></div><nav class="library-pager school-pages" aria-label="院校库底部翻页">${pagerHTML()}</nav>`;
        section.append(panel);section.addEventListener('click',action);
        el('schoolCatalogSearch').addEventListener('input',event=>{ui.q=event.target.value;ui.page=1;clearTimeout(searchTimer);searchTimer=setTimeout(render,150);});
        for(const [id,field]of [['schoolProvinceFilter','province'],['schoolYearFilter','year'],['schoolEvidenceFilter','evidence'],['schoolCatalogStatus','status'],['schoolCatalogSort','sort']])el(id).addEventListener('change',event=>{ui[field]=event.target.value;ui.page=1;remember();render();});
        el('schoolCatalogSort').value=ui.sort;
        panel.addEventListener('change',event=>{if(event.target.hasAttribute('data-school-page')){ui.page=Number(event.target.value)||1;render();}else if(event.target.hasAttribute('data-school-size')){const size=Number(event.target.value);if([24,48,96].includes(size)){ui.page=Math.floor((ui.page-1)*ui.pageSize/size)+1;ui.pageSize=size;remember();render();}}});
        const extra=document.createElement('section');extra.className='school-editor-extra';extra.innerHTML='<label>往年录取平均分<textarea name="admissionAverage" rows="2" placeholder="年份、专业、初试或综合分、满分和样本人数"></textarea></label><div class="grid-2"><label>地区<input name="province" /></label><label>城市<input name="city" /></label><label>招生年度<input name="enrollmentYear" inputmode="numeric" placeholder="如 2027，留空待核验" /></label><label>学院 / 专业 / 方向<input name="program" /></label></div><label>双一流建设学科<textarea name="disciplines" rows="2"></textarea></label><label>信息来源链接<textarea name="sourceLinks" rows="3" placeholder="每行一个完整网页链接"></textarea></label>';el('schoolForm').querySelector('[name=formula]').previousElementSibling.before(extra);el('schoolForm').onsubmit=event=>submit(event).catch(error=>PrepLocal.toast(error.message));
        const dialog=document.createElement('dialog');dialog.id='schoolDetailDialog';dialog.className='workspace-dialog school-detail-dialog';dialog.innerHTML='<header class="dialog-header"><button class="btn btn-icon btn-ghost" title="返回" aria-label="返回院校列表" onclick="this.closest(\'dialog\').close()"><i class="fas fa-arrow-left"></i></button><h3 id="schoolDetailTitle"></h3><button class="btn btn-icon btn-ghost" id="schoolDetailEdit" title="编辑院校资料" aria-label="编辑院校资料"><i class="fas fa-pen"></i></button></header><div class="dialog-body" id="schoolDetailContent"></div><footer class="dialog-actions"><button class="btn btn-ghost" onclick="this.closest(\'dialog\').close()">退回</button><button class="btn btn-primary" id="schoolDetailAdd">加入目标院校</button></footer>';document.body.append(dialog);
        renderSchools();tabs();if(document.body.dataset.view==='schools')selectTab(ui.tab).catch(error=>PrepLocal.toast(error.message));
    }
    const previousRender=renderView;renderView=id=>{previousRender(id);if(id==='schools'&&initialized)selectTab(ui.tab).catch(error=>PrepLocal.toast(error.message));};
    addEventListener('workspace-ready',setup,{once:true});
    addEventListener('workspace-change',()=>{if(initialized&&document.body.dataset.view==='schools'){renderSchools();render();}});
    addEventListener('workspace-open-record',event=>{if(event.detail.collection===collection){W.navigate('schools');selectTab('catalog').then(()=>showDetail('catalog',event.detail.record.id));}});
    window.SchoolLibrary={tab:selectTab,addTarget,load,records,withSources,safeURL};
})();
