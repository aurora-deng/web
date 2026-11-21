(() => {
    const el=id=>document.getElementById(id);
    const esc=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const points=value=>Array.isArray(value)?value:String(value||'').split(/[,，;；\n]/).map(value=>value.trim()).filter(Boolean);
    let activeDetail;
    function paperFiles(record, selected=[]) {
        const chosen=new Set(selected), target=el('sourcePaperFiles');
        const files=record?StudyLibrary.files(record):[];
        target.hidden=!files.length;
        target.innerHTML='<legend>题目所在附件</legend>'+files.map(file=>`<label class="workflow-checkbox"><input type="checkbox" name="sourceFiles" value="${esc(file.id)}" ${chosen.has(file.id)?'checked':''} /> ${esc(file.name)}</label>`).join('');
    }
    function formRecord(col,id) {
        const record=(state[col]||[]).find(record=>record.id===id)||{},form=el('recordForm');
        form.elements.knowledgePoints.value=points(record.knowledgePoints).join(', ');
        el('sourcePaperFields').hidden=col!=='mistakes';
        const select=form.elements.sourcePaperId;
        select.innerHTML='<option value="">未关联试卷</option>'+(state.papers||[]).map(paper=>`<option value="${esc(paper.id)}">${esc(paper.title)}</option>`).join('');
        if(record.sourcePaperId&&!state.papers.some(paper=>paper.id===record.sourcePaperId))select.append(new Option('已删除的试卷',record.sourcePaperId));
        select.value=record.sourcePaperId||'';
        form.elements.questionNumber.value=record.questionNumber||'';
        form.elements.noteIds.innerHTML=(state.notes||[]).map(note=>`<option value="${esc(note.id)}" ${(record.noteIds||[]).includes(note.id)?'selected':''}>${esc(note.title)}</option>`).join('');
        paperFiles(state.papers.find(paper=>paper.id===select.value),(record.files||[]).map(file=>file.id));
    }
    function extendRecord(record,fd) {
        record.knowledgePoints=points(fd.get('knowledgePoints'));
        if(fd.get('collection')!=='mistakes')return;
        record.sourcePaperId=String(fd.get('sourcePaperId')||'');
        record.questionNumber=String(fd.get('questionNumber')||'').trim();
        record.noteIds=fd.getAll('noteIds');
        const paper=state.papers.find(paper=>paper.id===record.sourcePaperId);
        const source=paper?StudyLibrary.files(paper):[],sourceIds=new Set(source.map(file=>file.id)),selected=new Set(fd.getAll('sourceFiles'));
        record.files=(record.files||[]).filter(file=>!sourceIds.has(file.id)||selected.has(file.id));
        for(const file of source)if(selected.has(file.id)&&!record.files.some(item=>item.id===file.id))record.files.push({...file});
    }
    function fromPaper(id) {
        const paper=state.papers.find(paper=>paper.id===id);if(!paper)return;
        openRecordDialog('mistakes');
        const form=el('recordForm');form.elements.sourcePaperId.value=id;form.elements.path.value=(paper.subject||'未分类')+' > 真题错题';form.elements.title.value=paper.title+' · 错题';
        paperFiles(paper);
    }
    function go(id) {
        const {record,col}=findRecordWithCol(id);if(!record){PrepLocal.toast('关联记录已删除，可到回收站查看');return;}
        el('studyDetail').close();PrepPush.openView(col==='notes'||col==='resources'?'resources':col);if(col==='notes'||col==='resources')StudyLibrary.tab(col);StudyLibrary.open(id);
    }
    function link(target,id,label) { const button=document.createElement('button');button.type='button';button.className='btn btn-ghost btn-sm';button.textContent=label;button.onclick=()=>go(id);target.append(button); }
    function decorate(detail) {
        activeDetail=detail;
        const {record}=findRecordWithCol(detail.id);if(!record)return;
        let section=el('studyConnections');if(!section){section=document.createElement('section');section.id='studyConnections';section.className='study-connections';el('studyDetailBody').append(section);}
        section.replaceChildren();
        const pointList=points(record.knowledgePoints);
        if(pointList.length){const tags=document.createElement('div');tags.className='library-record-metadata';pointList.forEach(text=>{const span=document.createElement('span');span.className='record-tag';span.textContent=text;tags.append(span);});section.append(tags);}
        if(detail.col==='papers'){
            const button=document.createElement('button');button.type='button';button.className='btn btn-primary btn-sm';button.id='paperToMistake';button.innerHTML='<i class="fas fa-plus"></i> 选题加入错题集';button.onclick=()=>fromPaper(record.id);section.append(button);
            for(const mistake of state.mistakes.filter(mistake=>mistake.sourcePaperId===record.id))link(section,mistake.id,`${mistake.questionNumber?'第 '+mistake.questionNumber+' 题 · ':''}${mistake.title}`);
        }
        if(record.sourcePaperId){const paper=state.papers.find(paper=>paper.id===record.sourcePaperId);link(section,record.sourcePaperId,(paper?.title||'已删除的来源试卷')+(record.questionNumber?' · 第 '+record.questionNumber+' 题':''));}
        for(const id of record.noteIds||[]){const note=state.notes.find(note=>note.id===id);link(section,id,note?.title||'已删除的关联笔记');}
        if(detail.col==='notes')for(const mistake of state.mistakes.filter(mistake=>(mistake.noteIds||[]).includes(record.id)))link(section,mistake.id,mistake.title);
        if(detail.col==='mistakes'){
            const attempts=(state.reviewAttempts||[]).filter(attempt=>attempt.mistakeId===record.id).sort((a,b)=>String(b.time).localeCompare(String(a.time)));
            if(attempts.length){const retests=attempts.filter(item=>item.phase!=='initial'),initial=attempts.length-retests.length,history=document.createElement('details');history.className='study-attempts';const summary=document.createElement('summary');summary.textContent=`首次作答 ${initial} 次 · 复测 ${retests.length} 次 · 复测做对 ${retests.filter(item=>item.correct===true).length} 次`;history.append(summary);const list=document.createElement('ol');attempts.slice(0,30).forEach(attempt=>{const item=document.createElement('li');item.textContent=`${PrepLocal.stampText(attempt.time)} · ${attempt.phase==='initial'?(attempt.correct?'首次作答正确':'首次作答错误'):(attempt.correct?'再次做对':'仍然做错')}`;list.append(item);});history.append(list);section.append(history);}
        }
    }
    async function answer(correct) {
        if(!activeDetail?.review)return;
        const record=state.mistakes.find(record=>record.id===activeDetail.id);if(!record)return;
        (state.reviewAttempts||=[]).push({id:uid(),mistakeId:record.id,sourcePaperId:record.sourcePaperId||'',knowledgePoints:points(record.knowledgePoints),correct,time:PrepLocal.nowDate().toISOString()});
        saveState();await StudyLibrary.rate('review');
    }
    function setup() {
        const extra=document.createElement('section');extra.className='study-link-fields';extra.innerHTML='<label>知识点<input name="knowledgePoints" /></label><div id="sourcePaperFields"><label>来源试卷<select name="sourcePaperId"></select></label><label>题号<input name="questionNumber" /></label><fieldset id="sourcePaperFiles"></fieldset><label>关联笔记<select name="noteIds" multiple size="4" aria-label="关联笔记"></select></label></div>';
        el('recordForm').querySelector('.record-extra-fields').after(extra);
        el('recordForm').elements.sourcePaperId.onchange=event=>paperFiles(state.papers.find(paper=>paper.id===event.target.value));
        const previous=openRecordDialog;openRecordDialog=(col,id)=>{previous(col,id);formRecord(col,id);};
        const controls=el('studyReviewControls').lastElementChild;
        const incorrect=controls.querySelector('[onclick*="review"]');incorrect.textContent='仍然做错';incorrect.onclick=null;incorrect.removeAttribute('onclick');incorrect.addEventListener('click',()=>answer(false));
        const correct=document.createElement('button');correct.type='button';correct.className='btn btn-ghost';correct.id='studyAnswerCorrect';correct.textContent='再次做对';correct.onclick=()=>answer(true);incorrect.after(correct);
        addEventListener('study-detail',event=>decorate(event.detail));
    }
    const previousReady=PrepLocal.ready;PrepLocal.ready=()=>{previousReady();setup();};
    window.StudyFlow={extendRecord,fromPaper,answer,go,points};
})();
