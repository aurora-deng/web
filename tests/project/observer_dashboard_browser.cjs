/* Actual browser checks: slow optional sources cannot block metrics or overwrite a newer page. */
const assert=require('node:assert/strict');
const {spawn}=require('node:child_process');
const fs=require('node:fs');
const path=require('node:path');
const base=process.env.OBSERVER_RD_URL||'http://127.0.0.1:18795';
const directory=path.resolve('build-function3','obs-qa','dashboard-browser-'+Date.now());
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(test,label){
  const deadline=Date.now()+12000;
  do{if(await test())return;await delay(100);}while(Date.now()<deadline);
  throw new Error('Timed out: '+label);
}
async function run(){
  const health=await fetch(base+'/health');assert(health.ok);
  const metricRoute=(await health.json()).seriesBatch?'/api/v1/series/batch':'/api/v1/series';
  const end=Math.floor(Date.parse(health.headers.get('date'))/1000);
  fs.mkdirSync(directory,{recursive:true});
  const edge=spawn(process.env.OBSERVER_RD_EDGE||'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
    ['--headless=new','--no-first-run','--disable-extensions','--user-data-dir='+directory,'--remote-debugging-port=0','about:blank'],
    {windowsHide:true,stdio:'ignore'});
  let socket;
  try{
    await until(()=>fs.existsSync(path.join(directory,'DevToolsActivePort')),'browser');
    const port=fs.readFileSync(path.join(directory,'DevToolsActivePort'),'utf8').split(/\r?\n/)[0];
    const tabs=await(await fetch('http://127.0.0.1:'+port+'/json/list')).json();
    socket=new WebSocket(tabs.find(tab=>tab.type==='page').webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{socket.onopen=resolve;socket.onerror=reject;});
    let sequence=0;const pending=new Map(),errors=[];
    socket.onmessage=({data})=>{
      const message=JSON.parse(data);
      if(message.method==='Runtime.exceptionThrown')errors.push(message.params.exceptionDetails);
      const task=pending.get(message.id);
      if(task){pending.delete(message.id);message.error?task.reject(message.error):task.resolve(message.result);}
    };
    const call=(method,params={})=>new Promise((resolve,reject)=>{
      const id=++sequence;pending.set(id,{resolve,reject});socket.send(JSON.stringify({id,method,params}));
    });
    const js=async expression=>{
      const result=await call('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
      assert(!result.exceptionDetails,result.exceptionDetails?.exception?.description);
      return result.result.value;
    };
    const screenshot=async name=>{
      const result=await call('Page.captureScreenshot',{format:'png'});
      fs.writeFileSync(path.join(directory,name+'.png'),Buffer.from(result.data,'base64'));
    };
    await call('Page.enable');await call('Runtime.enable');
    await call('Emulation.setDeviceMetricsOverride',{width:1440,height:900,mobile:false,deviceScaleFactor:1});
    await call('Page.addScriptToEvaluateOnNewDocument',{source:`
      window.dashboardStarted=performance.now();
      window.dashboardMetricRoute=${JSON.stringify(metricRoute)};
      window.optionalMode='hold';window.optionalHeld=[];window.dashboardCalls={};
      const nativeFetch=window.fetch;
      window.fetch=(url,options)=>{
        const route=new URL(url,location.href).pathname;
        window.dashboardCalls[route]=(window.dashboardCalls[route]||0)+1;
        if(['/api/v1/overview','/api/v1/alerts','/api/v1/version','/api/v1/markers'].includes(route)){
          if(window.optionalMode==='hold')return new Promise((resolve,reject)=>{
            const row={route,url,options,resolve,reject};window.optionalHeld.push(row);
            if(options?.signal?.aborted)reject(options.signal.reason);
            else options?.signal?.addEventListener('abort',()=>reject(options.signal.reason),{once:true});
          });
          if(window.optionalMode==='old'&&route==='/api/v1/version')return new Promise(resolve=>{
            window.releaseOld=()=>resolve(new Response(JSON.stringify({revision:'STALE_OLD_GENERATION'}),{status:200}));
          });
          if(window.optionalMode==='current'&&route==='/api/v1/version')
            return Promise.resolve(new Response(JSON.stringify({revision:'CURRENT_GENERATION'}),{status:200}));
        }
        return nativeFetch(url,options);
      };
      window.releaseSupplemental=()=>{
        window.optionalMode='pass';
        for(const row of window.optionalHeld.splice(0)){
          if(row.route==='/api/v1/alerts')row.reject(new Error('受控告警后端失败'));
          else nativeFetch(row.url,row.options).then(row.resolve,row.reject);
        }
      };
    `});
    const url=base+'/?view=service&from='+(end-900)+'&to='+end;
    await call('Page.navigate',{url});
    await until(()=>js('!!document.querySelector("#traffic svg")'),'metrics before supplemental completion');
    const firstChartMs=await js('Math.round(performance.now()-window.dashboardStarted)');
    assert(await js('window.optionalHeld.length===4'));
    assert(await js('document.querySelector(".dashboard-background").dataset.state==="loading"'));
    assert(await js('document.querySelector(".dashboard-alerts").textContent.includes("正在查询告警")'));
    await js('window.firstMetricPlot=document.querySelector("#traffic svg")');
    await screenshot('metrics-before-supplemental');
    const refreshes=await js('window.dashboardCalls[window.dashboardMetricRoute]||0');
    await js('document.querySelector("#refresh").click()');
    await until(()=>js('(window.dashboardCalls[window.dashboardMetricRoute]||0)>'+refreshes+'&&document.querySelector("#status").dataset.state!=="loading"'),'metrics refresh with slow sources still pending');
    await delay(120);
    assert(await js('window.optionalHeld.length===4&&window.optionalHeld.every(row=>!row.options.signal.aborted)'));
    await js('window.releaseSupplemental()');
    await until(()=>js('document.querySelector(".dashboard-background").dataset.state==="warn"'),'independent optional failure');
    assert(await js('document.querySelector(".dashboard-alerts").textContent.includes("不能据此判断没有告警")'));
    assert(await js('document.querySelector("#traffic svg")===window.firstMetricPlot'));
    const beforeRetry=await js('window.dashboardCalls[window.dashboardMetricRoute]||0');
    await js('document.querySelector(".dashboard-background button").click()');
    await until(()=>js('document.querySelector(".dashboard-background").hidden'),'retry failed source only');
    assert.equal(await js('window.dashboardCalls[window.dashboardMetricRoute]||0'),beforeRetry);
    assert(await js('document.querySelector("#traffic svg")===window.firstMetricPlot'));

    const counts=await js('({...window.dashboardCalls})');
    await js('document.querySelector("#traffic .panel-actions button:last-child").click()');
    await until(()=>js('!!document.querySelector(".focus-panel #traffic svg")'),'focused panel');
    for(const route of ['/api/v1/overview','/api/v1/alerts','/api/v1/version'])
      assert.equal(await js('window.dashboardCalls['+JSON.stringify(route)+']'),counts[route]);

    await js('window.optionalMode="old";document.querySelector("[data-view=service]").click()');
    await until(()=>js('!!window.releaseOld&&!!document.querySelector("#traffic svg")'),'old delayed version');
    await js('window.optionalMode="current";document.querySelector("[data-view=overview]").click()');
    await until(()=>js('document.querySelector(".dashboard-meta")?.textContent.includes("CURRENT_GENERATION")'),'current generation');
    await js('window.releaseOld()');await delay(150);
    assert(await js('!document.querySelector("#body").textContent.includes("STALE_OLD_GENERATION")'));
    await screenshot('independent-updates-and-generation-guard');
    await call('Emulation.setDeviceMetricsOverride',{width:390,height:844,mobile:false,deviceScaleFactor:1});
    await until(()=>js('document.documentElement.scrollWidth<=390'),'mobile width');
    await screenshot('mobile-dashboard');
    assert.equal(errors.length,0,JSON.stringify(errors));
    const evidence={passed:true,firstChartMs,metricRoute,withheldOptionalRequests:4,chartStableOnRetry:true,
      failedOnlyRetry:true,pendingSurvivesMetricRefresh:true,focusSkippedHiddenQueries:true,lateGenerationIgnored:true,artifacts:directory};
    fs.writeFileSync(path.join(directory,'result.json'),JSON.stringify(evidence,null,2)+'\n');
    console.log(JSON.stringify(evidence));
  }finally{socket?.close();edge.kill();}
}
run().catch(error=>{console.error(error);process.exitCode=1;});
