(() => {
    const el=id=>document.getElementById(id);
    const esc=value=>String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
    const today=()=>PrepLocal.dateText(PrepLocal.nowDate());
    const addDays=(day,n)=>{const date=new Date(day+'T12:00:00Z');date.setUTCDate(date.getUTCDate()+n);return date.toISOString().slice(0,10);};
    const monday=day=>addDays(day,-((new Date(day+'T12:00:00Z').getUTCDay()+6)%7));
    const duration=seconds=>seconds>=3600?(seconds/3600).toFixed(1)+' 小时':seconds>=60?Math.floor(seconds/60)+' 分钟':seconds+' 秒';
    let week, last, homeSignature = '';
    try{last=JSON.parse(YantuEnvironment.storage.getItem('yantu-last-study')||'null');}catch(_){}
    function dueRecords(){const day=today();return (state.mistakes||[]).filter(record=>record.mastery!=='mastered'&&(!record.nextReview||record.nextReview<=day));}
    function home(){
        if(!el('homeStudyFocus'))return;
        const day=today(),due=dueRecords(),resume=last&&findRecordWithCol(last.id)?.record;
        const unfinished=(state.tasks||[]).filter(task=>YantuActivity.taskIsActive(task)&&task.date&&task.date<=day);
        const firstTasks=[...unfinished].sort((a,b)=>Number(Boolean(b.carried))-Number(Boolean(a.carried))).slice(0,3);
        const todaySeconds=YantuActivity.dailySummary(state,day).totalSeconds;
        const signature=JSON.stringify([day,due.length,due.slice(0,3).map(record=>[record.id,record.title]),resume?.id,resume?.title,unfinished.length,firstTasks.map(task=>[task.id,task.title]),todaySeconds]);
        if(signature===homeSignature)return;homeSignature=signature;
        el('homeDueCount').textContent=due.length;
        const list=el('homeDueItems');list.replaceChildren();
        for(const record of due.slice(0,3)){const button=document.createElement('button');button.type='button';button.className='home-study-link';button.textContent=record.title;button.onclick=()=>{PrepPush.openView('mistakes');StudyLibrary.open(record.id,true);};list.append(button);}
        if(!due.length)list.innerHTML='<span class="report-empty">没有到期错题</span>';
        el('homeResumeTitle').textContent=resume?.title||'尚未打开资料';el('homeResume').disabled=!resume;
        el('homeMustDo').textContent=unfinished.length+' 项待完成';
        const tasks=el('homeMustDoItems');tasks.replaceChildren();
        for(const task of firstTasks){const row=document.createElement('label');row.className='home-task-check';row.dataset.recordId=task.id;const checkbox=document.createElement('input');checkbox.type='checkbox';checkbox.disabled=Boolean(task.bridgeReadonly);checkbox.onchange=()=>toggleTask(task.id);const text=document.createElement('span');text.textContent=task.title;row.append(checkbox,text);tasks.append(row);}
        el('homeFocusActual').textContent=duration(todaySeconds);
    }
    function contribution(session,day){
        return YantuActivity.sessionDaySeconds(session,day);
    }
    function summary(start){
        const days=Array.from({length:7},(_,i)=>addDays(start,i)),end=days.at(-1),sessions=YantuActivity.uniqueSessions(state.focusSessions||[]),day=today();
        const daily=days.map(day=>({day,seconds:sessions.reduce((sum,session)=>sum+contribution(session,day),0)}));
        const subjects=new Map();for(const session of sessions){const seconds=days.reduce((sum,day)=>sum+contribution(session,day),0);if(seconds)subjects.set(session.subject||'未分类',(subjects.get(session.subject||'未分类')||0)+seconds);}
        const attempts=(state.reviewAttempts||[]).filter(item=>item.phase!=='initial'&&item.time&&PrepLocal.dateText(new Date(item.time))>=start&&PrepLocal.dateText(new Date(item.time))<=end);
        const wrong=new Map();for(const attempt of attempts.filter(item=>item.correct===false))for(const point of attempt.knowledgePoints?.length?attempt.knowledgePoints:['未标注知识点'])wrong.set(point,(wrong.get(point)||0)+1);
        const tasks=(state.tasks||[]).filter(task=>task.date>=start&&task.date<=end&&!['cancelled','unknown'].includes(YantuActivity.taskStatus(task)));
        const backlog=(state.tasks||[]).filter(task=>YantuActivity.taskIsActive(task)&&task.date&&(task.date<day||task.carried));
        return {start,end,daily,total:daily.reduce((sum,day)=>sum+day.seconds,0),subjects:[...subjects],attempts,tasks,backlog,repeated:[...wrong].filter(([,count])=>count>=2).sort((a,b)=>b[1]-a[1])};
    }
    function report(){
        if(!el('weeklyReport'))return;
        week||=monday(today());el('reportWeek').value=week;
        const data=summary(week),max=Math.max(60,...data.daily.map(day=>day.seconds));
        el('reportRange').textContent=data.start+' 至 '+data.end;
        el('reportTotals').innerHTML=`<span>记录专注 <strong>${duration(data.total)}</strong></span><span>本周安排已完成 <strong>${data.tasks.filter(task=>task.done).length} / ${data.tasks.length}</strong></span><span>复习作答 <strong>${data.attempts.length} 次</strong></span><span>当前积压 <strong>${data.backlog.length} 项</strong></span>`;
        el('reportDaily').innerHTML=data.daily.map(day=>`<div><span>${day.day.slice(5)}</span><div class="report-bar-track"><i style="width:${Math.max(0,day.seconds/max*100)}%"></i></div><strong>${duration(day.seconds)}</strong></div>`).join('');
        el('reportSubjects').innerHTML=data.subjects.length?data.subjects.sort((a,b)=>b[1]-a[1]).map(([subject,seconds])=>`<span>${esc(subject)} <strong>${duration(seconds)}</strong></span>`).join(''):'<span class="report-empty">本周尚无计时记录</span>';
        const points=el('reportRepeated');points.replaceChildren();
        for(const [point,count] of data.repeated){const button=document.createElement('button');button.className='btn btn-ghost btn-sm';button.textContent=point+' · 做错 '+count+' 次';button.onclick=()=>search(point);points.append(button);}
        if(!data.repeated.length)points.innerHTML='<span class="report-empty">本周没有同一知识点重复做错的记录</span>';
        el('reportBacklog').replaceChildren();
        for(const task of data.backlog.slice(0,8)){const row=document.createElement('div');row.className='report-backlog-row';row.innerHTML=`<time>${esc(task.originalDate||task.date)}</time><span>${esc(task.title)}${task.carried?' · 延续任务':''}</span>`;el('reportBacklog').append(row);}
        if(!data.backlog.length)el('reportBacklog').innerHTML='<span class="report-empty">当前没有积压任务</span>';
        el('reportNext').textContent=data.repeated.length?'建议优先复习：'+data.repeated.slice(0,3).map(([point])=>point).join('、'):data.backlog.length?'建议先检查积压任务，按当前可用时间重新安排。':'保持当前节奏，下次作答后再观察薄弱知识点。';
    }
    function search(point){PrepPush.openView('mistakes');const input=document.querySelector('.library-shell[data-col=mistakes] [data-field=q]');input.value=point==='未标注知识点'?'':point;input.dispatchEvent(new Event('input',{bubbles:true}));}
    function setup(){
        const homeBand=document.createElement('section');homeBand.id='homeStudyFocus';homeBand.className='home-study-focus';homeBand.innerHTML='<div><h3>今日必做</h3><strong id="homeMustDo"></strong><div id="homeMustDoItems"></div><span>今日记录专注 <b id="homeFocusActual"></b></span></div><div><h3>到期错题 <span id="homeDueCount"></span></h3><div id="homeDueItems"></div></div><div><h3>继续上次学习</h3><p id="homeResumeTitle"></p><button class="btn btn-ghost btn-sm" id="homeResume"><i class="fas fa-play"></i> 继续</button></div>';
        el('view-overview').prepend(homeBand);el('homeResume').onclick=()=>{if(last)StudyFlow.go(last.id);};
        const section=document.createElement('section');section.id='weeklyReport';section.className='weekly-report';section.innerHTML='<header><h3><i class="fas fa-chart-column"></i> 学习周报</h3><div class="report-week-picker"><button class="btn btn-icon btn-ghost" id="reportPrevious" title="上一周" aria-label="上一周"><i class="fas fa-chevron-left"></i></button><input type="date" id="reportWeek" aria-label="选择周内日期" /><button class="btn btn-icon btn-ghost" id="reportFollowing" title="下一周" aria-label="下一周"><i class="fas fa-chevron-right"></i></button></div></header><p id="reportRange"></p><div id="reportTotals" class="report-totals"></div><div class="report-grid"><section><h4>实际专注时长</h4><div id="reportDaily"></div><div id="reportSubjects"></div></section><section><h4>反复出错的知识点</h4><div id="reportRepeated"></div><h4>当前积压任务</h4><div id="reportBacklog"></div></section></div><p id="reportNext"></p>';
        el('view-review').append(section);el('reportPrevious').onclick=()=>{week=addDays(week,-7);report();};el('reportFollowing').onclick=()=>{week=addDays(week,7);report();};el('reportWeek').onchange=event=>{if(event.target.value){week=monday(event.target.value);report();}};
        const label=document.createElement('label');label.className='focus-duration focus-subject';label.innerHTML='专注科目<select id="focusSubject"><option>未分类</option><option>数学</option><option>英语</option><option>政治</option><option>408</option></select>';el('focusMinutes').closest('label').after(label);
        const activity=document.createElement('label');activity.className='focus-duration focus-subject';activity.innerHTML='统计归属<select id="focusActivity"><option value="unclassified">未分类</option><option value="exam">考研</option><option value="skill">算法技能</option><option value="project">项目</option></select>';label.after(activity);
        el('focusActivity').onchange=event=>{if(state.focusTimer?.running)return;state.focusTimer||={minutes:25,remaining:1500,running:false,endAt:null};state.focusTimer.activityType=YantuActivity.activityType(event.target.value);saveState();};
        el('focusSubject').onchange=event=>{if(state.focusTimer?.running)return;state.focusTimer||={minutes:25,remaining:1500,running:false,endAt:null};state.focusTimer.subject=event.target.value;saveState();};
        let day=today();setInterval(()=>{if(document.hidden)return;const select=el('focusSubject'),disabled=Boolean(state.focusTimer?.running),subject=state.focusTimer?.subject||'未分类';if(select.disabled!==disabled)select.disabled=disabled;if(document.activeElement!==select&&select.value!==subject)select.value=subject;const category=el('focusActivity');category.disabled=disabled;if(document.activeElement!==category)category.value=YantuActivity.activityType(state.focusTimer?.activityType);const current=today();if(current!==day){day=current;home();}},1000);
        addEventListener('yantu-sync',()=>{if(document.body.dataset.view==='overview'&&!document.hidden)home();});
        addEventListener('study-detail',event=>{last={id:event.detail.id,col:event.detail.col};YantuEnvironment.storage.setItem('yantu-last-study',JSON.stringify(last));});
        home();report();
    }
    const previousReady=PrepLocal.ready;PrepLocal.ready=()=>{previousReady();setup();};
    const previousOverview=renderOverview;renderOverview=()=>{previousOverview();home();};
    const previousReview=renderReview;renderReview=()=>{previousReview();report();};
    window.StudyReport={summary,home,report};
})();
