import { WebAssistantPanel } from '../WebAssistantPanel/WebAssistantPanel.js?v=1f08ad808b467825';
import { xiaoYanAvatar } from '../XiaoYan/XiaoYan.js?v=1f08ad808b467825';
import { LocalAssistantApi } from '../LocalAssistantApi/LocalAssistantApi.js?v=1f08ad808b467825';
import { openSpeechSettings } from '../SpeechSettings/SpeechSettings.js?v=1f08ad808b467825';
import { PersonalAgent } from '../PersonalAgent/PersonalAgent.js?v=1f08ad808b467825';
import { openPreferenceSettings } from '../PersonalAgent/PreferenceSettings.js?v=1f08ad808b467825';
import { assistantInference } from '../PersonalAgent/AssistantInference.js?v=1f08ad808b467825';
import { conversationStore, preferenceSite } from '../PersonalAgent/PreferenceStore.js?v=1f08ad808b467825';
import { runAgentTurn } from '../PersonalAgent/AgentTurnDag.js?v=1f08ad808b467825';
import { utf8Bytes } from '../PersonalAgent/ContextWindow.js?v=1f08ad808b467825';
const node=(tag,text='',className='')=>{const e=document.createElement(tag);e.textContent=text;if(className)e.className=className;return e;};
const token=()=>Array.from(crypto.getRandomValues(new Uint8Array(24)),n=>n.toString(16).padStart(2,'0')).join('');
/** 小研的两个入口共享本机对话目录，已有的两类会话各自保留。 */
export class AssistantPanel {
  constructor(host,{surface='sidebar',close}={}){
    this.host=host;this.surface=surface;this.close=close;this.api=new LocalAssistantApi();this.thread='';this.generation='';this.polling=false;this.disposed=false;this.shortcut='Alt+j';
    const css=document.createElement('link');css.rel='stylesheet';css.href='/phase11/shared/AssistantPanel/AssistantPanel.css?v=1f08ad808b467825';document.head.append(css);
    host.classList.add('private-assistant');
    host.innerHTML=`
      <header class="assistant-heading">
        <div><strong>小研</strong><small>Windows 私人空间</small></div>
        <a class="assistant-mode" href="/phase11/assistant/?mode=web">网页对话</a>
        <a data-full-view href="/phase11/assistant/">展开对话</a>
        <button type="button" data-close aria-label="关闭小研">×</button>
      </header>
      <p class="assistant-status" role="status">正在连接本机私人空间…</p>
      <nav class="assistant-tabs" aria-label="小研私人空间">
        <button data-tab="chat">对话</button><button data-tab="documents">资料</button>
        <button data-tab="memories">记忆</button><button data-tab="settings">设置</button>
      </nav>
      <section data-area="chat">
        <div class="assistant-threadbar">
          <select aria-label="私人会话"></select>
          <button data-new type="button">新对话</button><button data-delete-thread type="button">删除</button>
          <button data-model-settings type="button">模型连接</button>
          <button data-personalization type="button" hidden>个性化记忆</button>
        </div>
        <div class="assistant-messages" aria-live="polite"></div>
        <div class="assistant-citations"></div>
        <form class="assistant-composer">
          <textarea required maxlength="8000" rows="3" placeholder="想聊点什么？引用资料会先在本机检索" aria-label="给小研的私人消息"></textarea>
          <footer><small>Enter 发送 · Shift+Enter 换行</small><button type="button" data-stop disabled>停止</button><button type="submit">发送</button></footer>
        </form>
      </section>
      <section data-area="documents" hidden>
        <h3>只留在这台电脑的资料</h3><p>支持 TXT、Markdown、文本 PDF、DOCX。检索使用中文词法索引。</p>
        <label class="assistant-consent"><input data-consent type="checkbox">允许将检索命中的必要片段发送到我的 Linux AI 后端</label>
        <input type="file" data-import accept=".txt,.md,.pdf,.docx" aria-label="导入私人资料"><div data-documents></div>
      </section>
      <section data-area="memories" hidden>
        <h3>你决定小研记住什么</h3><p>小研的对话页和侧栏共用这些本机记忆。</p>
        <form data-memory-form><textarea required maxlength="2000" rows="3" placeholder="例如：我正在学习 C++ 网络服务器" aria-label="新增长期记忆"></textarea><button type="submit">确认并记住</button></form>
        <div data-memories></div>
      </section>
      <section data-area="settings" hidden>
        <h3>小研的私人模型</h3><label>Linux AI 账号 ID<input data-ai inputmode="numeric" pattern="[0-9]+" placeholder="填写已绑定 ONNX 的 AI 账号"></label>
        <p>私人推理发往自己的 Linux ONNX 后端，对话保存在 Windows。本站模型、本机 Ollama 和自带 API 可在「网页对话」中切换。</p>
        <label>侧栏快捷键<input data-shortcut value="Alt+j" maxlength="32"></label><button data-save-settings type="button">保存设置</button>
      </section>`;
    host.querySelector('.assistant-heading').prepend(xiaoYanAvatar());
    host.querySelector('[data-full-view]').hidden = !close;
    this.status=host.querySelector('.assistant-status');this.list=host.querySelector('.assistant-messages');this.select=host.querySelector('.assistant-threadbar select');this.input=host.querySelector('.assistant-composer textarea');
    const migration=node('details','','assistant-migration');migration.innerHTML='<summary>迁移旧服务器的私人资料与AI历史</summary><p>先复制并校验。原文件/训练数据/备份不在这项文字迁移的清除范围内，含附件的分页会保留原副本。</p><button data-migration-list type="button">读取本人可迁移项目</button><select data-migration-resource aria-label="迁移来源"></select><button data-migration-copy type="button" disabled>复制并核验下一页</button><p data-migration-progress></p><div data-migration-pages></div>';
    host.querySelector('[data-area=settings]').append(migration);this.migrationCursor=new Map();
    const speechButton=node('button','音色、云语音与试听');speechButton.type='button';speechButton.onclick=()=>openSpeechSettings(this.api).catch(e=>this.error(e));host.querySelector('[data-area=settings]').prepend(speechButton);
    migration.querySelector('[data-migration-list]').onclick=()=>this.loadMigration().catch(e=>this.error(e));
    migration.querySelector('[data-migration-resource]').onchange=()=>{migration.querySelector('[data-migration-copy]').disabled=false;};
    migration.querySelector('[data-migration-copy]').onclick=async()=>{const button=migration.querySelector('[data-migration-copy]');button.disabled=true;try{const key=migration.querySelector('[data-migration-resource]').value;if(!key)throw new Error('先选择迁移来源');const[kind,resourceId]=key.split(':');const cursor=this.migrationCursor.get(key)||{after:'0',upper:'0'};const result=await this.api.request('/migration/import',{method:'POST',data:{kind,resourceId,...cursor}});this.migrationCursor.set(key,{after:result.through,upper:result.upper});migration.querySelector('[data-migration-progress]').textContent=`已复制并核验至 ${result.through} / ${result.upper}，服务器副本仍保留。`;button.disabled=BigInt(result.through)>=BigInt(result.upper);await this.loadMigrationPages();}catch(e){button.disabled=false;this.error(e);}};
    const toolsTab=node('button','行动');toolsTab.dataset.tab='tools';host.querySelector('.assistant-tabs').append(toolsTab);
    const tools=node('section');tools.dataset.area='tools';tools.hidden=true;
    tools.innerHTML='<h3>先起草，再由你确认</h3><p>模型只能建议行动。确认卡固定目标与内容，重复确认沿用同一操作编号。</p><div class="assistant-query-tools"><button data-query="query_my_profile">查看本人资料</button><button data-query="query_visible_conversations">查看可访问会话</button><button data-open-page>打开个人主页</button></div><pre data-query-result></pre><form data-action-draft><label>行动类型<select name="tool"><option value="send_message">发送消息</option><option value="publish_activity">发布个人动态</option><option value="update_profile">更新本人资料</option></select></label><label data-target-label>会话 ID<input name="target" inputmode="numeric"></label><label data-name-label hidden>显示名称<input name="displayName" maxlength="64"></label><label>内容<textarea name="body" rows="4" maxlength="2000" required></textarea></label><p>动态草稿默认仅好友可见，不自动发布到神树。要公开发布，可打开统一发布器选择范围。</p><button type="submit">创建确认卡</button></form><div data-action-cards></div>';
    host.append(tools);
    tools.querySelectorAll('[data-query]').forEach(button=>button.onclick=async()=>{try{const result=await this.api.request('/tools/query',{method:'POST',data:{tool:button.dataset.query}});tools.querySelector('[data-query-result]').textContent=JSON.stringify(result,null,2);}catch(e){this.error(e);}});
    tools.querySelector('[data-open-page]').onclick=()=>this.api.request('/tools/query',{method:'POST',data:{tool:'open_page',page:'/phase11/profile/'}}).then(result=>location.assign(result.path)).catch(e=>this.error(e));
    const actionForm=tools.querySelector('[data-action-draft]');actionForm.elements.tool.onchange=()=>{tools.querySelector('[data-target-label]').hidden=actionForm.elements.tool.value!=='send_message';tools.querySelector('[data-name-label]').hidden=actionForm.elements.tool.value!=='update_profile';};
    actionForm.onsubmit=async event=>{event.preventDefault();try{const tool=actionForm.elements.tool.value,body=actionForm.elements.body.value;
      const args=tool==='send_message'?{conversationId:actionForm.elements.target.value,text:body}:tool==='update_profile'?{displayName:actionForm.elements.displayName.value,biography:body}:{body,categoryId:'0',destination:'profile',visibility:'friends',confirmPublic:'false'};
      await this.api.request('/tools/proposals',{method:'POST',data:{tool,arguments:args}});await this.loadActions();}catch(e){this.error(e);}};
    host.querySelector('[data-close]').onclick=()=>close?.();if(!close)host.querySelector('[data-close]').hidden=true;
    host.querySelectorAll('[data-tab]').forEach(button=>button.onclick=()=>this.tab(button.dataset.tab));this.tab('chat');
    this.select.onchange=()=>{this.thread=this.select.value;this.rememberThread();this.loadMessages().catch(e=>this.error(e));};
    host.querySelector('[data-model-settings]').onclick=()=>this.tab('settings');
    host.querySelector('[data-new]').onclick=()=>this.newThread().catch(e=>this.error(e));
    host.querySelector('[data-delete-thread]').onclick=async()=>{
      const thread=this.thread;
      if(this.sending||!thread||!confirm('删除本机这个助手会话及其历史？'))return;
      try{
        await this.api.request('/threads/'+thread,{method:'DELETE'});
        if(this.agent)await conversationStore.remove(this.agent.scope,thread).catch(()=>{});
        this.thread='';await this.loadThreads();
      }catch(e){this.error(e);}
    };
    host.querySelector('.assistant-composer').onsubmit=event=>{event.preventDefault();this.send().catch(e=>this.error(e));};
    this.input.onkeydown=event=>{if(event.key==='Enter'&&!event.shiftKey&&!event.isComposing){event.preventDefault();host.querySelector('.assistant-composer').requestSubmit();}};
    host.querySelector('[data-stop]').onclick=()=>this.stop().catch(e=>this.error(e));
    host.querySelector('[data-import]').onchange=event=>this.importFile(event.target.files[0]).catch(e=>this.error(e));
    host.querySelector('[data-memory-form]').onsubmit=async event=>{event.preventDefault();const input=event.currentTarget.querySelector('textarea');try{await this.api.request('/memories',{method:'POST',data:{text:input.value,confirmed:true}});input.value='';await this.loadMemories();}catch(e){this.error(e);}};
    host.querySelector('[data-save-settings]').onclick=async()=>{try{const shortcut=host.querySelector('[data-shortcut]').value;if(!/^(Alt|Ctrl)\+[a-z0-9]$/i.test(shortcut))throw new Error('快捷键格式为 Alt+j 或 Ctrl+字母');await this.api.request('/settings',{method:'PATCH',data:{aiUserId:host.querySelector('[data-ai]').value,shortcut}});this.shortcut=shortcut;this.status.textContent='设置已保存到本机';window.dispatchEvent(new CustomEvent('nebula:assistant-shortcut',{detail:shortcut}));}catch(e){this.error(e);}};
  }
  error(error){this.status.textContent=error.message;this.status.classList.add('error');if(error.status===409||error.status===401){this.polling=false;this.generation='';}}
  start(){
    if(this.web)return this.web.start();
    if(this.ready||this.disposed)return Promise.resolve();
    this.loading??=this.initialize().finally(()=>{this.loading=null;});
    return this.loading;
  }
  async initialize(){try{
      const publicMode=new URL(location.href).searchParams.get('mode')==='web';
      const result=await this.api.start().catch(error=>{
        if(publicMode)return {available:false};
        throw error;
      });
      if(this.disposed)return;
      const site=preferenceSite(this.api.session);
      if(publicMode||!result.available){this.web=new WebAssistantPanel(this.host,{close:this.close,hasPrivateSpace:result.available,site});return this.web.start();}
      this.host.querySelectorAll('input,textarea,select,button').forEach(e=>e.disabled=false);this.host.querySelector('[data-stop]').disabled=true;
      if(result.preferenceInference&&site){
        this.agent=new PersonalAgent(this.api.account,'private',{site});
        this.contextCompaction=Boolean(result.contextCompaction);
        const button=this.host.querySelector('[data-personalization]');
        button.hidden=false;
        button.onclick=()=>{if(!this.preferenceDialog?.open)this.preferenceDialog=openPreferenceSettings(this.agent,{local:true});};
      }
      const settings=await this.api.request('/settings');
      if(this.disposed)return;
      this.status.classList.remove('error');this.status.textContent='已连接 Windows 私人空间';this.host.querySelector('[data-ai]').value=settings.aiUserId||'';this.shortcut=settings.shortcut||'Alt+j';this.host.querySelector('[data-shortcut]').value=this.shortcut;
      try{const models=await this.api.request('/models');if(this.disposed)return;let choice=this.host.querySelector('[data-model-choice]');if(!choice){choice=document.createElement('select');choice.dataset.modelChoice='';choice.setAttribute('aria-label','选择已配置的私人AI');this.host.querySelector('[data-ai]').after(choice);}choice.replaceChildren(node('option','选择已配置的私人 AI'));choice.firstChild.value='';for(const model of models){const option=node('option',`${model.name} · ${model.runtime}（已配置）`);option.value=model.accountId;choice.append(option);}choice.onchange=()=>{if(choice.value)this.host.querySelector('[data-ai]').value=choice.value;};if(!settings.aiUserId&&models.length)this.host.querySelector('[data-ai]').value=models[0].accountId;choice.value=this.host.querySelector('[data-ai]').value;}catch{/* 旧服务器无模型目录时可填写账号。 */}
      await this.loadThreads();
      this.ready=!this.disposed;
    }catch(e){if(!this.disposed)this.error(e);}}
  tab(name){this.host.querySelectorAll('[data-area]').forEach(e=>e.hidden=e.dataset.area!==name);this.host.querySelectorAll('[data-tab]').forEach(e=>e.classList.toggle('active',e.dataset.tab===name));if(name==='documents')this.loadDocuments().catch(e=>this.error(e));if(name==='memories')this.loadMemories().catch(e=>this.error(e));if(name==='tools')this.loadActions().catch(e=>this.error(e));}
  async loadActions(){if(!this.api.account)return;const items=await this.api.request('/tools/proposals'),host=this.host.querySelector('[data-action-cards]');host.replaceChildren();for(const item of items){const card=node('article','','assistant-item');card.append(node('strong',({send_message:'发送消息',publish_activity:'发布好友可见动态',update_profile:'修改本人资料'})[item.tool]||item.tool),node('pre',JSON.stringify(item.arguments,null,2)),node('small','状态：'+item.state));if(['pending','retryable'].includes(item.state)){const confirm=node('button',item.state==='retryable'?'核对并用原编号重试':'确认执行');confirm.onclick=async()=>{confirm.disabled=true;try{await this.api.request('/tools/confirm',{method:'POST',data:{id:item.id,digest:item.digest}});await this.loadActions();this.status.textContent='操作已由服务器确认';}catch(e){this.error(e);confirm.disabled=false;}};const cancel=node('button','取消');cancel.onclick=()=>this.api.request('/tools/cancel',{method:'POST',data:{id:item.id}}).then(()=>this.loadActions()).catch(e=>this.error(e));card.append(confirm,cancel);}host.append(card);}}
  async loadMigration(){const items=await this.api.request('/migration/resources');const select=this.host.querySelector('[data-migration-resource]');select.replaceChildren(...items.map(item=>{const option=node('option',`${item.kind==='knowledge'?'资料':'AI对话'} · ${item.title}`);option.value=item.kind+':'+item.id;return option;}));this.host.querySelector('[data-migration-copy]').disabled=!items.length;await this.loadMigrationPages();}
  async loadMigrationPages(){const items=await this.api.request('/migration/pages');const host=this.host.querySelector('[data-migration-pages]');host.replaceChildren();for(const item of items){const card=node('article','','assistant-item');card.append(node('strong',`${item.kind==='knowledge'?'资料':'AI对话'} #${item.resource} · ${item.after}—${item.through}`),node('p',item.purged?'已核验，本页服务器文字正文已清除':item.attachments?'已核验；包含附件，服务器副本保留':'已核验；服务器副本保留'));if(!item.purged&&!item.attachments){const remove=node('button','另行清除本页服务器副本');remove.onclick=async()=>{if(!confirm('本机归档会再次校验，然后清除这一页的服务器文字正文。此操作不处理训练数据、附件或备份。确认继续？'))return;try{await this.api.request('/migration/purge',{method:'POST',data:{receiptId:item.id,confirmed:true}});await this.loadMigrationPages();}catch(e){this.error(e);}};card.append(remove);}host.append(card);}}
  rememberThread(){try{sessionStorage.setItem('nebula:xiaoyan:windows:'+this.api.account,this.thread);}catch{}}
  async loadThreads(){
    const [primary, legacy] = await Promise.all(['friend','sidebar'].map(surface=>this.api.request('/threads?surface='+surface)));
    if(this.disposed)return;
    const threads=[...primary,...legacy];
    const groups=[];
    for(const [items,label] of [[primary,'小研对话'],[legacy,'原侧栏历史']]){
      if(!items.length)continue;
      const group=document.createElement('optgroup');group.label=label;
      group.append(...items.map(thread=>new Option(thread.title,thread.id)));groups.push(group);
    }
    this.select.replaceChildren(...groups);
    let remembered='';try{remembered=sessionStorage.getItem('nebula:xiaoyan:windows:'+this.api.account)||'';}catch{}
    this.thread=[this.thread,remembered].find(id=>id&&threads.some(thread=>thread.id===id))||threads[0]?.id||'';
    this.select.value=this.thread;
    if(!this.thread){this.list.replaceChildren(node('div','和小研开始第一段对话。','assistant-empty'));return;}
    await this.loadMessages();
  }
  async newThread(){const created=await this.api.request('/threads',{method:'POST',data:{surface:'friend',title:'新对话 '+new Date().toLocaleDateString()}});this.thread=created.id;this.rememberThread();await this.loadThreads();}
  bubble(message){const article=node('article','',`assistant-bubble ${message.role}`);article.dataset.request=message.requestId;article.append(node('small',message.role==='user'?'我':'小研'),node('p',message.body||'正在思考…'),node('small',({completed:'',streaming:'生成中',failed:'生成未完成',cancelled:'已停止'})[message.state]||message.state));return article;}
  async loadMessages(){const thread=this.thread;const messages=await this.api.request('/threads/'+thread+'/messages');if(this.thread!==thread||this.disposed)return;this.list.replaceChildren(...messages.map(m=>this.bubble(m)));if(!messages.length)this.list.append(node('div','没有历史消息。写下第一个问题吧。','assistant-empty'));this.list.scrollTop=this.list.scrollHeight;const pending=messages.find(m=>m.role==='assistant'&&m.state==='streaming');if(pending&&!this.polling&&!this.sending){this.generation=pending.requestId;this.poll();}}
  inferPreferences(model, messages, { signal, maxTokens }) {
    const path='/preference-inference';
    return assistantInference({
      start:(data,signal)=>this.api.request(path,{method:'POST',data,signal}),
      poll:(id,after,signal)=>this.api.request(path+'/'+id+'?after='+after,{signal}),
      stop:id=>this.api.request(path+'/'+id,{method:'DELETE'}),
    },messages,{model,signal,maxTokens});
  }

  async send(){
    if(this.generation||this.sending)throw new Error('请等待当前回答完成，或先停止生成');
    const text=this.input.value.trim();
    if(!text)return;
    if(utf8Bytes(text)>8192)throw new Error('单条私人消息上限为 8 KiB，请将这条消息拆分。');
    const aiUserId=this.host.querySelector('[data-ai]').value;
    if(!aiUserId){this.tab('settings');throw new Error('请先选择已绑定 ONNX 的私人模型');}
    this.sending=true;
    this.select.disabled=true;
    const stop=this.host.querySelector('[data-stop]');
    const actions=[...this.host.querySelectorAll('[data-new],[data-delete-thread]')];
    actions.forEach(button=>{button.disabled=true;});
    const controller=new AbortController();
    this.turnController=this.preparing=controller;
    stop.disabled=false;
    try{
      if(!this.thread)await this.newThread();
      if(this.disposed)return;
      const requestId=token(),threadId=this.thread;
      const infer=(messages,options)=>this.inferPreferences(aiUserId,messages,options);
      this.status.textContent='小研正在准备本轮上下文…';
      const result=await runAgentTurn({
        agent:this.agent,text,id:requestId,mode:'private',infer,
        readHistory:async signal=>{
          if(!this.contextCompaction)return [];
          const rows=await this.api.request('/threads/'+threadId+'/messages?limit=30',{signal});
          return rows.filter(row=>row.state==='completed').map(row=>({sequence:row.id,role:row.role,content:row.body}));
        },
        readCheckpoint:()=>this.contextCompaction?conversationStore.read(this.agent.scope,threadId).catch(()=>null):null,
        saveCheckpoint:checkpoint=>conversationStore.save(this.agent.scope,threadId,checkpoint),
        onStage:stage=>{
          if(stage==='compacting')this.status.textContent='小研正在压缩旧对话…';
          if(stage==='answering')this.status.textContent='小研正在回答…';
        },
        generate:async(plan,signal)=>{
          this.preparing=null;
          stop.disabled=true;
          const data={threadId,requestId,text,aiUserId};
          if(plan.guidance)data.personalization=plan.guidance;
          if(plan.maxTokens!==512)data.maxOutputTokens=String(plan.maxTokens);
          if(plan.checkpoint){
            data.historyThrough=plan.checkpoint.through;
            data.historySummary=plan.checkpoint.summary;
          }
          const started=await this.api.request('/generations',{method:'POST',data});
          signal.throwIfAborted();
          this.generation=started.id;
          this.input.value='';
          await this.loadMessages();
          return this.poll(signal);
        },
      },controller.signal);
      if(!this.disposed&&result)this.status.textContent=result.error
        ||({cancelled:'已停止',failed:'生成未完成'})[result.state]
        ||(result.compacted?'旧对话已压缩。':'')+'回答已保存在本机';
    }catch(error){
      if(error.name!=='AbortError')throw error;
      if(!this.disposed)this.status.textContent='已停止回复';
    }finally{
      this.preparing=null;
      this.turnController=null;
      this.sending=false;
      actions.forEach(button=>{button.disabled=false;});
      if(!this.generation){this.select.disabled=false;stop.disabled=true;}
    }
  }
  async poll(signal){if(this.polling||!this.generation||this.disposed)return;this.polling=true;const id=this.generation;this.select.disabled=true;this.host.querySelector('[data-stop]').disabled=false;
    try{while(this.generation===id&&!this.disposed){const result=await this.api.request('/generations/'+id,{signal});const bubble=[...this.list.querySelectorAll('.assistant-bubble.assistant')].find(e=>e.dataset.request===id);if(bubble){bubble.querySelector('p').textContent=result.body||'正在思考…';bubble.lastChild.textContent=result.state==='streaming'?'生成中':result.state==='cancelled'?'已停止':result.state==='failed'?'生成失败':'';}
        const citations=this.host.querySelector('.assistant-citations');citations.replaceChildren(...(result.citations||[]).map(c=>{const details=node('details');details.append(node('summary','引用 · '+c.title),node('p',c.excerpt));return details;}));
        if(result.state!=='streaming'){
          this.generation='';
          if(!signal)this.status.textContent=result.error||({cancelled:'已停止',failed:'生成未完成'})[result.state]||'回答已保存在本机';
          return result;
        }
        await new Promise(resolve=>setTimeout(resolve,400));}
    }catch(e){if(signal)throw e;this.error(e);}finally{this.polling=false;this.select.disabled=Boolean(this.generation);const stop=this.host.querySelector('[data-stop]');if(stop)stop.disabled=!this.generation;}}
  async stop(){if(this.preparing){this.preparing.abort();return;}if(!this.generation)return;await this.api.request('/generations/'+this.generation,{method:'DELETE'});this.status.textContent='正在停止生成并回收任务…';}
  async importFile(file){if(!file)return;if(file.size>10*1024*1024)throw new Error('单份资料最多 10 MiB');const extension='.'+file.name.split('.').pop().toLowerCase();this.status.textContent='正在本机解析和建立索引…';await this.api.request('/documents/import',{method:'POST',body:file,headers:{'Content-Type':'application/octet-stream','X-Document-Extension':extension,'X-Document-Authorized':String(this.host.querySelector('[data-consent]').checked)}});this.status.textContent='原文与索引已保存在本机';await this.loadDocuments();}
  async loadDocuments(){if(!this.api.account)return;const items=await this.api.request('/documents'),host=this.host.querySelector('[data-documents]');host.replaceChildren();for(const d of items){const card=node('article','', 'assistant-item');card.append(node('strong',d.title));const label=node('label','允许检索片段发送到自有后端');const checkbox=document.createElement('input');checkbox.type='checkbox';checkbox.checked=d.authorized;label.prepend(checkbox);checkbox.onchange=()=>this.api.request('/documents/'+d.id,{method:'PATCH',data:{enabled:true,authorized:checkbox.checked}}).catch(e=>this.error(e));const remove=node('button','删除原文与索引');remove.onclick=async()=>{if(!confirm('删除这份本机资料？'))return;try{await this.api.request('/documents/'+d.id,{method:'DELETE'});await this.loadDocuments();}catch(e){this.error(e);}};card.append(label,remove);host.append(card);}if(!items.length)host.append(node('p','还没有导入资料。'));}
  async loadMemories(){if(!this.api.account)return;const items=await this.api.request('/memories'),host=this.host.querySelector('[data-memories]');host.replaceChildren();for(const memory of items){const card=node('article','','assistant-item'),input=node('textarea');input.value=memory.text;input.maxLength=2000;const enabled=document.createElement('input');enabled.type='checkbox';enabled.checked=memory.enabled;const label=node('label','启用');label.prepend(enabled);const save=node('button','保存修改');save.onclick=()=>this.api.request('/memories/'+memory.id,{method:'PATCH',data:{text:input.value,enabled:enabled.checked}}).then(()=>this.status.textContent='记忆已更新').catch(e=>this.error(e));const remove=node('button','删除');remove.onclick=()=>this.api.request('/memories/'+memory.id,{method:'DELETE'}).then(()=>this.loadMemories()).catch(e=>this.error(e));card.append(input,label,save,remove);host.append(card);}}
  dispose(){this.disposed=true;this.turnController?.abort();this.agent?.dispose();this.preferenceDialog?.close();this.web?.dispose();}
}
export function mountGlobalAssistant(navigation,{fullPage=false}={}){
  const button=node('button','','workspace-assistant');button.title='小研 · Alt+J';button.setAttribute('aria-label','打开小研');button.type='button';button.append(xiaoYanAvatar('xiaoyan-nav-portrait'));navigation.append(button);
  if(fullPage){button.onclick=()=>document.querySelector('#private-friend .assistant-composer textarea')?.focus();return;}
  button.setAttribute('aria-expanded','false');button.setAttribute('aria-controls','global-assistant-drawer');
  const drawer=node('aside','','assistant-drawer');drawer.id='global-assistant-drawer';drawer.hidden=true;drawer.setAttribute('aria-label','小研侧栏');document.body.append(drawer);let panel,shortcut='Alt+j';
  const close=()=>{drawer.hidden=true;button.setAttribute('aria-expanded','false');};
  const show=async ({settings=false}={})=>{
    drawer.hidden=false;button.setAttribute('aria-expanded','true');
    if(!panel)panel=new AssistantPanel(drawer,{surface:'sidebar',close});
    await panel.start();shortcut=panel.shortcut;
    if(settings&&!drawer.hidden){
      if(panel.web)panel.web.connections();
      else panel.tab('settings');
    }
  };
  const toggle=()=>drawer.hidden?show():close();
  button.onclick=toggle;window.addEventListener('nebula:assistant-shortcut',event=>shortcut=event.detail);
  window.addEventListener('nebula:assistant-settings',()=>show({settings:true}));
  const readShortcut=async()=>{try{const api=new LocalAssistantApi();const result=await api.start();if(result.available){const settings=await api.request('/settings');shortcut=settings.shortcut||'Alt+j';}}catch{}};
  window.addEventListener('nebula:user',event=>{
    const owner=panel?.web?.owner||panel?.api.account;
    if(owner&&String(event.detail?.id||'')!==owner){panel.dispose();panel=undefined;drawer.replaceChildren();close();}
    readShortcut();
  });readShortcut();
  document.addEventListener('keydown',event=>{const[modifier,key]=shortcut.toLowerCase().split('+');if(event.key.toLowerCase()===key&&(modifier==='alt'?event.altKey:event.ctrlKey)&&!event.shiftKey){event.preventDefault();toggle();}if(event.key==='Escape'&&!drawer.hidden){drawer.hidden=true;button.setAttribute('aria-expanded','false');}});
}
