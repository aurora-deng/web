/* Local-only browser fixture: real modules, synthetic users; no real database. */
const http=require('node:http'),fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const root=path.resolve(__dirname,'../../..'),front=path.resolve(process.env.CHAT_FIXTURE_FRONT||path.join(root,'frontend/phase11'));
const now=Date.now(),members=[{userId:1,role:'owner',displayName:'林川'},{userId:2,role:'administrator',displayName:'苏然'},{userId:3,role:'member',displayName:'陈雨'}];
const messages=[
  [1,1,'树下的晚风很舒服，今天也辛苦啦。',190000],
  [2,2,'把日常的小事放在这里，慢慢聊就好。',181000],
  [3,3,'花开的时候，像下了一场粉色的雪。',170000],
  [4,2,'这是一条发送了两分半的消息，可以在三分钟内撤回。',150000],
  [5,1,'右侧留一点空白，消息读起来更轻松。',130000],
  [6,3,'刚刚试了图片和长消息，输入框一直贴在底部。\n下次再一起去看树吧。',1000],
].map(([id,senderId,body,age])=>({id,sequence:id,conversationId:1,senderId,body,createdAt:now-age,clientMessageId:'fixture-'+id,state:'active',attachments:[]}));
const stories=messages.map(message=>({
 id:String(message.id),authorId:'2',authorName:'苏然',body:message.body,createdAt:message.createdAt,
 categoryId:'1',tags:['日常'],media:[],likes:'1',repliers:'0',liked:false,capabilities:{},
}));
let recalls=0;const events=new Set(),sockets=new Set();
const summary=()=>({id:1,kind:'group',title:'树下闲谈',state:'active',createdBy:1,nextSequence:messages.length+1,unreadCount:0,lastReadSequence:messages.length});
const server=http.createServer((req,res)=>{
 const url=new URL(req.url,'http://localhost'),json=value=>{res.writeHead(200,{'Content-Type':'application/json','Cache-Control':'no-store'});res.end(JSON.stringify(value));};
 if(url.pathname==='/events'){res.writeHead(200,{'Content-Type':'text/event-stream'});res.write(': fixture\n\n');events.add(res);req.on('close',()=>events.delete(res));return;}
 if(url.pathname==='/fixture/report')return json({recalls,kind:'synthetic UI fixture, recall ACK only; broadcast deliberately dropped'});
 if(url.pathname==='/api/me')return json({user:{id:2,username:'fixture',displayName:'苏然'},csrfToken:'fixture-only'});
 if(url.pathname==='/api/conversations')return json([summary()]);
 if(url.pathname.endsWith('/members'))return json(members);
 if(url.pathname.endsWith('/messages'))return json(messages.filter(m=>!url.searchParams.has('after')||m.sequence>Number(url.searchParams.get('after'))));
 if(url.pathname==='/api/admin/me')return json({role:'administrator'});
 if(url.pathname==='/api/tree/categories')return json([{id:'1',name:'日常'}]);
 if(url.pathname==='/api/tree/posts')return json({posts:stories,total:'100',next:'0'});
 if(url.pathname==='/api/tree/ranking')return json({version:'1',settledAt:now,entries:[]});
 if(url.pathname==='/api/profiles/2')return json({id:'2',displayName:'苏然',username:'fixture',biography:'愿留住花开的每一刻。'});
 if(url.pathname==='/api/local/session/info')return json({available:false});
 if(url.pathname.startsWith('/api/'))return json([]);
 const relative=url.pathname.slice('/phase11/'.length)+(url.pathname.endsWith('/')?'index.html':''),file=path.resolve(front,relative);
 if(!url.pathname.startsWith('/phase11/')||!file.startsWith(front+path.sep)||!fs.existsSync(file)||!fs.statSync(file).isFile()){res.writeHead(404);return res.end();}
 res.writeHead(200,{'Content-Type':file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':file.endsWith('.jpg')?'image/jpeg':'text/html','Cache-Control':'no-store'});fs.createReadStream(file).pipe(res);
});
function send(socket,value){const body=Buffer.from(JSON.stringify(value));const head=Buffer.alloc(body.length<126?2:4);head[0]=0x81;if(body.length<126)head[1]=body.length;else{head[1]=126;head.writeUInt16BE(body.length,2);}socket.write(Buffer.concat([head,body]));}
server.on('upgrade',(req,socket)=>{
 if(req.url!=='/ws'){socket.destroy();return;}
 const accept=crypto.createHash('sha1').update(req.headers['sec-websocket-key']+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
 socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: '+accept+'\r\n\r\n');
 sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});let buffer=Buffer.alloc(0);
 socket.on('data',chunk=>{buffer=Buffer.concat([buffer,chunk]);while(buffer.length>=6){
  const opcode=buffer[0]&15;let length=buffer[1]&127,offset=2;if(length===126){if(buffer.length<4)return;length=buffer.readUInt16BE(2);offset=4;}if(length===127||length>65535){socket.destroy();return;}
  if(buffer.length<offset+4+length)return;const mask=buffer.subarray(offset,offset+4),body=Buffer.from(buffer.subarray(offset+4,offset+4+length));buffer=buffer.subarray(offset+4+length);for(let i=0;i<body.length;i++)body[i]^=mask[i%4];
  if(opcode===8){socket.end();return;}if(opcode!==1)continue;
  const value=JSON.parse(body.toString());
  if(value.type==='chat.recall'){
   const message=messages.find(m=>String(m.id)===value.replyTo);
   if(!message||Date.now()-message.createdAt>180000){send(socket,{type:'chat.error',replyTo:value.id,status:'message recall window has expired'});continue;}
   recalls++;message.state='recalled';message.body='';message.attachments=[];
   send(socket,{type:'chat.recall.accepted',status:'recalled',replyTo:value.id,id:String(message.id),conversationId:1,sequence:message.sequence});
  }else if(value.type==='chat.send'){
   const message={id:messages.length+1,sequence:messages.length+1,conversationId:1,senderId:2,body:value.content,createdAt:Date.now(),state:'active',attachments:[],clientMessageId:value.id};messages.push(message);
   send(socket,{...message,type:'chat.message'});send(socket,{type:'chat.accepted',replyTo:value.id,id:String(message.id),sequence:message.sequence,conversationId:1,status:'inserted'});
  }
 }});
});
server.listen(Number(process.env.CHAT_FIXTURE_PORT||18761),'127.0.0.1',()=>console.log('Chat fixture http://127.0.0.1:'+server.address().port+'/phase11/'));
for(const sig of ['SIGINT','SIGTERM'])process.on(sig,()=>{events.forEach(e=>e.end());sockets.forEach(s=>s.destroy());server.close(()=>process.exit(0));});
