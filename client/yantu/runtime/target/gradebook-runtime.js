(() => {
    const el=id=>document.getElementById(id);
    const esc=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const subjects=[['数学','math',150],['英语','english',100],['政治','politics',100],['408','major',150]];
    const filters={q:'',from:'',to:'',paperId:''};
    let comparison={a:'',b:''},timer;
    const sorted=records=>[...records].sort((a,b)=>String(a.date||'').localeCompare(String(b.date||'')));
    function selected(){return (state.scores||[]).filter(score=>(!filters.q||[score.name,score.comment,state.papers.find(paper=>paper.id===score.paperId)?.title].join(' ').toLocaleLowerCase().includes(filters.q.toLocaleLowerCase()))&&(!filters.from||score.date>=filters.from)&&(!filters.to||score.date<=filters.to)&&(!filters.paperId||score.paperId===filters.paperId));}
    function paperOptions(current){const rows=(state.papers||[]).map(paper=>`<option value="${esc(paper.id)}">${esc(paper.title)}</option>`);if(current && !state.papers.some(paper=>paper.id===current))rows.push(`<option value="${esc(current)}">已删除的试卷</option>`);return '<option value="">未关联试卷</option>'+rows.join('');}
    function scoreOptions(records){return '<option value="">选择考试</option>'+records.map(score=>`<option value="${esc(score.id)}">${esc(score.date)} · ${esc(score.name)}</option>`).join('');}
    function compare(records){
        const ordered=sorted(records),ids=new Set(ordered.map(score=>score.id));
        if(!ids.has(comparison.b))comparison.b=ordered.at(-1)?.id||'';
        if(!ids.has(comparison.a))comparison.a=ordered.at(-2)?.id||'';
        for(const key of ['a','b']){const input=el('scoreCompare'+key.toUpperCase());input.innerHTML=scoreOptions(ordered);input.value=comparison[key];}
        const a=records.find(score=>score.id===comparison.a),b=records.find(score=>score.id===comparison.b);
        el('scoreComparisonBody').innerHTML='<table class="grade-comparison"><thead><tr><th>科目</th><th>基准</th><th>本次</th><th>变化</th></tr></thead><tbody>'+[...subjects,['总分','total',500]].map(([name,key])=>{
            const av=a?(key==='total'?(scoreComplete(a)?scoreTotal(a):null):scoreValue(a[key])):null,bv=b?(key==='total'?(scoreComplete(b)?scoreTotal(b):null):scoreValue(b[key])):null;
            const change=av!==null&&bv!==null?Number((bv-av).toFixed(2)):null;
            return `<tr><th>${name}</th><td>${av??'--'}</td><td>${bv??'--'}</td><td class="${change>0?'grade-up':change<0?'grade-down':''}">${change===null?'--':`${change>0?'+':''}${change}`}</td></tr>`;
        }).join('')+'</tbody></table>';
    }
    function renderHistory(records){
        el('scoreList').innerHTML=records.length?`<div class="grade-history-wrap"><table class="grade-history"><thead><tr><th>考试</th><th>总分</th>${subjects.map(([name])=>`<th class="grade-subject-cell">${name}</th>`).join('')}<th></th></tr></thead><tbody>${sorted(records).reverse().map(score=>`<tr><td><strong>${esc(score.name)}</strong><time>${esc(score.date)}</time>${score.paperId?`<span class="grade-paper-link">${esc(state.papers.find(paper=>paper.id===score.paperId)?.title||'已删除的试卷')}</span>`:''}${score.comment?`<p>${esc(score.comment)}</p>`:''}</td><td class="grade-total">${scoreComplete(score)?scoreTotal(score):`--<small>已录 ${scoreTotal(score)}<br>未录全</small>`}</td>${subjects.map(([,key])=>`<td class="grade-subject-cell">${scoreValue(score[key])??'--'}</td>`).join('')}<td><div class="grade-row-actions"><button class="btn btn-icon btn-ghost" onclick="openScoreDialog('${score.id}')" title="编辑成绩" aria-label="编辑成绩"><i class="fas fa-pen"></i></button><button class="btn btn-icon btn-danger" onclick="deleteScore('${score.id}')" title="删除成绩" aria-label="删除成绩"><i class="fas fa-trash"></i></button></div></td></tr>`).join('')}</tbody></table></div>`:'<div class="empty-state"><i class="fas fa-chart-line"></i><p>暂无符合条件的成绩</p></div>';
    }
    function insights(records){
        const totals=records.filter(scoreComplete).map(scoreTotal),average=totals.length?Number((totals.reduce((a,b)=>a+b,0)/totals.length).toFixed(1)):'--';
        const ratios=subjects.map(([name,key,max])=>{const values=records.map(score=>scoreValue(score[key])).filter(value=>value!==null);return {name,ratio:values.length?values.reduce((a,b)=>a+b,0)/values.length/max:null};}).filter(item=>item.ratio!==null).sort((a,b)=>a.ratio-b.ratio);
        el('gradeStatistics').innerHTML=`<span>完整考试 <strong>${totals.length}</strong></span><span>平均总分 <strong>${average}</strong></span><span>最高总分 <strong>${totals.length?Math.max(...totals):'--'}</strong></span>${ratios.length?`<span>平均得分率最低 <strong>${ratios[0].name} ${(ratios[0].ratio*100).toFixed(1)}%</strong></span>`:''}`;
        el('scoreResultCount').textContent=`${records.length} / ${(state.scores||[]).length} 次考试`;
        el('scoreCSV').disabled=!records.length;
    }
    const previousScores=renderScores;
    renderScores=()=>{const records=selected();previousScores(records);if(el('gradeStatistics')){insights(records);compare(records);renderHistory(records);const input=el('scorePaperFilter'),value=filters.paperId;input.innerHTML='<option value="">所有试卷</option>'+paperOptions(value).replace('<option value="">未关联试卷</option>','');input.value=value;}};
    const previousOpen=openScoreDialog;
    openScoreDialog=id=>{
        previousOpen(id);const record=state.scores.find(score=>score.id===id),form=el('scoreForm');
        form.elements.paperId.innerHTML=paperOptions(record?.paperId);form.elements.paperId.value=record?.paperId||'';form.elements.comment.value=record?.comment||'';
        el('scoreDialog').querySelector('h3').textContent=id?'编辑成绩':'录入成绩';
    };
    function forPaper(id){const paper=state.papers.find(paper=>paper.id===id);if(!paper)return;openScoreDialog();const form=el('scoreForm');form.elements.paperId.value=id;form.elements.name.value=paper.title+'（练习）';}
    function exportCSV(){
        const records=sorted(selected());
        const cell=value=>{let text=String(value??'');if(typeof value!=='number'&&/^\s*[=+\-@]/.test(text))text="'"+text;return '"'+text.replaceAll('"','""')+'"';};
        const rows=[['日期','考试名称','数学','英语','政治','408','总分','已录分数','记录类型','关联试卷','备注'],...records.map(score=>[score.date,score.name,...subjects.map(([,key])=>scoreValue(score[key])??''),scoreComplete(score)?scoreTotal(score):'',scoreTotal(score),scoreComplete(score)?'完整':'未录全',state.papers.find(paper=>paper.id===score.paperId)?.title||'',score.comment||''])];
        downloadBlob(`研途成绩-${PrepLocal.dateText(PrepLocal.nowDate())}.csv`,new Blob(['\ufeff'+rows.map(row=>row.map(cell).join(',')).join('\r\n')],{type:'text/csv;charset=utf-8'}));
    }
    function setup(){
        const toolbar=document.createElement('div');toolbar.className='grade-filters';toolbar.innerHTML='<label class="library-search"><i class="fas fa-search"></i><input type="search" id="scoreSearch" placeholder="搜索考试" aria-label="搜索考试" /></label><label>开始<input type="date" id="scoreFrom" /></label><label>结束<input type="date" id="scoreTo" /></label><select id="scorePaperFilter" aria-label="关联试卷"></select><button class="btn btn-icon btn-ghost" id="scoreResetFilters" title="清除筛选" aria-label="清除筛选"><i class="fas fa-filter-circle-xmark"></i></button><button class="btn btn-ghost" id="scoreCSV"><i class="fas fa-download"></i> CSV</button>';
        el('scoreInsights').before(toolbar);
        const stats=document.createElement('div');stats.id='gradeStatistics';stats.className='grade-statistics';el('scoreInsights').after(stats);
        const comparisonPanel=document.createElement('section');comparisonPanel.className='grade-comparison-section';comparisonPanel.innerHTML='<header><h4><i class="fas fa-code-compare"></i> 两次考试对比</h4><span id="scoreResultCount"></span></header><div class="grade-compare-inputs"><label>基准考试<select id="scoreCompareA"></select></label><label>对比考试<select id="scoreCompareB"></select></label></div><div id="scoreComparisonBody"></div>';el('scoreList').before(comparisonPanel);
        const extra=document.createElement('div');extra.className='record-extra-fields';extra.innerHTML='<label>关联试卷<select name="paperId"></select></label><label>考试备注<textarea name="comment" rows="2"></textarea></label>';el('scoreForm').lastElementChild.before(extra);
        el('scoreSearch').oninput=event=>{filters.q=event.target.value;clearTimeout(timer);timer=setTimeout(renderScores,150);};
        for(const [id,key] of [['scoreFrom','from'],['scoreTo','to'],['scorePaperFilter','paperId']])el(id).onchange=event=>{filters[key]=event.target.value;renderScores();};
        el('scoreResetFilters').onclick=()=>{Object.assign(filters,{q:'',from:'',to:'',paperId:''});el('scoreSearch').value='';el('scoreFrom').value='';el('scoreTo').value='';renderScores();};
        for(const key of ['a','b'])el('scoreCompare'+key.toUpperCase()).onchange=event=>{comparison[key]=event.target.value;compare(selected());};el('scoreCSV').onclick=exportCSV;
        el('scoreForm').onsubmit=async event=>{
            event.preventDefault();const form=event.target;if(!form.reportValidity())return;const fd=new FormData(form),old=state.scores.find(score=>score.id===fd.get('id'));
            if(!subjects.some(([,key])=>scoreValue(fd.get(key))!==null)){PrepLocal.toast('请填写至少一科成绩');return;}
            const data={...old,...Object.fromEntries(fd),id:fd.get('id')||uid(),updatedAt:PrepLocal.nowDate().toISOString()};const button=form.querySelector('[type=submit]');button.disabled=true;
            try {const index=state.scores.findIndex(score=>score.id===data.id);if(index<0)state.scores.push(data);else state.scores[index]=data;saveState();await PrepLocal.flush();el('scoreDialog').close();renderScores();window.StudyLibrary?.refresh('papers');PrepLocal.toast(PrepLocal.hasPending()?'成绩已保留，等待同步':'成绩已保存');}catch(error){PrepLocal.toast(error.message);}finally{button.disabled=false;}
        };
    }
    const previousReady=PrepLocal.ready;
    PrepLocal.ready=()=>{setup();previousReady();};
    window.GradeBook={forPaper,exportCSV};
})();
