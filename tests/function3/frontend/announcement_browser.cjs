/* Real Edge/native audio; HTTP/claims are fixtures. Does not replace PostgreSQL integration. */
const http=require('node:http'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const root=path.resolve(__dirname,'../../..');let claimed=false,successfulClaims=0;
const wav=Buffer.alloc(44+24000*2*3);wav.write('RIFF');wav.writeUInt32LE(wav.length-8,4);wav.write('WAVEfmt ',8);wav.writeUInt32LE(16,16);wav.writeUInt16LE(1,20);wav.writeUInt16LE(1,22);wav.writeUInt32LE(24000,24);wav.writeUInt32LE(48000,28);wav.writeUInt16LE(2,32);wav.writeUInt16LE(16,34);wav.write('data',36);wav.writeUInt32LE(wav.length-44,40);
for(let i=0;i<24000*3;i++)wav.writeInt16LE(Math.round(Math.sin(i*2*Math.PI*440/24000)*200),44+i*2);
const server=http.createServer(async(req,res)=>{
 const url=new URL(req.url,'http://localhost');const json=value=>{res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify(value));};
 if(url.pathname==='/preview.wav'){res.writeHead(200,{'Content-Type':'audio/wav'});return res.end(wav);}
 if(url.pathname==='/api/me/notification-settings')return json({sound:true,voice:'zf_001'});
 if(url.pathname==='/api/announcements')return json(url.searchParams.get('after')==='1'?[]:[{id:'1',version:'1',text:'测试公告',createdAt:Date.now(),expiresAt:Date.now()+300000}]);
 if(url.pathname==='/api/announcements/1/audio')return json({state:'ready',url:'/preview.wav'});
 if(url.pathname==='/api/announcements/1/claim'){const value=!claimed;claimed=true;if(value)successfulClaims++;return json({claimed:value});}
 if(url.pathname==='/test'){res.writeHead(200,{'Content-Type':'text/html'});return res.end(`<div id="announcements"></div><script type="module">import{AnnouncementPlayer}from'/phase11/shared/AnnouncementPlayer/AnnouncementPlayer.js';import{BusinessApi}from'/phase11/shared/BusinessApi/BusinessApi.js';let api=new BusinessApi();api.csrf='fixture';window.player=new AnnouncementPlayer(api,document.querySelector('#announcements'));await player.start();window.ready=true;</script>`);}
 const file=path.resolve(root,'frontend',url.pathname.replace(/^\/phase11\//,'phase11/'));
 if(!file.startsWith(path.join(root,'frontend')+path.sep)||!fs.existsSync(file)){res.writeHead(404);return res.end();}
 res.writeHead(200,{'Content-Type':file.endsWith('.js')?'text/javascript':'text/css'});fs.createReadStream(file).pipe(res);
});
(async()=>{await new Promise(r=>server.listen(0,'127.0.0.1',r));const base='http://127.0.0.1:'+server.address().port;
 const browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true,args:['--autoplay-policy=user-gesture-required']});
 try{
  const first=await browser.newPage();
  // Explicitly inject one autoplay denial, then exercise native Audio after the user click.
  await first.addInitScript(()=>{const play=HTMLMediaElement.prototype.play;let blocked=false;HTMLMediaElement.prototype.play=function(){if(!blocked){blocked=true;return Promise.reject(new DOMException('fixture autoplay denied','NotAllowedError'));}return play.call(this);};});
  await first.goto(base+'/test');await first.waitForFunction(()=>window.player?.resume&&!player.resume.hidden);
  assert.equal(successfulClaims,1);assert.match(await first.locator('.announcement-sound small').innerText(),/自动播放/);
  await first.getByRole('button',{name:'继续播报'}).click();await first.waitForFunction(()=>player.audio&&!player.audio.paused);
  await first.evaluate(()=>document.dispatchEvent(new CustomEvent('nebula:recording',{detail:true})));assert.ok(await first.evaluate(()=>player.audio.paused));
  await first.evaluate(()=>document.dispatchEvent(new CustomEvent('nebula:recording',{detail:false})));await first.waitForFunction(()=>!player.audio.paused);
  const second=await browser.newPage();await second.goto(base+'/test');await second.waitForFunction(()=>window.ready);await second.waitForTimeout(200);
  assert.equal(successfulClaims,1);assert.equal(await second.evaluate(()=>Boolean(player.audio)),false,'second tab must not create another announcement audio');
  const lifecycle=await second.evaluate(async()=>{const{AnnouncementPlayer}=await import('/phase11/shared/AnnouncementPlayer/AnnouncementPlayer.js');const count=document.querySelectorAll('.announcement-sound').length;let resolve;const pending=new Promise(r=>resolve=r),test=new AnnouncementPlayer({request:()=>pending},document.createElement('div'));const start=test.start();test.dispose();resolve({sound:false,voice:'zf_001'});await start;return document.querySelectorAll('.announcement-sound').length===count;});assert.ok(lifecycle,'late settings must not resurrect a disposed player');
  await first.evaluate(()=>player.dispose());await second.evaluate(()=>player.dispose());console.log('Announcement autoplay-denial recovery/native playback, recording pause/resume, two-tab claim and disposed lifecycle passed');
 }finally{await browser.close();server.closeAllConnections();await new Promise(r=>server.close(r));}
})().catch(error=>{console.error(error);process.exitCode=1;server.closeAllConnections();server.close();});
