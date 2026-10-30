/* Deterministic synthetic UI/render comparison. Real API ACL is tested separately. */
const {chromium}=require(process.env.PLAYWRIGHT_MODULE);
const fs=require('node:fs'),path=require('node:path'),http=require('node:http'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'../../..'),output=path.join(root,'build-function3/phase48-ui/tree');fs.mkdirSync(output,{recursive:true});
let source=root,requests=[];
const posts=Array.from({length:24},(_,i)=>({id:String(100-i),authorId:'1',authorName:'树下的旅人',body:'把今天的小小快乐，挂在花开处。',categoryId:'1',mood:'平静',tags:['日常'],media:[],createdAt:1729000000000,version:'1',likes:'2',repliers:'1',liked:false,visibility:'members',capabilities:{delete:true}}));
const ranks=posts.slice(0,10).map((p,i)=>({postId:p.id,score:10-i}));
const server=http.createServer((req,res)=>{
  const url=new URL(req.url,'http://localhost');requests.push(url.pathname+url.search);
  if(url.pathname.startsWith('/api/')){
    let result={};
    if(url.pathname==='/api/me')result={user:{id:'1',displayName:'树下的旅人'},csrfToken:'synthetic-ui'};
    else if(url.pathname==='/api/admin/me')result={role:'user'};
    else if(url.pathname==='/api/ai/web/models')result=[{accountId:'1',name:'测试模型',runtime:'onnx'}];
    else if(url.pathname==='/api/activities/100')result={...posts[0],treeChannel:true,profileChannel:true};
    else if(url.pathname==='/api/tree/posts/100/replies')result=[];
    else if(url.pathname==='/api/local/session/info')result={available:false};
    else if(url.pathname==='/api/tree/categories')result=[{id:'1',name:'生活'}];
    else if(url.pathname==='/api/tree/ranking')result={version:'1',settledAt:1729000000000,entries:ranks};
    else if(url.pathname==='/api/tree/posts'){const ids=url.searchParams.get('ids')?.split(',');result={total:'100',posts:ids?posts.filter(p=>ids.includes(p.id)):posts};}
    else if(/^\/api\/tree\/posts\/\d+$/.test(url.pathname))result=posts.find(p=>p.id===url.pathname.split('/').pop());
    else if(url.pathname==='/api/me/notification-settings')result={sound:false,voice:'zf_001'};
    else if(url.pathname==='/api/announcements')result=[];
    else if(url.pathname==='/api/me/speech/status')result={enabled:false};
    res.writeHead(200,{'Content-Type':'application/json'});return res.end(JSON.stringify(result));
  }
  if(url.pathname==='/fixture.html'){
    res.setHeader('Content-Type','text/html');return res.end(`<!doctype html><html><head><style>body{margin:0;background:radial-gradient(ellipse,#313044,#171d2a)}#scene{height:100vh}</style><script type="importmap">{"imports":{"three":"/phase11/vendor/three-r186/three.module.js"}}</script></head><body><div id="scene"></div><script type="module">import{TreeScene}from'/phase11/tree/TreeScene/TreeScene.js';window.scene=new TreeScene(document.querySelector('#scene'),id=>window.selected=id);window.scene.update(${JSON.stringify(posts)},100,${JSON.stringify(ranks)});window.frameCount=0;const render=scene.renderer.render.bind(scene.renderer);scene.renderer.render=(...args)=>{frameCount++;return render(...args)};window.ready=true;</script></body></html>`);
  }
  let file=path.resolve(source,'frontend','.'+decodeURIComponent(url.pathname));
  if(!file.startsWith(path.join(source,'frontend')+path.sep)){res.writeHead(403);return res.end();}
  if(fs.existsSync(file)&&fs.statSync(file).isDirectory())file=path.join(file,'index.html');
  if(!fs.existsSync(file)){res.writeHead(404);return res.end();}
  res.setHeader('Content-Type',({'.js':'text/javascript','.css':'text/css','.html':'text/html','.svg':'image/svg+xml'})[path.extname(file)]||'application/octet-stream');res.end(fs.readFileSync(file));
});
(async()=>{
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const base='http://127.0.0.1:'+server.address().port;
  const browser=await chromium.launch({executablePath:process.env.CHAT_TEST_BROWSER,headless:true});
  const report={kind:'synthetic browser fixture; not real API/model evidence',variants:{}};
  try{
    for(const variant of ['after']){
      source=variant==='before'?path.join(root,'build-client/phase42-baseline/source'):root;
      const context=await browser.newContext({viewport:{width:1440,height:900}});const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
      await page.goto(base+'/fixture.html');await page.waitForFunction(()=>window.ready);await page.waitForTimeout(1000);
      const metrics=await page.evaluate(()=>({drawCalls:scene.renderer.info.render.calls,triangles:scene.renderer.info.render.triangles,geometries:scene.renderer.info.memory.geometries}));
      const rebuild=await page.evaluate(posts=>{const id=scene.fruits.geometry.uuid;for(let i=0;i<100;i++)scene.update(posts.map(p=>({...p,likes:String(i)})),100,posts.slice(0,10).map((p,j)=>({postId:p.id,score:100-j})));return scene.fruits.geometry.uuid!==id;},posts);
      metrics.rebuiltOnInteractionOnly=rebuild;
      report.variants[variant]=metrics;
      await page.evaluate(()=>window.frameCount=0);await page.waitForTimeout(2000);metrics.framesInTwoSeconds=await page.evaluate(()=>frameCount);
      if(variant==='after'){
        assert(!rebuild,'unchanged fruit placement reallocated GPU geometry');assert(metrics.drawCalls<=27,'static branches were not merged');
        assert(metrics.framesInTwoSeconds<=64,'render frequency exceeds desktop budget');
        await page.evaluate(()=>scene.setVisible(false));const paused=await page.evaluate(()=>frameCount);await page.waitForTimeout(350);assert.equal(await page.evaluate(()=>frameCount),paused,'hidden scene still renders');
        await page.evaluate(()=>scene.setVisible(true));await page.waitForTimeout(100);
        const click=await page.evaluate(()=>{scene.world.updateMatrixWorld(true);const p=scene.positions[0].position.clone();scene.world.localToWorld(p);p.project(scene.camera);return{x:(p.x+1)*innerWidth/2,y:(1-p.y)*innerHeight/2};});
        await page.mouse.click(click.x,click.y);assert(await page.evaluate(()=>Boolean(window.selected)),'fruit raycast failed after matrix update');
        await page.emulateMedia({reducedMotion:'reduce'});await page.reload();await page.waitForFunction(()=>window.ready);await page.waitForTimeout(300);const still=await page.evaluate(()=>frameCount);await page.waitForTimeout(300);assert.equal(await page.evaluate(()=>frameCount),still,'reduced-motion scene is not on-demand');
      }
      await page.goto(base+'/phase11/tree/');await page.waitForFunction(()=>document.querySelector('#tree-count')?.textContent.includes('100'));await page.waitForTimeout(700);
      requests=[];await page.reload();await page.waitForFunction(()=>document.querySelector('#ranking')?.children.length===10);await page.waitForTimeout(700);
      metrics.rankingDetailRequests=requests.filter(p=>/^\/api\/tree\/posts\/\d+$/.test(p)||p.startsWith('/api/tree/posts?limit=10&ids=')).length;
      if(variant==='after'){
        assert.equal(metrics.rankingDetailRequests,1,'ranking still fans out per fruit');
        for(const size of [{width:1440,height:900},{width:1920,height:1080},{width:390,height:844}]){
          await page.setViewportSize(size);await page.reload();await page.waitForFunction(()=>document.querySelector('#tree-count')?.textContent.includes('100'));await page.waitForTimeout(300);assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+2),'horizontal overflow');
          const layout=await page.evaluate(()=>{const bounds=selector=>{const r=document.querySelector(selector).getBoundingClientRect();return {left:r.left,right:r.right,top:r.top,bottom:r.bottom,width:r.width,height:r.height};};return {heading:bounds('.scene-heading'),scene:bounds('#scene'),controls:bounds('.scene-controls'),visible:getComputedStyle(document.querySelector('.scene-heading')).display!=='none'};});
          (metrics.viewportLayouts ||= []).push({width:size.width,...layout});
          if(size.width>760){assert(layout.heading.width>=180,'old overlay max-width shrank the caption grid');assert(layout.heading.right<=layout.scene.left+2,'caption overlaps 3D hit-test viewport');assert(layout.heading.bottom<layout.controls.top,'caption overlaps scene controls');}
          else assert(!layout.visible,'mobile decorative caption was not collapsed');
          await page.screenshot({path:path.join(output,`tree-${size.width}.png`)});
        }
        await page.getByRole('button',{name:/故事广场/}).click();assert.equal(await page.locator('.tree-stage').isVisible(),false);
        await page.getByRole('button',{name:/神树全景/}).click();assert.equal(await page.locator('.tree-stage').isVisible(),true);
      }else await page.screenshot({path:path.join(output,'tree-before.png')});
      assert.deepEqual(errors,[]);report.variants[variant]=metrics;await context.close();
    }
    report.passed=true;console.log(JSON.stringify(report,null,2));
  }finally{fs.writeFileSync(path.join(output,'summary.json'),JSON.stringify(report,null,2));await browser.close();server.close();}
})().catch(error=>{console.error(error);server.close();process.exitCode=1;});
