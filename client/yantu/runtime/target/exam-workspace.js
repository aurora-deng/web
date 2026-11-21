/* R06. Load after workspace-core.js and practice-workspace.js; no practice overrides.
 * Endpoints and request bodies are documented in exam_service.py.
 * Optional ExamWorkspace.configure({save,confirm,accept,openAttachment}) hooks:
 * save({session,base}) / confirm(body) return ordinary Storage snapshots;
 * accept(snapshot) installs one; openAttachment(session,file,anchor|null) opens
 * the exact snapshotted attachment (never replace it with a current source file).
 * DeviceStore emergency-export prefix: exam-local- (one independent key per tab).
 * Include that prefix in rescue export/restore and draft-preservation patterns.
 * Exam feedback has kind:'exam', phase:'initial'|'retest'; repeated-error counts
 * must exclude phase:'initial'. Mistakes also keep sourceAnchorsSnapshot.
 */
(() => {
    'use strict';
    const W=window.Workspace,{el,esc,clone,button,icon}=W;
    const causes=['知识不清','条件遗漏','推理错误','计算错误','审题错误','用时不足','待核对'];
    const statuses={unanswered:'未作答',answering:'作答中',answered:'已作答',flagged:'待检查',skipped:'暂跳过'};
    const defaults=[{id:'subject-math',name:'数学',maximum:150,legacyKey:'math'},{id:'subject-english',name:'英语',maximum:100,legacyKey:'english'},{id:'subject-politics',name:'政治',maximum:100,legacyKey:'politics'},{id:'subject-major',name:'408',maximum:150,legacyKey:'major'}];
    const hooks={},tabId=uid();
    let active,preparation,host,busy=false,opening=0,listing=0,autosave,clockTimer;
    const now=()=>PrepLocal.nowDate().toISOString();
    const numeric=value=>value===''||value==null?null:typeof value==='boolean'||!Number.isFinite(Number(value))?NaN:Number(value);
    const round=value=>Number(value.toFixed(6));
    const canonical=value=>JSON.stringify(value,(_,entry)=>entry&&typeof entry==='object'&&!Array.isArray(entry)?Object.fromEntries(Object.keys(entry).sort().map(key=>[key,entry[key]])):entry);
    const same=(a,b)=>canonical(a)===canonical(b);
    const current=context=>context?.session.items[context.session.position];
    const find=(collection,id)=>(state[collection]||[]).find(record=>record.id===id);
    const filesOf=record=>window.StudyLibrary?StudyLibrary.files(record):record.files||[];
    const allFiles=items=>[...new Map(items.flatMap(item=>item.files||[]).map(file=>[file.id,file])).values()];
    const error=message=>{throw new Error(message);};
    function definitions(){return clone(window.PlanningScores?.definitions(state)||defaults);}
    function summary(session){
        const entries=session.subjectSnapshot.map(subject=>{const items=session.items.filter(item=>item.subjectId===subject.id),graded=items.filter(item=>item.awarded!==null),coverage=round(items.reduce((sum,item)=>sum+(item.maximum||0),0)),subtotal=round(graded.reduce((sum,item)=>sum+item.awarded,0)),complete=items.length>0&&graded.length===items.length&&items.every(item=>item.maximum!==null)&&Math.abs(coverage-subject.maximum)<.005;return {...clone(subject),questions:items.length,graded:graded.length,coverage,subtotal,complete,value:complete?subtotal:null};});
        const complete=entries.every(item=>item.complete),subtotal=round(entries.reduce((sum,item)=>sum+item.subtotal,0));
        return {entries,complete,graded:entries.reduce((sum,item)=>sum+item.graded,0),questions:session.items.length,subtotal,maximum:entries.reduce((sum,item)=>sum+item.maximum,0),total:complete?subtotal:null};
    }
    function validate(session){
        if(!session.title.trim()||!session.items.length||session.items.length>100)error('请填写考试名称并保留 1 至 100 道题');
        const ids=new Set(),maxima={};
        if(session.subjectSnapshot.length>40)error('本次最多 40 个科目');
        for(const subject of session.subjectSnapshot){if(!subject.id||ids.has(subject.id)||!Number.isFinite(subject.maximum)||subject.maximum<1||subject.maximum>10000)error('本次科目满分需为 1 至 10000');ids.add(subject.id);maxima[subject.id]=0;}
        if(!ids.size)error('请为本次题目选择科目');
        for(const item of session.items){
            const subject=session.subjectSnapshot.find(subject=>subject.id===item.subjectId);if(!subject)error('请为每道题选择科目');
            if(item.maximum!==null&&(!Number.isFinite(item.maximum)||item.maximum<=0||item.maximum>subject.maximum))error(item.title+'：本题满分无效');
            if(item.awarded!==null&&(item.maximum===null||!Number.isFinite(item.awarded)||item.awarded<0||item.awarded>item.maximum))error(item.title+'：得分需在零分至本题满分之间，空白为未核分');
            maxima[item.subjectId]+=item.maximum||0;
        }
        for(const subject of session.subjectSnapshot)if(maxima[subject.id]>subject.maximum+.005)error(subject.name+'：逐题满分超过本次科目满分');
        return true;
    }
    function pack(context){const value=clone(context.session);if(value.status==='running')value.status='paused';return value;}
    function read(context=active){
        if(!context||context!==active||!el('examSessionDialog')?.open)return;
        const item=current(context),form=el('examAnswerForm');
        for(const key of['answer','wrongThought','correctionReason','nextAction'])item[key]=form.elements[key].value;
        for(const key of['maximum','awarded'])item[key]=numeric(form.elements[key].value);
        item.status=form.elements.status.value;
        item.causeKinds=[...form.querySelectorAll('[name=causeKinds]:checked')].map(input=>input.value);
    }
    function settle(context,keep=true){
        if(context?.clock){const tick=performance.now();current(context).elapsedSeconds+=Math.max(0,(tick-context.clock)/1000);context.clock=keep?tick:0;}
        if(context&&!keep&&context.session.status==='running')context.session.status='paused';
    }
    function mark(context=active){if(!context)return;context.version++;context.dirty=true;context.session.updatedAt=now();}
    function local(context=active){
        if(!context)return Promise.resolve();read(context);settle(context);const value={session:pack(context),base:clone(context.base),dirty:context.dirty,updatedAt:now()};
        context.localTail=context.localTail.catch(()=>{}).then(()=>DeviceStore.put(context.key,value));
        return context.localTail.then(()=>{context.localFailed=false;if(active===context)saveStatus(context.conflict?'远端已变化；本机草稿保留，未覆盖远端':context.dirty?'本机草稿已保留':'已保存到电脑，本机留有恢复副本');}).catch(exception=>{context.localFailed=true;if(active===context)saveStatus('本机草稿未能写入，请保持页面打开并立即导出作答');throw exception;});
    }
    function saveStatus(message){el('examSaveStatus').textContent=message;}
    async function request(kind,body){
        if(hooks[kind])return hooks[kind](clone(body));
        await PrepLocal.flush();if(PrepLocal.hasPending())error('还有待同步修改，考试本机草稿已保留；连接恢复后再保存到电脑');
        return PrepLocal.jsonRequest('/api/workspace/exam-'+(kind==='save'?'save':'confirm'),body);
    }
    function accept(snapshot){if(hooks.accept)hooks.accept(snapshot);else PrepLocal.accept(snapshot);dispatchEvent(new CustomEvent('workspace-change'));}
    function sync(context=active){
        if(!context)return Promise.resolve();
        context.saveTail=context.saveTail.catch(()=>{}).then(async()=>{
            await local(context);if(!context.dirty)return;
            const value=pack(context),version=context.version;validate(value);
            const snapshot=await request('save',{session:value,base:clone(context.base)}),saved=snapshot.state.examSessions.find(item=>item.id===value.id);
            if(!same(saved,value))error('电脑返回的会话与草稿不同，本机副本仍保留');
            accept(snapshot);context.base=clone(saved);context.conflict=false;
            if(context.version===version)context.dirty=false;
            await local(context);
        }).catch(async exception=>{context.conflict=true;await local(context).catch(()=>{});if(active===context)saveStatus(context.localFailed?'本机草稿未能写入，请保持页面打开并立即导出作答':exception.message+'；可导出本机作答');throw exception;});
        return context.saveTail;
    }
    function safe(action){return async()=>{try{await action();}catch(exception){PrepLocal.toast(exception.message);}};}
    function exportSession(context=active){if(!context)return;read(context);settle(context);downloadBlob(context.session.title+'-整卷作答.json',new Blob([JSON.stringify({format:'yantu-exam-draft-v1',session:pack(context),base:context.base,dirty:context.dirty},null,2)],{type:'application/json'}));}
    async function closeSession(){
        const context=active;if(!context||busy)return;read(context);settle(context,false);mark(context);clearTimeout(autosave);
        await local(context);try{await sync(context);}catch(exception){PrepLocal.toast(exception.message);}
        if(active===context){active=null;el('examSessionDialog')._close();render();}
    }
    async function openSession(id,key){
        if(busy)return;await closeSession();const token=++opening,record=find('examSessions',id);
        const drafts=(await DeviceStore.entries('exam-local-')).filter(([,value])=>value.session?.id===id&&(key?true:value.dirty)).sort((a,b)=>String(b[1].updatedAt).localeCompare(String(a[1].updatedAt)));
        if(token!==opening)return;
        const chosen=key?drafts.find(([draftKey])=>draftKey===key):drafts[0];
        let session=record&&clone(record),base=record&&clone(record),dirty=false;
        if(chosen&&(!session||key||confirm('发现本机未入库的整卷作答，继续该草稿？取消可查看电脑保存版，草稿不会删除。'))){session=clone(chosen[1].session);base=clone(chosen[1].base);dirty=chosen[1].dirty;}
        if(!session)error('未找到考试会话或本机草稿');
        if(session.status==='running')session.status='paused';
        active={session,base:base||null,dirty,version:0,clock:0,key:'exam-local-'+tabId+'-'+session.id,localTail:Promise.resolve(),saveTail:Promise.resolve(),conflict:!same(base||null,record||null)};
        el('examSessionDialog').showModal();show();await local();
    }
    function inferSubject(record,subjects){const known=record.subjectId||record.sourceSnapshot?.subjectId;if(subjects.some(subject=>subject.id===known))return known;const name=String(record.subject||record.path||record.sourceSnapshot?.path||'');return subjects.find(subject=>name.includes(subject.name))?.id||'';}
    function snapshotItem(item,source,kind,subjects){
        const origin=W.record(item.sourceId||item.id),base=item.sourceSnapshot||source;
        const value={id:uid(),sourceId:item.sourceId||source.id,sourceCollection:origin?.collection||kind,
            sourceSnapshot:clone(base),title:item.title||'题目',content:item.content||'',solution:item.solution||'',
            questionType:item.questionType||'',knowledgeIds:clone(item.knowledgeIds||[]),anchors:clone(item.anchors||[]),
            files:clone(item.files||filesOf(source)),subjectId:inferSubject(item,subjects),maximum:numeric(item.maximum),
            awarded:null,answer:'',status:'unanswered',elapsedSeconds:0,revealed:false,causeKinds:[],wrongThought:'',correctionReason:'',nextAction:''};
        return value;
    }
    function prepareSource(kind,id,count=1){
        const source=find(kind,id);if(!source)error('请选择已有试卷或题组');
        const subjects=definitions();if(!subjects.length)error('请先启用考试科目');
        let items;
        if(kind==='questionSets'){if(!source.items?.length)error('题组没有题目');items=source.items.map(item=>snapshotItem(item,source,kind,subjects));}
        else{
            if(!Number.isInteger(count)||count<1||count>100)error('请输入 1 至 100 道题');
            items=Array.from({length:count},(_,index)=>snapshotItem({title:'第 '+(index+1)+' 题',content:'',solution:'',subjectId:source.subjectId,path:source.path,files:filesOf(source)},source,kind,subjects));
        }
        if(items.length>100)error('本次最多 100 道题');
        preparation={kind,source:clone(source),subjects,items};
        el('examCreateForm').elements.title.value=source.title||'整卷模拟';
        el('examSubjectMaxima').innerHTML=subjects.map(subject=>`<label>${esc(subject.name)} · 本次满分<input type="number" data-subject-maximum="${esc(subject.id)}" min="1" max="10000" step="0.01" value="${subject.maximum}" required /></label>`).join('');
        el('examQuestionSetup').innerHTML=items.map((item,index)=>`<fieldset data-exam-item="${index}"><legend>第 ${index+1} 题</legend><label>题目名称<input data-field="title" maxlength="300" value="${esc(item.title)}" required /></label><label>科目<select data-field="subjectId" required><option value="">选择科目</option>${subjects.map(subject=>`<option value="${esc(subject.id)}" ${item.subjectId===subject.id?'selected':''}>${esc(subject.name)}</option>`).join('')}</select></label><label>本题满分<input type="number" data-field="maximum" min="0.01" max="1000000" step="0.01" value="${item.maximum??''}" /></label>${kind==='papers'?'<label class="exam-wide">题干补充（原题见附件）<textarea data-field="content" rows="2" maxlength="200000"></textarea></label>':'<p class="exam-wide exam-text">'+esc(item.content||'题干见原件')+'</p>'}</fieldset>`).join('');
        el('examCreateDetails').hidden=false;
    }
    function newExam(kind,id){
        preparation=null;const form=el('examCreateForm');form.reset();form.elements.date.value=PrepLocal.dateText(PrepLocal.nowDate());el('examCreateDetails').hidden=true;
        form.elements.source.innerHTML='<option value="">选择已有试卷或题组</option>'+['papers','questionSets'].flatMap(collection=>(state[collection]||[]).map(record=>`<option value="${collection}:${esc(record.id)}">${collection==='papers'?'试卷':'题组'} · ${esc(record.title)}</option>`)).join('');
        if(kind&&id)form.elements.source.value=kind+':'+id;
        el('examCreateDialog').showModal();
    }
    async function create(event){
        event.preventDefault();if(busy||!preparation)return;const form=event.target;if(!form.reportValidity())return;
        const {kind,source,subjects}=preparation;
        if(!same(find(kind,source.id),source))error('题源已变化，请退回重新预览');
        const items=clone(preparation.items);
        for(const fieldset of el('examQuestionSetup').querySelectorAll('[data-exam-item]')){const item=items[Number(fieldset.dataset.examItem)];for(const input of fieldset.querySelectorAll('[data-field]'))item[input.dataset.field]=input.dataset.field==='maximum'?numeric(input.value):input.value;}
        const used=new Set(items.map(item=>item.subjectId)),subjectSnapshot=subjects.filter(subject=>used.has(subject.id)).map(subject=>{const maximum=numeric(el('examSubjectMaxima').querySelector('[data-subject-maximum="'+subject.id+'"]').value),value={id:subject.id,name:subject.name,maximum};if(subject.legacyKey)value.legacyKey=subject.legacyKey;if(subject.target!=null&&subject.target<=maximum)value.target=subject.target;return value;});
        const session={id:uid(),schemaVersion:1,title:form.elements.title.value.trim(),date:form.elements.date.value,sourceKind:kind,sourceId:source.id,paperId:kind==='papers'?source.id:source.paperId||'',setId:kind==='questionSets'?source.id:'',sourceSnapshot:clone(source),subjectSnapshot,items,files:allFiles(items),status:'paused',position:0,createdAt:now(),updatedAt:now()};
        validate(session);busy=true;
        try{
            active={session,base:null,dirty:true,version:0,clock:0,key:'exam-local-'+tabId+'-'+session.id,localTail:Promise.resolve(),saveTail:Promise.resolve(),conflict:false};
            await local();el('examCreateDialog').close();el('examSessionDialog').showModal();show();
            try{await sync();}catch(exception){PrepLocal.toast(exception.message);}
        }finally{busy=false;}
    }
    function sourceState(){const session=active.session,source=find(session.sourceKind,session.sourceId);return !source?'题源已删除，当前会话仍保留原件引用及文字快照':!same(source,session.sourceSnapshot)?'题源已修改，当前使用建卷时快照':'题源快照与当前记录一致';}
    function nav(){
        const target=el('examQuestionNav');target.replaceChildren();active.session.items.forEach((item,index)=>{const value=button(String(index+1)+' · '+statuses[item.status]+(item.awarded!==null?' · '+item.awarded+'/'+(item.maximum??'未设'):''),'',()=>move(index));value.querySelector('i')?.remove();value.setAttribute('aria-current',index===active.session.position?'step':'false');target.append(value);});
    }
    async function openAttachment(file,anchor=null){
        const context=active;read(context);settle(context,false);mark(context);await local(context);show();
        const fingerprint=anchor?.fingerprint||file.sha256;
        if(fingerprint){let digest;if(file.library){const verified=await PrepLocal.jsonRequest('/api/workspace/file-version?id='+encodeURIComponent(file.id));digest=verified.fingerprint;}else{const blob=await getFile(file.id);if(!blob)error('原件尚未缓存或已缺失，作答与引用快照仍保留');digest=[...new Uint8Array(await crypto.subtle.digest('SHA-256',await blob.arrayBuffer()))].map(value=>value.toString(16).padStart(2,'0')).join('');}if(digest!==fingerprint)error('原件版本与题源快照不同，未打开替换后的文件');}
        if(hooks.openAttachment)return hooks.openAttachment(clone(context.session),clone(file),anchor&&clone(anchor));
        if(!find('examSessions',context.session.id))error('本机会话尚未同步，请先保存到电脑后查看原件');
        const position=context.session.files.findIndex(item=>item.id===file.id);
        if(anchor&&window.ReaderWorkspace)return ReaderWorkspace.open(context.session.id,position,anchor.page,clone(anchor));
        return openViewer(context.session.id,position);
    }
    function show(){
        if(!active)return;const session=active.session,item=current(active),form=el('examAnswerForm'),subject=session.subjectSnapshot.find(subject=>subject.id===item.subjectId);
        el('examTitle').textContent=session.title;el('examPosition').textContent=(session.position+1)+' / '+session.items.length;
        el('examSourceStatus').textContent=sourceState();el('examQuestionTitle').textContent=item.title+' · '+subject.name;
        el('examQuestionText').textContent=item.content||'题干见原件';
        for(const key of['answer','wrongThought','correctionReason','nextAction','maximum','awarded','status'])form.elements[key].value=item[key]??'';
        form.elements.maximum.max=subject.maximum;form.elements.awarded.max=item.maximum??subject.maximum;
        form.querySelectorAll('[name=causeKinds]').forEach(input=>input.checked=item.causeKinds.includes(input.value));
        el('examSolution').textContent=item.revealed?item.solution||'尚未录入解析':'';el('examReveal').hidden=item.revealed;
        const attachments=el('examAttachments');attachments.replaceChildren();
        for(const anchor of item.anchors||[]){const file=item.files.find(file=>file.id===anchor.fileId);if(file)attachments.append(button('原题第 '+anchor.page+' 页','location-dot',()=>openAttachment(file,anchor)));}
        for(const file of item.files)attachments.append(button(file.name||'原件','file',()=>openAttachment(file)));
        el('examPrevious').disabled=session.position===0;el('examNext').disabled=session.position===session.items.length-1;
        el('examPause').innerHTML='<i class="fas fa-'+(active.clock?'pause':'play')+'" aria-hidden="true"></i><span>'+(active.clock?'暂停计时':'继续作答')+'</span>';
        el('examFinalizedNote').textContent=session.finalization?'本次结果已确认入库。作答仍可修改；修订不会自动改写已入库成绩、错题或反馈。':'交卷后仍可返回修改；查看解析不产生分数或复习反馈。';
        nav();renderClock();links();
    }
    function renderClock(){if(!active)return;const item=current(active),seconds=Math.floor(item.elapsedSeconds+(active.clock?(performance.now()-active.clock)/1000:0)),total=Math.floor(active.session.items.reduce((sum,item)=>sum+item.elapsedSeconds,0)+(active.clock?(performance.now()-active.clock)/1000:0));el('examElapsed').textContent='本题 '+Math.floor(seconds/60)+':'+String(seconds%60).padStart(2,'0')+' · 全卷 '+Math.floor(total/60)+':'+String(total%60).padStart(2,'0')+(active.clock?'':' · 已暂停');}
    async function pause(){if(!active||busy)return;read();if(active.clock)settle(active,false);else{active.clock=performance.now();active.session.status='running';}mark();await local();show();}
    async function move(position){if(!active||busy)return;read();settle(active,false);active.session.position=Math.max(0,Math.min(active.session.items.length-1,position));mark();show();await local();sync().catch(exception=>PrepLocal.toast(exception.message));}
    async function reveal(){if(!active||busy)return;read();settle(active,false);current(active).revealed=true;mark();await local();show();}
    function links(){
        const target=el('examLinks');target.replaceChildren();if(!active)return;const session=active.session,receipt=session.finalization;
        target.append(button('题源记录','arrow-up-right-from-square',async()=>{const id=session.sourceId;await closeSession();W.openRecord(id);}));
        if(receipt?.scoreId)target.append(button('关联成绩','chart-line',()=>openScore(receipt.scoreId)));
        for(const[itemId,mistakeId]of Object.entries(receipt?.mistakeLinks||{})){const title=session.items.find(item=>item.id===itemId)?.title||'错题';target.append(button('错题 · '+title,'arrow-up-right-from-square',async()=>{await closeSession();W.openRecord(mistakeId);}));if(window.PracticeWorkspace)target.append(button('再练 · '+title,'pen-to-square',async()=>{await closeSession();await PracticeWorkspace.startQuestion(mistakeId);}));}
    }
    function scoreProposal(session){const data=summary(session);return {name:session.title,date:session.date||(session.submittedAt||session.createdAt).slice(0,10),paperId:session.paperId,examSessionId:session.id,setId:session.setId,subjectSnapshot:clone(session.subjectSnapshot),subjectScores:Object.fromEntries(data.entries.map(entry=>[entry.id,entry.value])),examCoverage:data,comment:'整卷逐题人工核分；空白科目尚未核全或题目满分未覆盖本科满分。'};}
    async function openScore(id){
        if(!window.PlanningScores?.open)error('成绩编辑模块尚未就绪，考试草稿已保留');
        const proposal=scoreProposal(active.session),receiptId=active.session.finalization?.scoreId,linked=(state.scores||[]).find(score=>score.examSessionId===active.session.id);await closeSession();await PlanningScores.open(id||receiptId||linked?.id||null,proposal);
    }
    async function submit(){
        if(!active||busy)return;read();settle(active,false);validate(active.session);
        if(!confirm('交卷并进入人工核分预览？未答和未核分仍保持原状，不会自动判分；之后仍可返回修改。')){await local();show();return;}
        active.session.status='submitted';active.session.submittedAt=now();mark();await local();
        try{await sync();}catch(exception){PrepLocal.toast(exception.message);}
        show();preview();
    }
    function preview(){
        if(!active)return;const session=active.session,data=summary(session),receipt=session.finalization;
        el('examGradeSummary').textContent=data.complete?'已人工核全：'+data.total+' / '+data.maximum:'未核全：'+data.graded+' / '+data.questions+' 题；已核小计 '+data.subtotal+'，不作为完整总分';
        el('examSubjectSummary').replaceChildren();for(const entry of data.entries){const value=document.createElement('p');value.textContent=entry.name+'：'+(entry.complete?entry.value+' / '+entry.maximum:'未核全，已核小计 '+entry.subtotal+'；题目满分覆盖 '+entry.coverage+' / '+entry.maximum)+' · '+entry.graded+'/'+entry.questions+' 题';el('examSubjectSummary').append(value);}
        const target=el('examGradeItems');target.replaceChildren();
        for(const item of session.items){
            const lost=item.awarded!==null&&item.maximum!==null&&item.awarded<item.maximum,existing=item.sourceCollection==='mistakes'&&find('mistakes',item.sourceId),row=document.createElement('section');row.className='exam-grade-row';
            const heading=document.createElement('strong');heading.textContent=item.title+' · '+(item.awarded===null?'未核分':item.awarded+' / '+item.maximum)+' · '+statuses[item.status];row.append(heading);
            const meta=document.createElement('small');meta.textContent='用时 '+Math.floor(item.elapsedSeconds)+' 秒'+(item.causeKinds.length?' · '+item.causeKinds.join('、'):'');row.append(meta);
            if(lost){const label=document.createElement('label');label.className='workflow-checkbox';label.innerHTML='<input type="checkbox" name="mistakeItemIds" value="'+esc(item.id)+'" '+(receipt?'disabled':'')+' /> '+(existing?'关联已有错题':'确认新增错题');row.append(label);}
            if(item.awarded!==null&&(existing||lost)){const label=document.createElement('label');label.className='workflow-checkbox';label.innerHTML='<input type="checkbox" name="reviewItemIds" value="'+esc(item.id)+'" '+(receipt?'disabled':'')+' /> '+(existing?'另记本次复习反馈':'另存首次作答反馈');row.append(label);}
            row.append(button('返回本题','pen',()=>{el('examGradeDialog').close();return move(session.items.indexOf(item));}));target.append(row);
        }
        const score=el('examCreateScore');score.checked=false;score.disabled=Boolean(receipt)||!data.entries.some(entry=>entry.complete);
        el('examConfirmGrades').disabled=Boolean(receipt);el('examGradeResult').textContent=receipt?'已确认。再次点击或网络重试不会重复入库；后续修改请打开关联成绩或错题。':score.disabled?'尚无完整科目总分，仍可选取失分题入错题；部分分数保留在逐题记录。':'请核对逐题得分、错因及入库选项。';
        el('examGradeDialog').showModal();
    }
    async function confirmGrades(){
        const context=active;if(!context||busy||context.session.finalization)return;
        const selected=name=>[...el('examGradeItems').querySelectorAll('[name='+name+']:checked')].map(input=>input.value),mistakeItemIds=selected('mistakeItemIds'),reviewItemIds=selected('reviewItemIds'),createScore=el('examCreateScore').checked;
        if(!createScore&&!mistakeItemIds.length&&!reviewItemIds.length)error('请至少选择一项入库结果，或返回继续作答');
        for(const id of [...mistakeItemIds,...reviewItemIds]){const item=context.session.items.find(item=>item.id===id);if(item.awarded<item.maximum&&!item.causeKinds.length)error(item.title+'：请先补充错因，不确定时选“待核对”');if(reviewItemIds.includes(id)&&!mistakeItemIds.includes(id)&&!(item.sourceCollection==='mistakes'&&find('mistakes',item.sourceId)))error(item.title+'：先确认新增错题，再记录复习反馈');}
        if(!confirm('确认写入 '+(createScore?'1 条成绩、':'')+mistakeItemIds.length+' 个错题关联、'+reviewItemIds.length+' 条真实复习反馈？未选择的题目不会入库。'))return;
        busy=true;el('examConfirmGrades').disabled=true;el('examAnswerForm').querySelectorAll('input,textarea,select').forEach(input=>input.disabled=true);
        try{
            await sync(context);const result=await request('confirm',{id:context.session.id,base:clone(context.base),confirmation:'CONFIRM_EXAM_RESULTS',createScore,mistakeItemIds,reviewItemIds});
            const saved=result.state.examSessions.find(item=>item.id===context.session.id);if(!saved?.finalization)error('未收到确认凭据，本机草稿保留，请重新连接核对');
            accept(result);context.session=clone(saved);context.base=clone(saved);context.dirty=false;context.conflict=false;context.version++;await local(context);
            el('examGradeDialog')._close();show();render();PrepLocal.toast('已确认入库，原题和未选择的记录均未改动');
        }catch(exception){el('examGradeResult').textContent=exception.message;throw exception;}finally{busy=false;el('examAnswerForm').querySelectorAll('input,textarea,select').forEach(input=>input.disabled=false);el('examConfirmGrades').disabled=Boolean(context.session.finalization);}
    }
    async function render(){
        if(!host)return;const token=++listing,target=el('examSessionList');target.replaceChildren();
        for(const session of [...state.examSessions||[]].reverse()){
            const row=document.createElement('div');row.className='exam-record-row';const data=summary(session),meta=document.createElement('small');meta.textContent=(session.finalization?'已确认入库':session.status==='submitted'?'待人工核分':'可继续')+' · '+session.items.length+' 题 · '+(data.complete?data.total+' / '+data.maximum:'未核全 '+data.graded+'/'+data.questions+' 题');
            row.append(button(session.title,'file-pen',()=>openSession(session.id)),meta,icon('导出已保存作答','download',()=>downloadBlob(session.title+'-整卷会话.json',new Blob([JSON.stringify(session,null,2)],{type:'application/json'}))));target.append(row);
        }
        if(!target.children.length)target.textContent='暂无整卷会话';
        const drafts=await DeviceStore.entries('exam-local-');if(token!==listing)return;const localTarget=el('examLocalDrafts');localTarget.replaceChildren();
        for(const[key,value]of drafts.filter(([,value])=>value.dirty&&value.session)){
            const row=document.createElement('div');row.className='exam-record-row';const status=document.createElement('small');status.textContent='本机未入库 · '+value.updatedAt;
            row.append(button(value.session.title,'rotate-left',()=>openSession(value.session.id,key)),status,icon('导出本机草稿','download',()=>downloadBlob(value.session.title+'-本机草稿.json',new Blob([JSON.stringify(value,null,2)],{type:'application/json'}))),icon('删除此本机副本','trash',async()=>{if(!confirm('仅删除这个本机恢复副本？电脑记录和其他副本不受影响。'))return;await DeviceStore.put(key,undefined);render();}));localTarget.append(row);
        }
        if(!localTarget.children.length)localTarget.textContent='暂无未入库的本机草稿';
    }
    function setup(){
        if(host)return;state.examSessions||=[];
        host=W.view('exam-sessions','整卷模拟','clipboard-list',()=>render().catch(exception=>PrepLocal.toast(exception.message)));
        host.querySelector('.workspace-actions').append(button('新建整卷会话','plus',()=>newExam(),'btn-primary'));
        host.querySelector('.workspace-content').innerHTML='<h3>考试记录</h3><div id="examSessionList"></div><h3>本机未入库作答</h3><div id="examLocalDrafts"></div>';
        const createDialog=W.dialog('examCreateDialog','建立整卷会话','<form id="examCreateForm"><label>题源<select name="source" required></select></label><label>试卷题数<input type="number" name="count" min="1" max="100" value="1" required /></label><button type="button" id="examPrepare" class="btn btn-ghost"><i class="fas fa-list" aria-hidden="true"></i> 预览题目</button><div id="examCreateDetails" hidden><label>考试名称<input name="title" maxlength="300" required /></label><div id="examSubjectMaxima" class="exam-fields"></div><div id="examQuestionSetup"></div><div class="dialog-actions"><button type="button" id="examCreateReturn" class="btn btn-ghost">退回</button><button type="submit" class="btn btn-primary"><i class="fas fa-save" aria-hidden="true"></i> 保存并进入作答</button></div></div></form>');createDialog.classList.add('exam-create-dialog');
        const dialog=W.dialog('examSessionDialog','整卷作答','<header class="exam-heading"><h3 id="examTitle"></h3><span id="examPosition"></span><span id="examElapsed"></span><button id="examPause" class="btn btn-ghost"></button></header><p id="examSaveStatus" role="status"></p><p id="examSourceStatus"></p><p id="examFinalizedNote"></p><div class="exam-layout"><section><h4 id="examQuestionTitle"></h4><p id="examQuestionText" class="exam-text"></p><div id="examAttachments" class="workspace-actions"></div><form id="examAnswerForm"><label>作答状态<select name="status">'+Object.entries(statuses).map(([value,label])=>'<option value="'+value+'">'+label+'</option>').join('')+'</select></label><label>独立作答与过程<textarea name="answer" rows="9" maxlength="200000"></textarea></label><div class="exam-fields"><label>本题满分<input name="maximum" type="number" min="0.01" step="0.01" /></label><label>人工核定得分<input name="awarded" type="number" min="0" step="0.01" /></label></div><button id="examReveal" type="button" class="btn btn-ghost"><i class="fas fa-eye" aria-hidden="true"></i> 查看参考解析</button><p id="examSolution" class="exam-text"></p><fieldset class="exam-causes"><legend>失分原因</legend>'+causes.map(cause=>'<label class="workflow-checkbox"><input type="checkbox" name="causeKinds" value="'+cause+'" /> '+cause+'</label>').join('')+'</fieldset><label>当时的错误思路<textarea name="wrongThought" rows="2" maxlength="20000"></textarea></label><label>修正理由<textarea name="correctionReason" rows="2" maxlength="20000"></textarea></label><label>下一步行动<textarea name="nextAction" rows="2" maxlength="20000"></textarea></label></form></section><aside><h4>逐题进度</h4><div id="examQuestionNav"></div><h4>关联记录</h4><div id="examLinks" class="exam-links"></div></aside></div><footer class="dialog-actions"><button id="examPrevious" class="btn btn-icon" aria-label="上一题" title="上一题"><i class="fas fa-chevron-left" aria-hidden="true"></i></button><button id="examNext" class="btn btn-icon" aria-label="下一题" title="下一题"><i class="fas fa-chevron-right" aria-hidden="true"></i></button><button id="examExport" class="btn btn-icon" aria-label="导出本机作答" title="导出本机作答"><i class="fas fa-download" aria-hidden="true"></i></button><button id="examSync" class="btn btn-ghost"><i class="fas fa-save" aria-hidden="true"></i> 保存到电脑</button><button id="examInterrupt" class="btn btn-ghost">中断并保存</button><button id="examSubmit" class="btn btn-primary">交卷核分</button></footer>');dialog.classList.add('exam-workspace');
        const grading=W.dialog('examGradeDialog','人工核分预览','<p id="examGradeSummary"></p><div id="examSubjectSummary"></div><div id="examGradeItems"></div><label class="workflow-checkbox"><input type="checkbox" id="examCreateScore" /> 确认新增关联成绩（未核全科目保持未录）</label><p id="examGradeResult" role="status"></p><div class="dialog-actions"><button id="examGradeReturn" class="btn btn-ghost">返回修改</button><button id="examScoreProposal" class="btn btn-ghost"><i class="fas fa-chart-line" aria-hidden="true"></i> 打开成绩提案</button><button id="examConfirmGrades" class="btn btn-primary">确认所选结果入库</button></div>');grading.classList.add('exam-grade-dialog');
        const dateLabel=document.createElement('label');dateLabel.innerHTML='考试日期<input name="date" type="date" required />';el('examCreateDetails').prepend(dateLabel);
        el('examPrepare').onclick=safe(()=>{const source=el('examCreateForm').elements.source.value,[kind,id]=source.split(':');prepareSource(kind,id,Number(el('examCreateForm').elements.count.value));});
        for(const input of[el('examCreateForm').elements.source,el('examCreateForm').elements.count])input.onchange=()=>{preparation=null;el('examCreateDetails').hidden=true;};
        el('examCreateForm').onsubmit=event=>create(event).catch(exception=>PrepLocal.toast(exception.message));el('examCreateReturn').onclick=()=>{preparation=null;el('examCreateDetails').hidden=true;};
        const form=el('examAnswerForm');form.onsubmit=event=>event.preventDefault();form.oninput=()=>{
            if(!active||busy)return;read();if(active.session.status==='submitted')active.session.status='paused';mark();
            if(form.elements.awarded.value!==''&&current(active).maximum!==null)form.elements.awarded.max=current(active).maximum;
            local().catch(exception=>PrepLocal.toast(exception.message));nav();clearTimeout(autosave);const context=active;autosave=setTimeout(()=>sync(context).catch(exception=>PrepLocal.toast(exception.message)),1500);
        };
        el('examPause').onclick=safe(pause);el('examPrevious').onclick=safe(()=>move(active.session.position-1));el('examNext').onclick=safe(()=>move(active.session.position+1));el('examReveal').onclick=safe(reveal);el('examExport').onclick=()=>exportSession();el('examSync').onclick=safe(()=>sync());el('examInterrupt').onclick=safe(closeSession);el('examSubmit').onclick=safe(submit);
        el('examGradeReturn').onclick=()=>grading.close();el('examConfirmGrades').onclick=safe(confirmGrades);el('examScoreProposal').onclick=safe(async()=>{grading.close();await openScore();});
        dialog._close=dialog.close.bind(dialog);dialog.close=()=>safe(closeSession)();dialog.oncancel=event=>{event.preventDefault();safe(closeSession)();};
        grading.oncancel=event=>{event.preventDefault();if(!busy)grading.close();};grading._close=grading.close.bind(grading);grading.close=()=>{if(!busy)grading._close();};
        clockTimer=setInterval(()=>{renderClock();if(active?.clock){mark();local().catch(exception=>PrepLocal.toast(exception.message));}},1000);
        const interrupt=()=>{if(!active)return;read();settle(active,false);mark();local().catch(exception=>PrepLocal.toast(exception.message));sync().catch(()=>{});show();};
        document.addEventListener('visibilitychange',()=>{if(document.hidden)interrupt();});addEventListener('pagehide',interrupt);
        addEventListener('workspace-open-record',event=>{if(event.detail.collection==='examSessions'){W.navigate('exam-sessions');safe(()=>openSession(event.detail.record.id))();}});
        addEventListener('study-detail',event=>{
            const target=el('studyConnections');if(!target)return;
            const {col,id}=event.detail,record=find(col,id);
            if(['papers','questionSets'].includes(col))target.append(button('建立整卷会话','clipboard-list',()=>newExam(col,id)));
            for(const session of (state.examSessions||[]).filter(session=>session.id===record?.examSessionId||session.sourceId===id||session.paperId===id||(col==='mistakes'&&session.items.some(item=>item.sourceCollection==='mistakes'&&item.sourceId===id))).slice().reverse())target.append(button('整卷记录 · '+session.title,'clipboard-list',()=>W.openRecord(session.id)));
        });
        render().catch(exception=>PrepLocal.toast(exception.message));
    }
    window.ExamWorkspace={configure:value=>Object.assign(hooks,value),newExam,prepareSource,openSession,submit,confirmGrades,summary,scoreProposal,validate,save:()=>sync(),interrupt:closeSession,exportDraft:exportSession,debug:()=>active?clone(active.session):null};
    addEventListener('workspace-ready',setup,{once:true});if(W.ready())setup();
})();
