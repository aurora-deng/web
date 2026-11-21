/* Windows / Edge RD smoke. Requires an already running real Observer lab with fixtures and reports. */
const assert = require("node:assert/strict");
const cp = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");

const base = process.env.OBSERVER_RD_URL || "http://127.0.0.1:18787";
const edgePath = process.env.OBSERVER_RD_EDGE || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
const work = path.resolve("build-function3","obs-qa","observer-browser-"+Date.now());
const delay = ms => new Promise(resolve=>setTimeout(resolve,ms));

async function wait(fn, description, timeout=12000) {
  const due=Date.now()+timeout;
  do {
    if (await fn()) return;
    await delay(125);
  } while (Date.now()<due);
  throw new Error("Timed out: "+description);
}

async function response(route) {
  const reply=await fetch(base+route);
  assert(reply.ok, route+" response "+reply.status);
  return reply.json();
}

async function run() {
  const state=await response("/health");
  assert.equal(state.simulated,false,"use the actual lab for this smoke test");
  assert.match(state.assetVersion,/^[0-9a-f]{16}$/);
  const end=Math.floor(Date.now()/1000);
  const report = await response("/api/v1/runs?from="+(end-604800)+"&to="+end);
  assert(report.length>=2, "load real experiment reports first");
  const pairs=report.flatMap((a,i)=>report.slice(i+1).map(b=>[a,b]));
  const first=pairs.find(([a,b])=>a.identity.workloadSha256===b.identity.workloadSha256 &&
    a.identity.configSha256===b.identity.configSha256 && a.identity.environmentSha256===b.identity.environmentSha256 &&
    a.result.ackSamples>=1000 && b.result.ackSamples>=1000);
  assert(first,"no comparable experiment reports");
  const incompatible=pairs.find(([a,b])=>a.result.workers!==b.result.workers);
  const traces=await response("/api/v1/traces");
  assert(traces.items.length,"run an authenticated trace probe first");
  const dag=await response("/api/v1/events?domain=dag");
  const agent=await response("/api/v1/events?domain=agent");
  assert(dag.items.length && agent.items.length,"import the isolated DAG and Agent scenarios");

  fs.mkdirSync(work,{recursive:true});
  const edge=cp.spawn(edgePath,["--headless=new","--no-first-run","--disable-extensions",
    "--user-data-dir="+work,"--remote-debugging-port=0","about:blank"],{stdio:"ignore",windowsHide:true});
  let socket;
  try {
    const debugFile=path.join(work,"DevToolsActivePort");
    await wait(()=>fs.existsSync(debugFile),"Edge DevTools");
    const port=fs.readFileSync(debugFile,"utf8").split(/\r?\n/)[0];
    const tabs=await (await fetch("http://127.0.0.1:"+port+"/json/list")).json();
    socket=new WebSocket(tabs.find(tab=>tab.type==="page").webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    let count=0;
    const pending=new Map(), errors=[];
    socket.onmessage=({data})=>{
      const msg=JSON.parse(data);
      if(msg.method==="Runtime.exceptionThrown") errors.push(msg.params.exceptionDetails);
      if(!pending.has(msg.id)) return;
      const [resolve,reject]=pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? reject(msg.error) : resolve(msg.result);
    };
    const call=(method,params={})=>new Promise((resolve,reject)=>{
      const id=++count;
      pending.set(id,[resolve,reject]);socket.send(JSON.stringify({id,method,params}));
    });
    await call("Page.enable");await call("Runtime.enable");await call("Network.enable");
    const ev=async expression=>{
      const value=await call("Runtime.evaluate",{expression,returnByValue:true});
      assert(!value.exceptionDetails,value.exceptionDetails?.exception?.description);
      return value.result.value;
    };
    const check=(expression,title)=>wait(()=>ev(expression),title);
    const nav=async view=>{
      await ev('document.querySelector("[data-view='+view+']").click()');
      await check('new URL(location).searchParams.get("view")==='+JSON.stringify(view) +
        ' && document.querySelector("#status")?.dataset.state!=="loading"',view);
    };
    const contains=(selector,text)=>'document.querySelector('+JSON.stringify(selector)+')?.textContent.includes('+JSON.stringify(text)+')';
    await call("Emulation.setDeviceMetricsOverride",{width:1440,height:900,deviceScaleFactor:1,mobile:false});
    await call("Page.navigate",{url:base+"/"});
    await check("document.querySelector('#host svg') && document.querySelector('#boot-status').hidden","overview");
    await ev("(()=>{const f=document.getElementById('instance');f.selectedIndex=1;f.dispatchEvent(new Event('change'))})()");
    await check("new URL(location).searchParams.has('instance')","instance filter");
    await nav("service");
    await check("document.querySelector('#latency svg')","latency");
    await check("!!document.querySelector('#latency .cursor-line')","latency canvas ready");
    await ev("document.querySelector('#latency svg').scrollIntoView({block:'center',behavior:'instant'})");
    const r=await ev("(()=>{const r=document.querySelector('#latency svg').getBoundingClientRect();return {x:r.right-25,y:r.top+60}})()");
    await call("Input.dispatchMouseEvent",{type:"mouseMoved",...r});
    await check("document.querySelectorAll('.cursor-line[visibility=visible]').length>=2","shared cursor");
    await call("Input.dispatchMouseEvent",{type:"mousePressed",button:"left",clickCount:1,x:r.x-200,y:r.y});
    await call("Input.dispatchMouseEvent",{type:"mouseMoved",buttons:1,x:r.x,y:r.y});
    await call("Input.dispatchMouseEvent",{type:"mouseReleased",button:"left",clickCount:1,x:r.x,y:r.y});
    await check(contains("#status","固定窗口"),"time selection");
    await ev("document.querySelector('#latency .panel-actions button:last-child').click()");
    await check("document.querySelector('.focus-panel #latency')","panel focus");
    await ev("document.querySelector('#latency button').click()");
    await check("document.querySelector('#inspector').open && document.querySelector('#inspector tbody tr')","inspector");
    await ev("[...document.querySelectorAll('#inspector button')].find(b=>b.textContent==='原始数据').click()");
    await check("document.querySelector('#inspector tbody tr')","raw data");
    await call("Page.setDownloadBehavior",{behavior:"allow",downloadPath:work});
    await ev("[...document.querySelectorAll('#inspector button')].find(b=>b.textContent==='下载 CSV').click()");
    await wait(()=>fs.existsSync(path.join(work,"nebula-latency.csv")),"CSV download");
    assert.match(fs.readFileSync(path.join(work,"nebula-latency.csv"),"utf8"),/时间\(UTC\).*序列/);
    await ev("document.querySelector('#inspector form button').click()");
    await ev("document.querySelector('#pause').click()");
    await check("!new URL(location).searchParams.has('from')","resume live window");
    await nav("explore");
    await ev("document.querySelector('[aria-label=查询指标]').value='threads';document.querySelector('#body form').requestSubmit()");
    await check(contains("#body","样本 · 平均"),"metric explore");
    await ev("document.querySelector('#body summary').click()");
    await check(contains("#body code","nebula_probe_target_threads"),"query expression");
    await nav("traces");
    await ev("document.querySelector('[aria-label=链路操作]').selectedIndex=2;document.querySelector('#body form:nth-of-type(2)').requestSubmit()");
    await check("document.querySelector('#body .list .row button')","trace result");
    await ev("document.querySelector('#body .list .row button').click()");
    await check("document.querySelectorAll('.span-bar').length>=3","business and SQL trace");
    await check(contains("#body","PostgreSQL command"),"SQL row");
    await nav("runs");
    await ev("(()=>{const f=document.getElementById('range');f.value='604800';f.dispatchEvent(new Event('change'));})()");
    await check("document.querySelectorAll('#body input[type=checkbox]').length==="+report.length,"seven-day experiment rows");
    await ev("document.querySelector('#body details summary').click()");
    await check(contains("#body pre","ackSamples"),"experiment detail");
    const choose=pair=>ev("(()=>{const selection="+JSON.stringify(pair.map(row=>report.indexOf(row)))+
      ";const items=[...document.querySelectorAll('#body input[type=checkbox]')];items.filter(i=>i.checked).forEach(i=>i.click());"+
      "selection.forEach(i=>items[i].click());[...document.querySelectorAll('#body button')].find(b=>b.textContent.includes('比较')).click();})()");
    await choose(first);
    await check(contains("#body","ACK P99 变化"),"compatible experiment comparison");
    if(incompatible) {
      await choose(incompatible);
      await check(contains("#body","证据不可比"),"incompatible comparison");
    }
    await nav("extensions");
    await ev("document.querySelector('#body .list button').click()");
    await check("document.querySelector('[data-dag-node=B]')","DAG");
    await ev("document.querySelector('[data-dag-node=B]').dispatchEvent(new MouseEvent('click'))");
    await check("document.querySelectorAll('.span-row[data-node]:not([hidden])').length===2","DAG retry selection");
    await nav("extensions");
    await ev("[...document.querySelectorAll('#body .strip button')].find(b=>b.textContent==='Agent').click()");
    await check(contains("#body","技能调用"),"Agent list");
    await ev("document.querySelector('#body .list button').click()");
    await check("document.querySelectorAll('.agent-grid .stat-card').length===5","Agent summary");
    await nav("extensions");
    await ev("[...document.querySelectorAll('#body .strip button')].find(b=>b.textContent==='设备遥测').click()");
    await check(contains("#body","控制指令与执行回执"),"Device ACK");
    await ev("[...document.querySelectorAll('#body .strip button')].find(b=>b.textContent==='服务事件').click()");
    await check(contains("#body","服务探针"),"actual service event");
    await call("Emulation.setDeviceMetricsOverride",{width:390,height:844,deviceScaleFactor:1,mobile:false});
    await nav("overview");
    await check("document.documentElement.scrollWidth <= 390","mobile overflow");
    assert.equal(errors.length,0,JSON.stringify(errors));

    await call("Network.setBlockedURLs",{urls:[base+"/workspace.js"]});
    await call("Page.navigate",{url:base+"/?rd=blocked"});
    await check(contains("#boot-status","监控页面资源加载失败"),"module load failure is visible");
    await call("Network.setBlockedURLs",{urls:[]});
    console.log("PASS desktop and mobile: all six boards, data queries, CSV, traces, runs, event drilldown and boot recovery");
  } finally {
    socket?.close();
    edge.kill();
  }
}
run().catch(error=>{console.error(error);process.exitCode=1;});
