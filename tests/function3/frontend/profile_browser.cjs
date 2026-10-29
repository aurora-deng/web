/* Real Edge + real profile frontend; server replies are deterministic fixtures.
 * Permission enforcement is separately exercised by activities_blackbox.py. */
const http=require('node:http'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const{chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
const root=path.resolve(__dirname,'../../..');let writes=[],posts=[];
const profile={id:'1',username:'profile_user',displayName:'小满',biography:'记录日常，认真回应。',avatarUrl:'',self:true,showBiography:true,showAvatar:true};
const server=http.createServer(async(req,res)=>{
 const url=new URL(req.url,'http://localhost'),json=(body,status=200)=>{res.writeHead(status,{'Content-Type':'application/json'});res.end(JSON.stringify(body));};
 if(url.pathname==='/events'){res.writeHead(200,{'Content-Type':'text/event-stream'});return res.write(': ready\n\n');}
 if(['POST','PATCH','DELETE'].includes(req.method)){let raw='';for await(const chunk of req)raw+=chunk;const data=JSON.parse(raw||'{}');writes.push({path:url.pathname,method:req.method,data,csrf:req.headers['x-csrf-token']});
  if(url.pathname==='/api/activities'){const p={id:String(posts.length+1),authorId:'1',authorName:'小满',...data,media:[],tags:[],likes:'0',repliers:'0',createdAt:Date.now(),treeChannel:data.destination!=='profile',profileChannel:data.destination!=='tree',capabilities:{delete:true}};posts.unshift(p);return json(p,201);}
  if(url.pathname==='/api/me'){Object.assign(profile,data);return json({user:profile});}
  if(url.pathname==='/api/me/profile-display'){Object.assign(profile,{showBiography:data.showBiography==='true',showAvatar:data.showAvatar==='true'});return json({ok:true});}
  return json({ok:true});
 }
 if(url.pathname==='/api/me')return json({user:profile,csrfToken:'profile-test-csrf'});
 if(url.pathname==='/api/profiles/1')return json(profile);
 if(url.pathname==='/api/profiles/2')return json({...profile,id:'2',username:'friend',displayName:'树下的朋友',self:false});
 if(url.pathname==='/api/tree/categories')return json([{id:'1',name:'生活'}]);
 if(url.pathname==='/api/activities')return json(posts);
 if(url.pathname==='/api/activities/favorites')return json({items:[],nextCursor:'0'});
 if(url.pathname.startsWith('/api/'))return json([]);
 const relative=url.pathname==='/phase11/profile/'?'profile/index.html':url.pathname.slice('/phase11/'.length),file=path.join(root,'frontend/phase11',relative);
 if(!url.pathname.startsWith('/phase11/')||!fs.existsSync(file)){res.writeHead(404);return res.end();}
 res.writeHead(200,{'Content-Type':file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html'});fs.createReadStream(file).pipe(res);
});
(async()=>{await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const base=`http://127.0.0.1:${server.address().port}`;const browser=await chromium.launch({headless:true,executablePath:process.env.CHAT_TEST_BROWSER});
try{const page=await browser.newPage({viewport:{width:1440,height:900}}),errors=[];page.on('pageerror',e=>errors.push(e.message));await page.goto(base+'/phase11/profile/');await page.waitForFunction(()=>document.querySelector('#public-name').textContent==='小满');
 await page.locator('#publish-activity').click();assert.equal(await page.locator('[name=destination]').inputValue(),'profile');assert.equal(await page.locator('[name=visibility]').inputValue(),'friends');
 await page.locator('#profile-publish-form [name=body]').fill('这个日常只分享给好友。');await page.locator('#profile-publish-form [type=submit]').click();await page.waitForFunction(()=>!document.querySelector('#profile-publish-dialog').open);assert.equal(writes[0].data.visibility,'friends');assert.equal(writes[0].csrf,'profile-test-csrf');
 await page.locator('#publish-activity').click();await page.locator('[name=destination]').selectOption('both');await page.locator('[name=categoryId]').selectOption('1');await page.locator('#profile-publish-form [name=body]').fill('这条同时挂到树上。');const count=writes.length;await page.locator('#profile-publish-form [type=submit]').click();assert.equal(writes.length,count,'public confirmation must not be bypassed');
 await page.locator('[name=publicConfirmed]').check();await page.locator('#profile-publish-form [type=submit]').click();await page.waitForFunction(()=>!document.querySelector('#profile-publish-dialog').open);
 assert.equal(writes.at(-1).data.publicConfirmed,'true');assert.equal(writes.at(-1).data.destination,'both');
 await page.locator('#publish-activity').click();assert.equal(await page.locator('[name=publicConfirmed]').isChecked(),false);await page.locator('[data-close-publisher]').first().click();
 for(const size of[{width:1440,height:900},{width:1920,height:1080},{width:390,height:844}]){await page.setViewportSize(size);assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));await page.screenshot({path:path.join(root,`build-function3/profile-${size.width}.png`)});}
 await page.locator('[data-section=information]').click();await page.locator('#profile-edit-form [name=displayName]').fill('小满的新名字');await page.locator('#profile-edit-form [type=submit]').click();await page.waitForFunction(()=>document.querySelector('#public-name').textContent==='小满的新名字');
 await page.goto(base+'/phase11/profile/?user=2');await page.waitForFunction(()=>document.querySelector('#public-name').textContent==='树下的朋友');assert.equal(await page.locator('#profile-edit-form').isVisible(),false);assert.equal(await page.locator('#publish-activity').isVisible(),false);assert.equal(await page.locator('#favorites-tab').isVisible(),false);
 assert.deepEqual(errors,[]);console.log('PASS profile three viewports, own/public areas, unified publisher confirmation/reset and real form handlers');
}finally{await browser.close();server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}})().catch(error=>{console.error(error);process.exitCode=1;server.closeAllConnections();server.close();});
