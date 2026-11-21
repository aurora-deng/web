/* Windows / Edge RD against a running Linux Observer lab. */
const assert=require("node:assert/strict");
const fs=require("node:fs");
const path=require("node:path");
const {spawn}=require("node:child_process");
const base=process.env.OBSERVER_RD_URL || "http://127.0.0.1:18789";
const edgePath=process.env.OBSERVER_RD_EDGE || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
const work=path.resolve("build-function3","obs-qa","workspace-browser-"+Date.now());
const inspectTrace=process.env.OBSERVER_RD_TRACE;
const alertWindow=process.env.OBSERVER_RD_FUNCTION_WINDOW;
const sloWindow=process.env.OBSERVER_RD_SLO_WINDOW;
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(check,label) {
  const end=Date.now()+16000;
  do {if(await check())return;await sleep(120);} while(Date.now()<end);
  throw Error("Timed out: "+label);
}
const records=async()=>(await fetch(base+"/api/v1/workspace")).json();
async function update(route,data) {
  const response=await fetch(base+"/api/v1/workspace/"+route,{method:"POST",body:JSON.stringify(data),
    headers:{"Content-Type":"application/json","X-Observer-Workspace":"1"}});
  const content=await response.json();
  assert(response.ok,JSON.stringify(content));
  return content;
}

async function run(){
  const health=await (await fetch(base+"/health")).json();
  assert.equal(health.simulated,false);
  fs.mkdirSync(work,{recursive:true});
  const edge=spawn(edgePath,["--headless=new","--no-first-run","--disable-extensions","--user-data-dir="+work,
    "--remote-debugging-port=0","about:blank"],{stdio:"ignore",windowsHide:true});
  let socket;
  const title="RD Observer "+Date.now();
  const ruleTitle=title+" alert";
  const ownedBoards=new Set([title,title+" imported"]);
  try{
    const dev=path.join(work,"DevToolsActivePort");
    await until(()=>fs.existsSync(dev),"DevTools");
    const port=fs.readFileSync(dev,"utf8").split(/\r?\n/)[0];
    const tabs=await (await fetch("http://127.0.0.1:"+port+"/json/list")).json();
    socket=new WebSocket(tabs.find(x=>x.type==="page").webSocketDebuggerUrl);
    await new Promise((done,fail)=>{socket.onopen=done;socket.onerror=fail;});
    let seq=0;
    const calls=new Map(),errors=[];
    socket.onmessage=({data})=>{
      const event=JSON.parse(data);
      if(event.method==="Runtime.exceptionThrown")errors.push(event.params.exceptionDetails);
      if(!calls.has(event.id))return;
      const {resolve,reject}=calls.get(event.id);
      calls.delete(event.id);event.error?reject(event.error):resolve(event.result);
    };
    const call=(method,params={})=>new Promise((resolve,reject)=>{
      const id=++seq;calls.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));
    });
    const ev=async expression=>{
      const result=await call("Runtime.evaluate",{expression,returnByValue:true});
      assert(!result.exceptionDetails,result.exceptionDetails?.exception?.description);
      return result.result.value;
    };
    const phrase=(selector,label)=>"document.querySelector("+JSON.stringify(selector)+")?.textContent.includes("+JSON.stringify(label)+")";
    const expect=(selector,label)=>until(()=>ev(phrase(selector,label)),label);
    const click=(label,selector="button")=>ev("[...document.querySelectorAll("+JSON.stringify(selector)+")].find(e=>e.textContent.trim()==="+JSON.stringify(label)+")?.click()");
    const field=(label,value)=>ev("(()=>{const wrap=[...document.querySelectorAll('dialog[open] label')].find(e=>e.firstElementChild?.textContent.startsWith("+
      JSON.stringify(label)+"));const f=wrap.querySelector('input,select,textarea');f.value="+JSON.stringify(value)+";f.dispatchEvent(new Event('change'));})()");
    const view=async(name,search="")=>{await call("Page.navigate",{url:base+"/?view="+name+search});};
    const desktop=async(width,height)=>call("Emulation.setDeviceMetricsOverride",{width,height,deviceScaleFactor:1,mobile:false});
    await call("Page.enable");await call("Runtime.enable");
    await desktop(1440,900);
    await view("sources");
    await expect("#body","已连接的数据源");
    await click("添加数据源","#body button");
    await click("填入当前 NEBULA /metrics","dialog[open] button");
    await field("名称",title);
    await click("测试连接","dialog[open] button");
    await expect("dialog[open]","连接成功");
    await click("+ webserver_http_requests_total","dialog[open] button");
    await click("保存数据源","dialog[open] button");
    await expect("#body",title);
    const source=(await records()).sources.find(item=>item.name===title);
    assert(source);
    await view("boards");
    await expect("#body","Kubernetes 工作负载");
    await click("新建仪表盘","#body button");
    await field("名称",title);
    await click("创建","dialog[open] button");
    await expect("#body","添加面板");
    await click("添加面板","#body button");
    await field("面板名称","实时 HTTP 请求累计");
    await field("数据源",source.id);
    await click("保存面板","dialog[open] button");
    await expect("#body","实时 HTTP 请求累计");
    await click("添加面板","#body button");
    await field("面板名称","会话延迟与请求速率");
    await field("图形","area");
    await field("宽度","2");
    await click("添加查询","dialog[open] button");
    await field("查询 B","http_p99");
    await click("运行查询","dialog[open] button");
    await expect("dialog[open]","样本");
    await click("保存面板","dialog[open] button");
    await expect("#body","会话延迟与请求速率");
    await click("后移","#body button:not(:disabled)");
    await until(()=>ev("document.querySelector('.custom-panels h3')?.textContent === '会话延迟与请求速率'"),"reorder");
    await ev("(()=>{const f=document.getElementById('interval');f.value='5';f.dispatchEvent(new Event('change'));})()");
    await until(()=>ev("document.querySelector('.custom-panels .time-chart path')"),"chart");
    await ev("window.rdEmpty=0; window.rdWatch=new MutationObserver(()=>{if(!document.querySelector('.custom-panels .time-chart path'))window.rdEmpty++}); rdWatch.observe(document.querySelector('#body'),{childList:true});");
    const before=await ev("document.getElementById('status').textContent");
    await until(()=>ev("document.getElementById('status').textContent!=="+JSON.stringify(before)),"live refresh");
    assert.equal(await ev("window.rdEmpty"),0);
    await call("Page.setDownloadBehavior",{behavior:"allow",downloadPath:work});
    await click("导出 JSON","#body button");
    const exported=()=>fs.readdirSync(work).find(f=>/^observer-.*\.json$/.test(f));
    await until(exported,"export board JSON");
    const definition=JSON.parse(fs.readFileSync(path.join(work,exported()),"utf8"));
    assert.equal(definition.panels.length,2);
    assert(definition.sources.some(item=>item.id===source.id&&item.type==="metrics"));
    const moved="1".repeat(24), upload=path.join(work,"dashboard-from-another-instance.json");
    fs.writeFileSync(upload,JSON.stringify({...definition,title:title+" imported",
      panels:definition.panels.map(panel=>({...panel,source:panel.source===source.id?moved:panel.source})),
      sources:definition.sources.map(ref=>({...ref,id:ref.id===source.id?moved:ref.id}))}));
    await click("导入 JSON","#body button");
    const doc=await call("DOM.getDocument");
    const input=await call("DOM.querySelector",{nodeId:doc.root.nodeId,selector:"dialog[open] input[type=file]"});
    assert(input.nodeId);
    await call("DOM.setFileInputFiles",{nodeId:input.nodeId,files:[upload]});
    await expect("dialog[open]","dashboard-from-another-instance.json");
    await click("预览导入","dialog[open] button");
    await expect("dialog[open]","可导入 2 块");
    await field("数据源绑定 · "+title,source.id);
    await click("确认导入","dialog[open] button");
    await expect("#body",title+" imported");
    await expect("#body","实时 HTTP 请求累计");
    await view("distributed");
    await expect("#body","查询关联日志");
    await click("搜索链路","#body button");
    await expect("#body","本页按耗时排序");
    await ev("[...document.querySelectorAll('#body tr')].find(row=>row.textContent.includes('nebula-probe'))?.querySelector('button')?.click()");
    await expect("#body","查看同一时段的业务指标");
    await expect("#body","PostgreSQL command");
    await expect("#body","本链路调用边");
    if(inspectTrace) {
      await view("traces","&trace="+inspectTrace);
      await expect("#body","实际调用路线");
      await expect("#body","PostgresConversationRepository::listConversationSummaries");
      await click("↳ PostgreSQL command",".trace-tree-row button.trace-span-name");
      await expect(".trace-selected","父 Span");
      const screenshot=await call("Page.captureScreenshot",{format:"png"});
      fs.writeFileSync(path.join(work,"actual-trace.png"),Buffer.from(screenshot.data,"base64"));
      await view("overview");
      await until(()=>ev("document.querySelector('#bc option[value=lab]')"),"BC options");
      await ev("(()=>{const f=document.querySelector('#bc');f.value='lab';f.dispatchEvent(new Event('change'));})()");
      await until(()=>ev("document.querySelector('#app option[value=nebula]')"),"APP options");
      await ev("(()=>{const f=document.querySelector('#app');f.value='nebula';f.dispatchEvent(new Event('change'));})()");
      await until(()=>ev("[...document.querySelector('#instance').options].some(option=>option.value.startsWith('127.0.0.1:'))"),"INSTANCE options");
      await expect("#body","HTTP 请求速率");
      await view("explore");
      await expect("#body","Explore · PromQL 与直方图");
      await click("运行 PromQL","#body button");
      await expect("#body","conversations · 延迟桶");
      await until(()=>ev("document.querySelectorAll('.histogram-bin meter').length>=10"),"real buckets");
      await view("alerts");
      await expect("#body","业务 HTTP 5xx 比例");
      await expect("#body","数据库历史最大耗时");
      const managed=(await records()).boards.find(board=>board.provisioned==="nebula-lab.json");
      assert(managed);
      await view("boards","&board="+managed.id);
      await expect("#body","由文件管理");
      await expect("#body","会话列表耗时 P99");
    }
    if(alertWindow) {
      await view("alerts");
      await expect("#body","自定义规则");
      await click("新建告警规则","#body button");
      await field("常用起点","2");
      await field("规则名称",ruleTitle);
      await field("触发阈值","-1");
      await field("持续秒数","0");
      await field("对用户的影响与处理","Observer 浏览器验收临时规则。");
      await click("试算当前数据","dialog[open] button");
      await expect("dialog[open]","预览 · 已触发");
      await click("保存规则","dialog[open] button");
      await expect("#body",ruleTitle);
      const alerts=async()=>(await (await fetch(base+"/api/v1/alert-rules")).json()).rules;
      await until(async()=> (await alerts()).some(item=>item.name===ruleTitle&&item.result.state==="firing"),"persistent evaluated alert");
      await ev("document.querySelector('#refresh').click()");
      await expect(".alert-card","已触发");
      await expect("#body","最近状态变化");
      await click("暂停",".alert-card button");
      await expect(".alert-card","已暂停");
      await click("编辑",".alert-card button");
      await field("触发阈值","90");
      await click("保存规则","dialog[open] button");
      await until(async()=> (await alerts()).some(item=>item.name===ruleTitle&&item.threshold===90),"edited threshold");
      await view("profiles","&"+alertWindow);
      await expect(".function-stats","ChatService::sendMessage");
      await ev("(()=>{const f=document.querySelector('[aria-label=搜索业务函数]');f.value='sendMessage';f.dispatchEvent(new Event('input'));})()");
      await expect(".function-trend","写入聊天消息");
      await until(()=>ev("document.querySelector('.function-trend svg.time-chart path')"),"function trends from recorded load");
      await expect(".function-trend","P95");
    }
    if(sloWindow) {
      await view("reliability","&"+sloWindow);
      const impact='[data-objective="adapter_http"]';
      await expect("#body","采集质量");
      await expect(impact,"超速");
      const request=await(await fetch(base+"/api/v1/service-levels?"+sloWindow)).json();
      const http=request.objectives[0];
      assert(http.observed.bad>0);
      await expect(impact,"未达标样本"+http.observed.bad);
      await expect('[data-objective="chat_ack"]',"可判定");
      const shot=await call("Page.captureScreenshot",{format:"png"});
      fs.writeFileSync(path.join(work,"service-levels.png"),Buffer.from(shot.data,"base64"));
      await click("查找同窗 HTTP 5xx Trace",impact+" a");
      await until(()=>ev("document.querySelector('[aria-label=响应状态]')?.value==='5xx'"),"HTTP status persisted from SLO");
      await until(()=>ev("[...document.querySelectorAll('#body .list .row small')].some(x=>/^HTTP 5[0-9]{2}$/.test(x.textContent))"),
        "real 5xx candidates");
      await click("打开链路","#body .list button");
      await expect(".trace-selected","状态");
      await desktop(390,844);
      await view("reliability","&"+sloWindow);
      await expect(impact,"超速");
      assert(await ev("document.documentElement.scrollWidth<=390"));
    }
    await desktop(390,844);
    await view("boards");
    await expect("#body","常用监控模板");
    assert(await ev("document.documentElement.scrollWidth<=390"));
    assert.equal(errors.length,0,JSON.stringify(errors));
    console.log("PASS live metrics, local dashboard file with remapping, Query and trace drill-down, desktop and mobile");
    if(sloWindow)console.log("PASS SLO impact, real 5xx filters and trace, 390px layout",work);
  }finally{
    try{
      const data=await records();
      if(alertWindow) {
        const response=await fetch(base+"/api/v1/alert-rules");
        if(response.ok)for(const rule of (await response.json()).rules.filter(item=>item.name===ruleTitle)) {
          await update("alert/delete",{id:rule.id,revision:rule.revision});
        }
      }
      for(const board of data.boards.filter(record=>ownedBoards.has(record.title))) await update("board/delete",board);
      for(const source of data.sources.filter(record=>record.name===title)) await update("source/delete",source);
    }finally{socket?.close();edge.kill();}
  }
}
run().catch(error=>{console.error(error);process.exitCode=1;});
