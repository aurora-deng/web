/* Load after gradebook-runtime.js and workspace-core.js. Legacy helpers stay intact. */
(() => {
    'use strict';
    const defaults = Object.freeze([
        {id:'subject-math',name:'数学',maximum:150,legacyKey:'math'},
        {id:'subject-english',name:'英语',maximum:100,legacyKey:'english'},
        {id:'subject-politics',name:'政治',maximum:100,legacyKey:'politics'},
        {id:'subject-major',name:'408',maximum:150,legacyKey:'major'}
    ]);
    const numeric = value => value === '' || value == null || typeof value === 'boolean' || !Number.isFinite(Number(value)) || Number(value)<0 ? null : Number(value);
    function definitions(data) {
        const records = data.subjects || [], merged = defaults.map(item => records.find(subject => subject.legacyKey === item.legacyKey || subject.id === item.id) || item);
        return [...new Map([...merged,...records.filter(item => !item.legacyKey && !defaults.some(base=>base.id===item.id))].map(item=>[item.id,item])).values()].filter(item => !item.archived).map(item => ({...item}));
    }
    function snapshot(score) { return Array.isArray(score.subjectSnapshot) && score.subjectSnapshot.length ? score.subjectSnapshot : defaults; }
    function value(score, subject) { return numeric(Array.isArray(score.subjectSnapshot)&&score.subjectSnapshot.length ? score.subjectScores?.[subject.id] : score[subject.legacyKey]); }
    function summarize(score) {
        const subjects = snapshot(score), entries = subjects.map(subject => ({...subject,value:value(score,subject)}));
        const recorded = entries.filter(item => item.value !== null), complete = recorded.length === subjects.length;
        return {entries,complete,recorded:recorded.length,denominator:subjects.length,
            total:Number(recorded.reduce((sum,item)=>sum+item.value,0).toFixed(2)),maximum:subjects.reduce((sum,item)=>sum+item.maximum,0)};
    }
    function comparable(a,b) {
        return Boolean(a?.paperId && a.paperId === b?.paperId && JSON.stringify(snapshot(a).map(item=>[item.id,item.maximum])) === JSON.stringify(snapshot(b).map(item=>[item.id,item.maximum])));
    }
    function csv(records) {
        const cell = input => {let text=String(input??'');if(typeof input!=='number'&&/^\s*[=+\-@]/.test(text))text="'"+text;return '"'+text.replaceAll('"','""')+'"';};
        const rows=[['日期','考试','科目','得分','当时满分','当时目标','关联试卷','备注']];
        for(const score of records)for(const item of summarize(score).entries)rows.push([score.date,score.name,item.name,item.value??'',item.maximum,item.target??'',score.paperId||'',score.comment||'']);
        return '\ufeff'+rows.map(row=>row.map(cell).join(',')).join('\r\n');
    }
    const api={defaults,definitions,snapshot,value,summarize,comparable,csv};
    globalThis.PlanningScores=api;
    if(typeof document==='undefined')return;
    let W,host,editor,filters={q:'',from:'',to:'',paperId:''},choice={a:'',b:''},page=0,charts=[],opening=0,scoreSaving=false;
    const esc = text => W.esc(text);
    function selected(){return (state.scores||[]).filter(item=>(!filters.q||[item.name,item.comment,state.papers?.find(paper=>paper.id===item.paperId)?.title].join(' ').toLowerCase().includes(filters.q.toLowerCase()))&&(!filters.from||item.date>=filters.from)&&(!filters.to||item.date<=filters.to)&&(!filters.paperId||item.paperId===filters.paperId)).sort((a,b)=>String(b.date).localeCompare(String(a.date)));}
    function drawCharts(records){
        charts.forEach(chart=>chart.destroy());charts=[];if(!window.Chart)return;
        const ordered=records.slice().reverse(),groups=new Map(),colors=['#177e61','#276bb3','#a84d4d','#765c9b','#827213','#497b84'];
        for(const score of ordered){const key=JSON.stringify(snapshot(score).map(item=>[item.id,item.maximum]));if(!groups.has(key))groups.set(key,{label:'总分 / '+summarize(score).maximum,key});}
        const datasets=[...groups.values()].map((group,n)=>({label:group.label+' · 科目组 '+(n+1),data:ordered.map(score=>JSON.stringify(snapshot(score).map(item=>[item.id,item.maximum]))===group.key&&summarize(score).complete?summarize(score).total:null),borderColor:colors[n%colors.length],backgroundColor:colors[n%colors.length],spanGaps:false,tension:0}));
        const subjectGroups=new Map();for(const score of ordered)for(const subject of snapshot(score)){const key=subject.id+'|'+subject.maximum;if(!subjectGroups.has(key))subjectGroups.set(key,subject);}
        const perSubject=[...subjectGroups.values()].map((subject,n)=>({label:subject.name+' / '+subject.maximum,data:ordered.map(score=>{const own=snapshot(score).find(item=>item.id===subject.id&&item.maximum===subject.maximum);return own?value(score,own):null;}),borderColor:colors[n%colors.length],backgroundColor:colors[n%colors.length],spanGaps:false,tension:0}));
        for(const[id,data]of[['planningTotalTrend',datasets],['planningSubjectTrend',perSubject]])charts.push(new Chart(host.querySelector('#'+id),{type:'line',data:{labels:ordered.map(score=>score.date+' '+score.name),datasets:data},options:{responsive:true,maintainAspectRatio:false,animation:false,scales:{y:{beginAtZero:true}},plugins:{legend:{position:'bottom'}}}}));
    }
    function render(){
        if(!host)return;
        const records=selected(),list=host.querySelector('[data-score-list]');list.replaceChildren();
        const complete=records.filter(item=>summarize(item).complete).length;
        host.querySelector('[data-score-count]').textContent=`筛选 ${records.length} / ${(state.scores||[]).length} 次；录全 ${complete} / ${records.length} 次。各次使用当时满分，不合并不同试卷的均分。`;
        page=Math.max(0,Math.min(page,Math.ceil(records.length/50)-1));
        for(const score of records.slice(page*50,page*50+50)){
            const data=summarize(score),row=document.createElement('div');row.className='planning-row planning-score-row';
            row.innerHTML=`<div><strong>${esc(score.name)}</strong><small>${esc(score.date)} · ${data.complete?'总分 '+data.total+' / '+data.maximum:'未录全 '+data.recorded+' / '+data.denominator+' 科'}${score.paperId?' · '+esc(state.papers?.find(item=>item.id===score.paperId)?.title||'试卷已删除'):''}</small><div class="planning-score-values">${data.entries.map(item=>`<span>${esc(item.name)} <b>${item.value??'未录'} / ${item.maximum}</b></span>`).join('')}</div><small>${esc(score.comment||'')}</small></div>`;
            const actions=document.createElement('div');actions.className='planning-actions';actions.append(W.icon('编辑成绩','pen',()=>open(score.id)),W.icon('移入回收站','trash',async()=>{await W.remove('scores',score.id);render();}));row.append(actions);list.append(row);
        }
        if(!records.length)list.textContent='暂无符合条件的成绩';
        const pager=host.querySelector('[data-score-pager]');pager.replaceChildren(W.button('上一页','chevron-left',()=>{page--;render();}),document.createTextNode(' '+(page+1)+' / '+Math.max(1,Math.ceil(records.length/50))+' '),W.button('下一页','chevron-right',()=>{page++;render();}));pager.firstElementChild.disabled=page===0;pager.lastElementChild.disabled=(page+1)*50>=records.length;
        const paper=host.querySelector('[data-score-filter=paperId]');paper.innerHTML='<option value="">所有试卷</option>'+[...new Set([...(state.papers||[]).map(item=>item.id),...state.scores.map(item=>item.paperId).filter(Boolean)])].map(id=>`<option value="${esc(id)}">${esc(state.papers.find(item=>item.id===id)?.title||'已删除的试卷')}</option>`).join('');paper.value=filters.paperId;
        drawCharts(records);
        for(const key of ['a','b']){const select=host.querySelector('[data-compare='+key+']');select.innerHTML='<option value="">选择考试</option>'+records.map(score=>`<option value="${esc(score.id)}">${esc(score.date+' '+score.name)}</option>`).join('');select.value=choice[key];}
        compare();
    }
    function compare(){
        const a=state.scores.find(item=>item.id===choice.a),b=state.scores.find(item=>item.id===choice.b),area=host.querySelector('[data-score-comparison]');
        if(!a||!b){area.textContent='选择两次考试后查看原始分数。';return;}
        const safe=comparable(a,b),ad=summarize(a),bd=summarize(b);
        area.innerHTML=`<p>${safe?'同一关联试卷、同满分，仅比较本次记录；重复练习可能有熟悉效应。':'试卷或满分不同，不直接计算进退；难度未校准。'}</p><div class="planning-table-scroll"><table><thead><tr><th>科目</th><th>基准（得分/满分）</th><th>本次（得分/满分）</th><th>差值</th></tr></thead><tbody>${[...new Set([...ad.entries,...bd.entries].map(item=>item.id))].map(id=>{const x=ad.entries.find(item=>item.id===id),y=bd.entries.find(item=>item.id===id);return `<tr><th>${esc(y?.name||x?.name)}</th><td>${x?`${x.value??'未录'} / ${x.maximum}`:'未设置'}</td><td>${y?`${y.value??'未录'} / ${y.maximum}`:'未设置'}</td><td>${safe&&x?.value!=null&&y?.value!=null?Number((y.value-x.value).toFixed(2)):'不比较'}</td></tr>`;}).join('')}</tbody></table></div>`;
    }
    async function open(id,initial={}){
        if(!W)return;if(scoreSaving){PrepLocal.toast('成绩正在保存，请稍后');return;}const request=++opening;
        const old=state.scores.find(item=>item.id===id),subjects=old?snapshot(old):initial.subjectSnapshot||definitions(state),newId=id||uid();
        if(!subjects.length){PrepLocal.toast('请先启用至少一个科目');return;}
        editor?.remove();
        editor=W.dialog('planningScoreDialog',old?'编辑成绩':'录入成绩',`<form id="planningScoreForm"><label>考试名称<input name="name" required maxlength="200" value="${esc(old?.name||initial.name||'')}" /></label><label>日期<input type="date" name="date" required value="${esc(old?.date||initial.date||PrepLocal.dateText(PrepLocal.nowDate()))}" /></label><div class="planning-fields">${subjects.map(subject=>`<label>${esc(subject.name)} / ${subject.maximum}<input type="number" name="score-${esc(subject.id)}" min="0" max="${subject.maximum}" step="0.01" value="${old?value(old,subject)??'':numeric(initial.subjectScores?.[subject.id])??''}" /></label>`).join('')}</div><label>关联试卷<select name="paperId"><option value="">未关联试卷</option>${(state.papers||[]).map(paper=>`<option value="${esc(paper.id)}">${esc(paper.title)}</option>`).join('')}</select></label><label>备注<textarea name="comment" rows="2">${esc(old?.comment||initial.comment||'')}</textarea></label><div class="dialog-actions"><button type="button" class="btn btn-ghost" data-return>返回</button><button class="btn btn-primary" type="submit"><i class="fas fa-save" aria-hidden="true"></i> 保存</button></div><p data-save-status role="status"></p></form>`);
        const dialog=editor;dialog.classList.add('planning-dialog');const form=dialog.querySelector('form'),baseline=JSON.stringify(old||null),close=dialog.close.bind(dialog);dialog.close=(...args)=>{if(!scoreSaving)close(...args);};
        const paperId=old?.paperId||initial.paperId||'';if(paperId&&!state.papers.some(paper=>paper.id===paperId)){const option=document.createElement('option');option.value=paperId;option.textContent='已删除的试卷';form.elements.paperId.append(option);}form.elements.paperId.value=paperId;
        const key='planning-draft-score-'+(id||'new'),draft=await DeviceStore.get(key);
        if(request!==opening)return;const usedDraft=Boolean(draft&&confirm('发现这次成绩的本机草稿，继续编辑？'));if(usedDraft)for(const [name,text]of Object.entries(draft.fields||{}))if(form.elements.namedItem(name))form.elements.namedItem(name).value=text;
        let writes=Promise.resolve(),dirty=false;
        form.oninput=()=>{dirty=true;const fields=Object.fromEntries(new FormData(form));writes=writes.then(()=>DeviceStore.put(key,{fields,at:new Date().toISOString()})).catch(()=>{form.querySelector('[data-save-status]').textContent='本机草稿未写入，存储空间可能不足';});};
        dialog.querySelector('[data-return]').onclick=async()=>{await writes;dialog.close();};
        form.onsubmit=async event=>{event.preventDefault();if(scoreSaving||!form.reportValidity())return;const button=form.querySelector('[type=submit]');button.disabled=true;scoreSaving=true;const controls=[...form.querySelectorAll('input,select,textarea')];controls.forEach(field=>field.disabled=true);
            try{await writes;if(JSON.stringify(state.scores.find(item=>item.id===id)||null)!==baseline)throw new Error('成绩已在其他位置变化，请保留草稿后重新打开');
                const values=Object.fromEntries(subjects.map(subject=>[subject.id,numeric(form.elements.namedItem('score-'+subject.id).value)]));if(!Object.values(values).some(item=>item!==null))throw new Error('至少录入一科实际分数');
                const name=form.elements.name.value.trim();if(!name)throw new Error('考试名称不能为空');const record={...old,id:newId,name,date:form.elements.date.value,paperId:form.elements.paperId.value,comment:form.elements.comment.value,updatedAt:PrepLocal.nowDate().toISOString(),...(initial.examSessionId?{examSessionId:initial.examSessionId}:{}),...(initial.setId?{setId:initial.setId}:{}),...(initial.examCoverage?{examCoverage:structuredClone(initial.examCoverage)}:{})};
                if(old&&!old.subjectSnapshot){for(const subject of defaults)record[subject.legacyKey]=values[subject.id]??'';}
                else{record.subjectSnapshot=subjects.map(({id,name,maximum,target,legacyKey})=>({id,name,maximum,...(target!=null?{target}:{}),...(legacyKey?{legacyKey}:{})}));record.subjectScores=values;}
                await W.put('scores',record);if(usedDraft||dirty)await DeviceStore.put(key,undefined);close();render();PrepLocal.toast(PrepLocal.hasPending()?'成绩已保留，等待同步':'成绩已同步到电脑');
            }catch(error){form.querySelector('[data-save-status]').textContent=error.message;}finally{button.disabled=false;scoreSaving=false;controls.forEach(field=>field.disabled=false);}
        };dialog.showModal();
    }
    function init(){
        if(host||!window.Workspace?.capabilities()?.features?.includes('planning-v1'))return;W=Workspace;
        const view=W.el('view-scores');if(!view)return;
        for(const child of view.children)child.hidden=true;
        host=document.createElement('section');host.className='planning-score-workspace';host.innerHTML='<header class="planning-toolbar"><h2>成绩</h2><div data-score-actions></div></header><div class="planning-fields"><label>搜索考试<input type="search" data-score-filter="q" /></label><label>起始日期<input type="date" data-score-filter="from" /></label><label>结束日期<input type="date" data-score-filter="to" /></label><label>关联试卷<select data-score-filter="paperId"></select></label></div><p data-score-count></p><div class="score-charts"><section class="chart-surface"><h3>总分趋势</h3><div class="chart-plot"><canvas id="planningTotalTrend" role="img" aria-label="完整成绩总分趋势，各满分科目组分别显示"></canvas></div></section><section class="chart-surface"><h3>科目成绩</h3><div class="chart-plot"><canvas id="planningSubjectTrend" role="img" aria-label="科目成绩趋势，按当时满分分别显示"></canvas></div></section></div><div data-score-list></div><nav class="planning-actions" data-score-pager aria-label="成绩列表分页"></nav><h3>两次记录核对</h3><div class="planning-fields"><label>基准考试<select data-compare="a"></select></label><label>本次考试<select data-compare="b"></select></label></div><div data-score-comparison></div>';
        host.querySelector('[data-score-actions]').append(W.button('录入成绩','plus',()=>open()),W.icon('导出筛选成绩 CSV','download',()=>downloadBlob('研途成绩.csv',new Blob([csv(selected())],{type:'text/csv;charset=utf-8'}))));view.append(host);
        host.querySelector('[data-score-actions]').append(W.icon('清除筛选','filter-circle-xmark',()=>{Object.keys(filters).forEach(key=>filters[key]='');host.querySelectorAll('[data-score-filter]').forEach(input=>input.value='');page=0;render();}),W.icon('将筛选成绩移入回收站','trash',async()=>{const rows=selected();if(!rows.length||!confirm('将筛选出的 '+rows.length+' 条成绩移入回收站？'))return;const ids=new Set(rows.map(item=>item.id));state.scores=state.scores.filter(item=>!ids.has(item.id));await W.persist();render();}));
        for(const input of host.querySelectorAll('[data-score-filter]'))input.oninput=()=>{filters[input.dataset.scoreFilter]=input.value;page=0;render();};
        for(const select of host.querySelectorAll('[data-compare]'))select.onchange=()=>{choice[select.dataset.compare]=select.value;compare();};
        openScoreDialog=id=>open(id);renderScores=render;
        scoreTotal=score=>summarize(score).total;scoreComplete=score=>summarize(score).complete;
        if(window.GradeBook){GradeBook.forPaper=id=>{const paper=state.papers.find(item=>item.id===id);if(paper)return open(null,{paperId:id,name:paper.title+'（练习）'});};GradeBook.exportCSV=()=>downloadBlob('研途成绩.csv',new Blob([csv(selected())],{type:'text/csv;charset=utf-8'}));}
        addEventListener('workspace-change',render);render();
    }
    api.init=init;api.open=open;api.refresh=render;
    addEventListener('workspace-ready',init);if(window.Workspace?.ready())init();
})();
