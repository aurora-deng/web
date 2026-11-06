import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
const load=async path=>{
  let source=await readFile(new URL('../../../'+path,import.meta.url),'utf8');
  if(path.endsWith('/ChatBackdrop.js')){
    const artwork=await readFile(new URL('../../../frontend/phase11/chat/ChatBackdrop/ChatBackdropArtwork.js',import.meta.url),'utf8');
    const uri='data:text/javascript;base64,'+Buffer.from(artwork).toString('base64');
    source=source.replace(/(['"])\.\/ChatBackdropArtwork\.js(?:\?v=[^'"]*)?\1/,JSON.stringify(uri));
  }
  return import('data:text/javascript;base64,'+Buffer.from(source).toString('base64'));
};
const {matchesRecallReceipt,recalledFromReceipt,watchRecallExpiry}=await load('frontend/phase11/chat/MessageRecall/MessageRecall.js');
const {backdropMarkup,mountChatBackdrop}=await load('frontend/phase11/chat/ChatBackdrop/ChatBackdrop.js');
const pending={commandId:'recall-1',id:'41',conversationId:'7',sequence:'15'};
const receipt={type:'chat.recall.accepted',status:'recalled',replyTo:'recall-1',id:'41',conversationId:7,sequence:15};
const original={id:41,conversationId:7,sequence:15,senderId:3,createdAt:1234,state:'active',body:'private text',attachments:[{fileId:1}]};

test('服务器撤回 ACK 独立于广播清除正文和附件，重复 ACK 保持终态',()=>{
  const recalled=recalledFromReceipt(receipt,pending,original);
  assert.equal(recalled.state,'recalled');assert.equal(recalled.body,'');assert.deepEqual(recalled.attachments,[]);
  assert.equal(original.body,'private text'); // No mutation of a cached snapshot.
  assert.deepEqual(recalledFromReceipt({...receipt,status:'already_recalled'},pending,recalled),recalled);
});
test('跨账号旧操作、不同消息/会话/序号和错误状态不能伪造撤回成功',()=>{
  for(const patch of [{id:'42'},{conversationId:8},{sequence:16},{replyTo:'old-command'},
    {status:'failed'},{type:'chat.accepted'}]){
    assert.equal(matchesRecallReceipt({...receipt,...patch},pending),false);
    assert.equal(recalledFromReceipt({...receipt,...patch},pending,original),null);
  }
  assert.equal(recalledFromReceipt(receipt,pending,{...original,conversationId:8}),null);
  assert.equal(recalledFromReceipt(receipt,undefined,original),null);
  assert.equal(recalledFromReceipt(receipt,pending,undefined),null);
});
test('可见撤回入口在180秒后移除，只有一个定时器，销毁会释放观察器和定时器',()=>{
  let now=180000,mutation,disconnected=false,job,id=0;
  const buttons=[{dataset:{expiresAt:'180000'},remove(){this.removed=true;}},{dataset:{expiresAt:'180050'},remove(){this.removed=true;}}];
  const list={querySelectorAll:()=>buttons.filter(b=>!b.removed)};
  class Observer{constructor(callback){mutation=callback;}observe(){}disconnect(){disconnected=true;}}
  const timers={setTimeout(fn,delay){job={fn,delay,id:++id};return id;},clearTimeout(timer){if(job?.id===timer)job=null;}};
  const lifecycle=new AbortController();
  watchRecallExpiry(list,{Observer,timers,clock:()=>now,signal:lifecycle.signal});
  assert.equal(job.delay,1);assert.equal(buttons[0].removed,undefined);
  now=180001;job.fn();assert.equal(buttons[0].removed,true);assert.equal(job.delay,50);
  mutation();assert.equal(job.delay,50);
  lifecycle.abort();assert.equal(job,null);assert.equal(disconnected,true);
});
test('神树背景只生成固定数量粒子，模块内嵌图片不请求未注册的图片路由',async()=>{
  const markup=backdropMarkup();
  assert.equal((markup.match(/class="chat-tree-drift /g)||[]).length,20);
  assert.equal((markup.match(/<img /g)||[]).length,1);
  assert.match(markup,/src="data:image\/jpeg;base64,/);
  assert.doesNotMatch(markup,/https?:|<image|<script|<foreignObject/);
  assert.equal(markup,backdropMarkup());
  const bytes=Buffer.from(/src="data:image\/jpeg;base64,([A-Za-z0-9+/=]+)"/.exec(markup)[1],'base64');
  assert.equal(bytes.readUInt16BE(0),0xffd8);
  assert.ok(bytes.length<512*1024,'Background download must stay below 512 KiB');
  assert.equal(createHash('sha256').update(bytes).digest('hex').slice(0,12),'5d7c79f59e36');
  assert.deepEqual(bytes,await readFile(new URL('../../../frontend/phase11/chat/ChatBackdrop/sakura-lake-5d7c79f59e36.jpg',import.meta.url)));
});
test('背景隐藏时暂停，恢复时继续；重复挂载不增加节点，页面销毁移除背景',()=>{
  let art,observe,disconnected=false;const handlers=new Map();
  class Observer{constructor(fn){observe=fn;}observe(){}disconnect(){disconnected=true;}}
  const doc={hidden:false,defaultView:{IntersectionObserver:Observer},
    createElement(){return{setAttribute(){},classList:{toggle(name,value){this[name]=value;}},remove(){art=null;}};},
    addEventListener(name,fn){handlers.set(name,fn);},removeEventListener(name){handlers.delete(name);}};
  const pane={ownerDocument:doc,querySelector:()=>art,prepend(node){art=node;}};
  const lifecycle=new AbortController();mountChatBackdrop(pane,{signal:lifecycle.signal});
  assert.equal(art.classList['is-paused'],true);observe([{isIntersecting:true}]);assert.equal(art.classList['is-paused'],false);
  const first=art;mountChatBackdrop(pane);assert.equal(art,first);
  doc.hidden=true;handlers.get('visibilitychange')();assert.equal(art.classList['is-paused'],true);
  lifecycle.abort();assert.equal(art,null);assert.equal(handlers.size,0);assert.equal(disconnected,true);
});
