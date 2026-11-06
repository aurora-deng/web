/* Real Edge/native audio; HTTP/claims are fixtures. Does not replace PostgreSQL integration. */
const http=require('node:http'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'../../..');let claimed=false,successfulClaims=0,announcement=process.argv.includes('--serve-only')?0:1,created=Date.now(),audioFailures=0,metadataDelay=0;
const generated=Buffer.alloc(44+24000*2*3);generated.write('RIFF');generated.writeUInt32LE(generated.length-8,4);generated.write('WAVEfmt ',8);generated.writeUInt32LE(16,16);generated.writeUInt16LE(1,20);generated.writeUInt16LE(1,22);generated.writeUInt32LE(24000,24);generated.writeUInt32LE(48000,28);generated.writeUInt16LE(2,32);generated.writeUInt16LE(16,34);generated.write('data',36);generated.writeUInt32LE(generated.length-44,40);
for(let i=0;i<24000*3;i++)generated.writeInt16LE(Math.round(Math.sin(i*2*Math.PI*440/24000)*200),44+i*2);
const wav=process.env.ANNOUNCEMENT_TEST_WAVE?fs.readFileSync(process.env.ANNOUNCEMENT_TEST_WAVE):generated;
const server=http.createServer(async(req,res)=>{
 const url=new URL(req.url,'http://localhost');const json=value=>{res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(value));};
 if(url.pathname==='/fixture/state')return json({successfulClaims,announcement});
 if(req.method==='POST'&&url.pathname==='/fixture/new'){announcement++;created=Date.now();claimed=false;return json({ok:true});}
 if(req.method==='POST'&&url.pathname==='/fixture/fail'){audioFailures=1;return json({ok:true});}
 if(req.method==='POST'&&url.pathname==='/fixture/delay'){metadataDelay=10000;return json({ok:true});}
 if(url.pathname==='/preview.wav'){
  if(audioFailures>0){audioFailures--;res.writeHead(503,{'Content-Type':'application/json'});return res.end('{}');}
  const range=/^bytes=(\d+)-(\d+)$/.exec(req.headers.range||'');
  if(range){const start=Number(range[1]),end=Math.min(Number(range[2]),wav.length-1);if(start>end){res.writeHead(416);return res.end();}res.writeHead(206,{'Content-Type':'audio/wav','Content-Range':`bytes ${start}-${end}/${wav.length}`});return res.end(wav.subarray(start,end+1));}
  res.writeHead(200,{'Content-Type':'audio/wav'});return res.end(wav);
 }
 if(url.pathname==='/api/me/notification-settings')return json({sound:true,voice:'zf_001'});
 if(url.pathname==='/api/me/speech/status')return json({enabled:true});
 if(url.pathname==='/api/me/speech/preview'){const delay=metadataDelay;metadataDelay=0;if(delay)await new Promise(r=>setTimeout(r,delay));return json({state:'ready',url:'/preview.wav'});}
 if(url.pathname==='/api/announcements')return json(Number(url.searchParams.get('after')||0)>=announcement?[]:[{id:String(announcement),version:String(announcement),text:'仅用于播放回归的公开公告',createdAt:created,expiresAt:Date.now()+300000}]);
 if(/^\/api\/announcements\/\d+\/audio$/.test(url.pathname))return json({state:'ready',url:'/preview.wav'});
 if(/^\/api\/announcements\/\d+\/claim$/.test(url.pathname)){const value=!claimed;claimed=true;if(value)successfulClaims++;return json({claimed:value});}
 if(url.pathname==='/test'){res.writeHead(200,{'Content-Type':'text/html'});return res.end(`<meta charset="utf-8"><style>body{background:#181a24;color:#eee;font:16px system-ui;padding:28px}button,select{margin:8px;padding:10px}audio{display:block;margin:16px}output{display:block;padding:14px;white-space:pre-wrap}.announcement-sound{border:1px solid #666;padding:18px;margin-top:18px}</style><h1>公告播放器回归验证</h1><p>只有此页使用API夹具；音频为指定本地样本。真实排名、数据库与TTS另在隔离Linux验证。</p><button id="record">模拟开始录音</button><button id="stop-record">模拟结束录音</button><button id="new">发布下一条测试公告</button><button id="fail">让下一次音频下载失败</button><button id="retry">重试待播公告</button><button id="delay">让下一次试听响应延迟10秒</button><output id="state"></output><div id="announcements"></div><script type="module">import{AnnouncementPlayer}from'/phase11/shared/AnnouncementPlayer/AnnouncementPlayer.js';import{BusinessApi}from'/phase11/shared/BusinessApi/BusinessApi.js';
 if(new URL(location.href).searchParams.has('deny')){const play=HTMLMediaElement.prototype.play;let denied=false;HTMLMediaElement.prototype.play=function(){if(!denied){denied=true;return Promise.reject(new DOMException('fixture autoplay denied','NotAllowedError'));}return play.call(this);};}
 let api=new BusinessApi();api.csrf='fixture';window.player=new AnnouncementPlayer(api,document.querySelector('#announcements'));await player.start();window.ready=true;
 document.querySelector('#record').onclick=()=>document.dispatchEvent(new CustomEvent('nebula:recording',{detail:true}));document.querySelector('#stop-record').onclick=()=>document.dispatchEvent(new CustomEvent('nebula:recording',{detail:false}));
 for(const [id,route]of [['new','new'],['fail','fail'],['delay','delay']])document.getElementById(id).onclick=async()=>{await fetch('/fixture/'+route,{method:'POST'});if(id==='new')player.tick();};document.querySelector('#retry').onclick=()=>player.tick();
 setInterval(async()=>{const s=await(await fetch('/fixture/state')).json();document.querySelector('#state').textContent=JSON.stringify({...s,voice:player.settings.voice,audio:!!player.audio,playing:!!player.audio&&!player.audio.paused,recording:player.recording,previewBusy:!!player.previewBusy},null,2);},300);
 </script>`);}
 const file=path.resolve(root,'frontend',url.pathname.replace(/^\/phase11\//,'phase11/'));
 if(!file.startsWith(path.join(root,'frontend')+path.sep)||!fs.existsSync(file)){res.writeHead(404);return res.end();}
 res.writeHead(200,{'Content-Type':file.endsWith('.js')?'text/javascript':'text/css'});fs.createReadStream(file).pipe(res);
});
if(process.argv.includes('--serve-only'))server.listen(18761,'127.0.0.1',()=>console.log('Announcement fixture http://127.0.0.1:18761/test?deny=1'));
else (async()=>{const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');await new Promise(r=>server.listen(0,'127.0.0.1',r));const base='http://127.0.0.1:'+server.address().port;
 const browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true,args:['--autoplay-policy=user-gesture-required']});
 try{
  const first=await browser.newPage();
  // Explicitly inject one autoplay denial, then exercise native Audio after the user click.
  await first.addInitScript(()=>{const play=HTMLMediaElement.prototype.play;let blocked=false;HTMLMediaElement.prototype.play=function(){if(!blocked){blocked=true;return Promise.reject(new DOMException('fixture autoplay denied','NotAllowedError'));}return play.call(this);};});
  await first.goto(base+'/test');await first.waitForFunction(()=>window.player?.resume&&!player.resume.hidden);
  assert.equal(successfulClaims,1);assert.match(await first.locator('.announcement-sound small').innerText(),/再次点击|自动播放/);
  await first.getByRole('button',{name:'播放声音 / 继续',exact:true}).click();await first.waitForFunction(()=>player.audio&&!player.audio.paused);
  await first.evaluate(()=>document.dispatchEvent(new CustomEvent('nebula:recording',{detail:true})));assert.ok(await first.evaluate(()=>player.audio.paused));
  await first.evaluate(()=>document.dispatchEvent(new CustomEvent('nebula:recording',{detail:false})));await first.waitForFunction(()=>!player.audio.paused);
  const second=await browser.newPage();await second.goto(base+'/test');await second.waitForFunction(()=>window.ready);await second.waitForTimeout(200);
  assert.equal(successfulClaims,1);assert.equal(await second.evaluate(()=>Boolean(player.audio)),false,'second tab must not create another announcement audio');
  const lifecycle=await second.evaluate(async()=>{const{AnnouncementPlayer}=await import('/phase11/shared/AnnouncementPlayer/AnnouncementPlayer.js');const count=document.querySelectorAll('.announcement-sound').length;let resolve;const pending=new Promise(r=>resolve=r),test=new AnnouncementPlayer({request:()=>pending},document.createElement('div'));const start=test.start();test.dispose();resolve({sound:false,voice:'zf_001'});await start;return document.querySelectorAll('.announcement-sound').length===count;});assert.ok(lifecycle,'late settings must not resurrect a disposed player');
  await first.evaluate(()=>player.dispose());await second.evaluate(()=>player.dispose());console.log('Announcement autoplay-denial recovery/native playback, recording pause/resume, two-tab claim and disposed lifecycle passed');
 }finally{await browser.close();server.closeAllConnections();await new Promise(r=>server.close(r));}
})().catch(error=>{console.error(error);process.exitCode=1;server.closeAllConnections();server.close();});
