/* Edge CDP smoke against a real Linux lab with an imported resource report. */
const assert=require("node:assert/strict");
const {spawn}=require("node:child_process");
const fs=require("node:fs");
const path=require("node:path");
const base=process.env.OBSERVER_RD_URL || "http://127.0.0.1:18791";
const functionWindow=process.env.OBSERVER_RD_TRACE_WINDOW;
const commitReport=process.env.OBSERVER_RD_COMMIT_REPORT;
const commitControl=process.env.OBSERVER_RD_COMMIT_CONTROL;
const liveTrend=process.env.OBSERVER_RD_LIVE_TREND;
const pageBoard=process.env.OBSERVER_RD_PAGE_BOARD;
const directory=path.resolve("build-function3","obs-qa","resource-browser-"+Date.now());
const wait=ms=>new Promise(done=>setTimeout(done,ms));

async function eventually(test,label) {
  for(let attempt=0;attempt<110;attempt++) {
    if(await test()) return;
    await wait(130);
  }
  throw Error("Timed out: "+label);
}

async function run() {
  fs.mkdirSync(directory,{recursive:true});
  const edge=spawn(process.env.OBSERVER_RD_EDGE || "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    ["--headless=new","--no-first-run","--disable-extensions","--user-data-dir="+directory,"--remote-debugging-port=0","about:blank"],
    {windowsHide:true,stdio:"ignore"});
  let socket;
  try {
    const portFile=path.join(directory,"DevToolsActivePort");
    await eventually(()=>fs.existsSync(portFile),"CDP port");
    const port=fs.readFileSync(portFile,"utf8").split(/\r?\n/)[0];
    const tabs=await (await fetch("http://127.0.0.1:"+port+"/json/list")).json();
    socket=new WebSocket(tabs.find(tab=>tab.type==="page").webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    let id=0;
    const pending=new Map(),errors=[];
    socket.onmessage=({data})=>{
      const message=JSON.parse(data);
      if(message.method==="Runtime.exceptionThrown") errors.push(message.params.exceptionDetails);
      const done=pending.get(message.id);
      if(done){pending.delete(message.id);message.error?done.reject(message.error):done.resolve(message.result);}
    };
    const call=(method,params={})=>new Promise((resolve,reject)=>{
      const seq=++id;pending.set(seq,{resolve,reject});socket.send(JSON.stringify({id:seq,method,params}));
    });
    const js=async expression=>{
      const result=await call("Runtime.evaluate",{expression,returnByValue:true,awaitPromise:true});
      assert(!result.exceptionDetails,result.exceptionDetails?.exception?.description);
      return result.result.value;
    };
    const present=(label,selector="#body")=>eventually(()=>js("document.querySelector("+JSON.stringify(selector)+")?.textContent.includes("+JSON.stringify(label)+")"),label);
    const click=(label,selector="#body button")=>js("[...document.querySelectorAll("+JSON.stringify(selector)+")].find(x=>x.textContent.trim()==="+JSON.stringify(label)+")?.click()");
    const page=async(view)=>call("Page.navigate",{url:base+"/?view="+view});
    const width=async(width,height)=>call("Emulation.setDeviceMetricsOverride",{width,height,mobile:false,deviceScaleFactor:1});
    const screenshot=async name=>{
      const result=await call("Page.captureScreenshot",{format:"png"});
      fs.writeFileSync(path.join(directory,name+".png"),Buffer.from(result.data,"base64"));
    };
    const exportPng=async selector=>{
      await call("Page.setDownloadBehavior",{behavior:"allow",downloadPath:directory});
      const earlier=new Set(fs.readdirSync(directory));
      await eventually(()=>js("!!document.querySelector("+JSON.stringify(selector)+")"),"PNG export control");
      await js("document.querySelector("+JSON.stringify(selector)+").click()");
      let downloaded;
      try {
        await eventually(()=>{
          downloaded=fs.readdirSync(directory).find(name=>name.startsWith("NEBULA-")&&name.endsWith(".png")&&!earlier.has(name));
          return downloaded&&fs.statSync(path.join(directory,downloaded)).size>512;
        },"PNG file downloaded");
      } catch(error) {
        const failure=await js("document.querySelector("+JSON.stringify(selector)+")?.title");
        throw new Error(failure||error.message);
      }
      const png=fs.readFileSync(path.join(directory,downloaded));
      assert.equal(png.subarray(0,8).toString("hex"),"89504e470d0a1a0a");
      assert(png.readUInt32BE(16)>500 && png.readUInt32BE(20)>160);
    };
    await call("Page.enable");await call("Runtime.enable");
    await width(1440,900);
    if(liveTrend) {
      const loaded=await (await fetch(base+"/health")).json();
      assert.equal(loaded.localScope,liveTrend);
      assert.equal(loaded.seriesBatch,true);
      const instrument=await(await fetch(base+"/api/v1/instrumentation")).json();
      await call("Page.addScriptToEvaluateOnNewDocument",{source:
        "const nativeObserverTestClock=Date.now;Date.now=()=>nativeObserverTestClock()+180000;"+
        "window.observerBatchQueries=0;const originalObserverFetch=window.fetch;"+
        "window.fetch=(url,options)=>{if(typeof url==='string'&&url.startsWith('/api/v1/series/batch?'))"+
        "window.observerBatchQueries++;return originalObserverFetch(url,options);};"});
      await page("service");
      await present(liveTrend==="daily"?"本机 · 日常实时":"本机 · 隔离实时","#environment");
      await present("请求速率","section#traffic");
      await exportPng("#traffic .chart-export:not([hidden])");
      await screenshot("live-service");
      await js(`(()=>{
        const panel=document.querySelector("#traffic");
        window.originalLivePanel=panel;
        window.originalLivePlot=panel.querySelector("svg");
        window.originalLiveLegend=panel.querySelector(".legend button");
        window.originalLiveLegend.click();
        window.originalLiveLegend.focus();
        window.livePaints=0;
        window.livePaintObserver=new MutationObserver(()=>window.livePaints++);
        window.livePaintObserver.observe(window.originalLivePlot,{childList:true});
        const interval=document.querySelector("#interval");
        interval.value="5";interval.dispatchEvent(new Event("change"));
      })()`);
      await eventually(()=>js("window.livePaints>0"),"in-place interval change");
      const pointer=await js(`(()=>{
        const rect=window.originalLivePlot.getBoundingClientRect();
        return {x:rect.left+rect.width*.7,y:rect.top+rect.height*.5};
      })()`);
      await call("Input.dispatchMouseEvent",{type:"mouseMoved",...pointer});
      const beforeRefresh=await js("({queries:window.observerBatchQueries,paints:window.livePaints})");
      await eventually(()=>js("window.observerBatchQueries>"+beforeRefresh.queries+" && window.livePaints>"+beforeRefresh.paints),
        "automatic refresh while hovering the chart");
      assert(await js("document.querySelector('#traffic')===window.originalLivePanel"));
      assert(await js("document.querySelector('#traffic svg')===window.originalLivePlot"));
      assert(await js("document.querySelector('#traffic .legend button')===window.originalLiveLegend"));
      assert(await js("document.activeElement===window.originalLiveLegend"));
      assert.equal(await js("window.originalLiveLegend.getAttribute('aria-pressed')"),"false");
      await js("window.originalLiveLegend.click();window.livePaintObserver.disconnect();"+
        "document.querySelector('#interval').value='0';document.querySelector('#interval').dispatchEvent(new Event('change'))");
      await present("已暂停","#status");
      await js(`(()=>{
        const original=window.fetch;
        let fail=true;
        window.fetch=(url,options)=>{
          if(fail&&typeof url==="string"&&url.startsWith("/api/v1/series/batch?")) {
            fail=false;
            return Promise.resolve(new Response('{"error":"temporary unavailable"}',{status:503}));
          }
          return original(url,options);
        };
        window.restoreLiveFetch=()=>window.fetch=original;
        document.querySelector("#refresh").click();
      })()`);
      await present("数据源不可用","#traffic");
      assert(await js("document.querySelector('#traffic')===window.originalLivePanel"));
      assert(await js("!document.querySelector('#traffic svg')"));
      assert.equal(await js("document.querySelector('#status').dataset.state"),"warn");
      await js("window.restoreLiveFetch();document.querySelector('#refresh').click()");
      await eventually(()=>js("!!document.querySelector('#traffic svg')"),"live graph recovers after failed refresh");
      assert(await js("document.querySelector('#traffic')===window.originalLivePanel"));
      if(liveTrend==="daily"&&!instrument.available) {
        await present("当前采样未收到所需耗时指标","#latency");
        await present("核对服务版本和业务埋点设置","#latency");
      }
      await js(`(()=>{
        const original=window.fetch;
        let stall=true;
        window.fetch=(url,options)=>{
          if(stall&&typeof url==="string"&&url.startsWith("/api/v1/series/batch?")) {
            stall=false;
            return new Promise((resolve,reject)=>{
              options.signal.addEventListener("abort",()=>reject(options.signal.reason),{once:true});
            });
          }
          return original(url,options);
        };
        window.restoreTimedFetch=()=>window.fetch=original;
        document.querySelector("#refresh").click();
      })()`);
      await present("查询超时","#traffic");
      await js("window.restoreTimedFetch();document.querySelector('#refresh').click()");
      await eventually(()=>js("!!document.querySelector('#traffic svg')"),"live graph recovers after query timeout");
      await width(390,844);
      await eventually(()=>js("document.documentElement.scrollWidth<=390"),"mobile overview width");
      await exportPng("#traffic .chart-export:not([hidden])");
      if(liveTrend==="daily") {
        await page("profiles&profile=");
        if(instrument.available)await present("已收到 nebula_function_seconds",".instrumentation-status");
        else await present("当前采样未收到所需耗时指标",".function-trend");
        await page("explore&metric=http_p99");
        if(instrument.available)await present("Explore · PromQL 与直方图");
        else await present("当前采样未收到所需耗时指标");
        assert(await js("document.documentElement.scrollWidth<=390"));
      }
      if(liveTrend==="daily"&&pageBoard) {
        await page("boards&board="+encodeURIComponent(pageBoard));
        await present("聊天室 · 页面探测延迟");
        await present("苦情树 · 页面探测延迟");
        await exportPng("#body .chart-export:not([hidden])");
      }
      assert.equal(errors.length,0,JSON.stringify(errors));
      console.log("PASS live metrics, in-place refresh, empty diagnostics, query timeout/recovery, function/Explore views and desktop/mobile PNG.");
      console.log(directory);
      return;
    }
    if(commitReport) {
      const naturalPath="/api/v1/resources/reports/"+encodeURIComponent(commitReport);
      const selectReport=async id=>js("(()=>{const list=document.querySelector('.commit-report select');list.value="+
        JSON.stringify(id)+";list.dispatchEvent(new Event('change'));})()");
      await page("profiles&profile=");
      await present("提交诊断",".profile-sections");
      await click("提交诊断",".profile-sections a");
      await present("PostgreSQL 提交诊断");
      await eventually(()=>js("!!document.querySelector('.commit-report select')"),"report selector");
      assert(await js("document.getElementById('range').parentElement.hidden"));
      await present("实验归档","#environment");
      await selectReport(commitReport);
      await present("IO:WalSync",".commit-sample-detail");
      await present("521.616 ms",".commit-sample-detail h3");
      await exportPng(".commit-samples-heading .chart-export");
      await screenshot("commit-desktop");
      await width(1920,1080);
      await js("document.querySelectorAll('.commit-sample')[1].click()");
      await present("第 2 条",".commit-sample-detail h3");
      const sampleLink=await js("location.href");
      assert.equal(new URL(sampleLink).searchParams.get("sample"),"2");
      assert.equal(await js("document.querySelectorAll('.commit-sample[aria-pressed=true]').length"),1);
      await js("document.querySelector('.commit-evidence summary').click()");
      await present("WAL 同步累计",".commit-evidence[open]");
      if(commitControl) {
        await selectReport(commitControl);
        await present("注入延迟 120 ms",".commit-kind");
        await present("Timeout:PgSleep",".commit-sample-detail");
        assert.equal(await js("new URL(location.href).searchParams.get('sample')"),null);
      }
      await js(`(async()=>{
        const original=window.fetch;
        const archive=await (await original(${JSON.stringify(naturalPath)})).json();
        window.restoreCommitFetch=()=>window.fetch=original;
        window.fetch=(resource,options)=>{
          if(resource!==${JSON.stringify(naturalPath)}) return original(resource,options);
          return new Promise(resolve=>{
            window.finishCommitFetch=async failed=>{
              resolve({ok:!failed,json:async()=>failed?{error:"迟到的结果"}:archive});
              await new Promise(done=>setTimeout(done,50));
            };
          });
        };
      })()`);
      for(const failed of [false,true]) {
        await selectReport(commitReport);
        await present("正在读取报告",".profile-status");
        await selectReport("");
        await js("window.finishCommitFetch("+failed+")");
        await present("请选择要查看的实验报告",".profile-status");
        assert(await js("!document.querySelector('.commit-report-heading')"));
        assert.equal(await js("new URL(location.href).searchParams.get('profile')"),"");
      }
      await js("window.restoreCommitFetch()");
      await js("window.previousSelector=document.querySelector('.commit-report select');document.querySelector('#refresh').click()");
      await eventually(()=>js("!!document.querySelector('.commit-report select') && document.querySelector('.commit-report select')!==window.previousSelector"),
        "refreshed report selector");
      await present("请选择要查看的实验报告",".profile-status");
      await selectReport(commitReport);
      await present("自然负载",".commit-kind");
      const deepLink=await js("location.href");
      await width(390,844);
      await call("Page.navigate",{url:deepLink});
      await present("IO:WalSync",".commit-sample-detail");
      assert(await js("document.documentElement.scrollWidth<=390"));
      const navigation=()=>js(`(()=>{
        const nav=document.querySelector(".sidebar nav"), active=nav.querySelector("[aria-current]");
        const frame=nav.getBoundingClientRect(), button=active.getBoundingClientRect();
        return {left:button.left,right:button.right,menuLeft:frame.left,menuRight:frame.right,scroll:nav.scrollLeft,
          clipped:[...nav.querySelectorAll("button")].filter(item=>item.scrollWidth>item.clientWidth+1).map(item=>item.textContent)};
      })()`);
      await eventually(async()=>{
        const layout=await navigation();
        return layout.left>=layout.menuLeft-1 && layout.right<=layout.menuRight+1;
      },"active mobile navigation item fully visible");
      assert.deepEqual((await navigation()).clipped,[]);
      assert.equal(await js("getComputedStyle(document.querySelector('.commit-sample-detail tbody td')).whiteSpace"),"nowrap");
      await screenshot("commit-mobile");
      await js("document.querySelector('.commit-sample-detail').scrollIntoView()");
      await screenshot("commit-mobile-detail");
      await call("Page.navigate",{url:sampleLink});
      await present("第 2 条",".commit-sample-detail h3");
      await click("函数与内核",".profile-sections a");
      await present("Pyroscope 持续剖析");
      assert(await js("!document.getElementById('range').parentElement.hidden"));
      await click("数据源",".sidebar nav button");
      await present("添加数据源");
      assert.equal(errors.length,0,JSON.stringify(errors));
      console.log("PASS archived COMMIT data, control, wait and disk drill-down, stale requests, deep link and mobile.");
      console.log(directory);
      return;
    }
    await page("overview");
    await present("查资源占比","#host");
    await js("document.querySelector('#host a').click()");
    await present("服务与进程");
    await present("PostgreSQL");
    await present("NEBULA");
    await present("主机容量占比");
    await present("主机物理内存与交换区");
    await js("[...document.querySelectorAll('#body summary')].find(node=>node.textContent.includes('逻辑 CPU')).click()");
    await eventually(()=>js("[...document.querySelectorAll('#body details[open] tbody td')].some(d=>/^cpu[0-9]+$/.test(d.textContent))"),"per-core table");
    await eventually(()=>js("document.querySelectorAll('#body svg.time-chart path').length>=3"),"kernel graphs");
    await screenshot("resources");
    await page("profiles");
    await present("NEBULA 业务函数");
    await present("ChatService::sendMessage");
    await present("CPU 亲和性");
    await present("ACK 与同区间资源");
    await present("混合负载 ACK 逐条分段");
    await js("document.querySelector('.ack-breakdown summary').click()");
    await present("客户端 ACK P99",".ack-breakdown[open]");
    await click("syscalls #1");
    await present("epoll_wait");
    await click("callgrind #1");
    await present("函数自身指令量");
    await present("函数归因的系统调用时间");
    await click("dhat #1");
    await present("全局堆峰值时仍存活");
    await screenshot("profiles");
    if(functionWindow) {
      await page("profiles&"+functionWindow);
      await present("ChatService::historyBefore",".function-stats");
      const inspect=async name=>{
        await js("[...document.querySelectorAll('.function-stats tr')].find(row=>row.querySelector('small')?.textContent==="+
          JSON.stringify(name)+")?.querySelector('.function-actions button:last-child').click()");
        await present(name,".function-traces .trace-selected h4");
        await present("内部调用",".function-traces .trace-selected");
      };
      for(const name of ["ChatService::conversations","ChatService::history","ChatService::historyBefore","ChatService::sendMessage"])
        await inspect(name);
      await present("WS chat.send",".function-traces .trace-heading");
      const before=await js("document.querySelectorAll('.function-traces .trace-tree-row').length");
      await js("document.querySelector('.function-traces .trace-toggle').click()");
      assert(await js("document.querySelectorAll('.function-traces .trace-tree-row').length")<before);
      await js("document.querySelector('.function-traces .trace-toggle').click()");
      await click("WS chat.send",".function-traces .trace-span-name");
      await present("服务端",".function-traces .trace-selected");
      assert(await js("document.querySelector('.function-traces .trace-tree-row.selected').nextElementSibling.matches('.trace-selected')"));
      await js("[...document.querySelectorAll('.function-traces summary')].find(s=>s.textContent.startsWith('资源属性')).click()");
      await present("nebula-webserver",".function-traces .trace-attributes[open]");
      const deep=await js("location.href");
      const id=new URL(deep).searchParams.get("call");
      assert.match(id,/^[a-f0-9]{32}$/);
      await call("Page.setDownloadBehavior",{behavior:"allow",downloadPath:directory});
      await click("导出 JSON",".function-traces button");
      const exported=path.join(directory,"trace-"+id+".json");
      await eventually(()=>fs.existsSync(exported),"downloaded trace JSON");
      const data=JSON.parse(fs.readFileSync(exported,"utf8"));
      assert.equal(data.traceId,id);
      assert(data.spans.some(span=>span.name==="PostgreSQL command"));
      await call("Page.navigate",{url:deep});
      await present("WS chat.send",".function-traces .trace-selected h4");
      await js("document.querySelector('.function-traces .trace-inspect').scrollIntoView()");
      await screenshot("function-trace");
      await js("[...document.querySelectorAll('.function-traces .trace-selected a')].find(a=>a.textContent==='按 Trace 关联日志').click()");
      await present("关联日志","#trace-logs");
      assert.equal(await js("new URL(location.href).searchParams.get('incident')"),id);
      await width(390,844);
      await call("Page.navigate",{url:deep});
      await present("WS chat.send",".function-traces .trace-selected h4");
      assert(await js("document.documentElement.scrollWidth<=390"));
    }
    await width(390,844);
    await page("resources");
    await present("服务与进程");
    assert(await js("document.documentElement.scrollWidth<=390"));
    await page("profiles");
    await present("Pyroscope 持续剖析");
    assert(await js("document.documentElement.scrollWidth<=390"));
    assert.equal(errors.length,0,JSON.stringify(errors));
    console.log("PASS CPU click-through, proc and core data, kernel, function timings, real syscall/instructions/heap reports and mobile.");
    console.log(directory);
  } finally {socket?.close();edge.kill();}
}
run().catch(error=>{console.error(error);process.exitCode=1;});
