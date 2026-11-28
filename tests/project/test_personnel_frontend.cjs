const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.resolve(__dirname, '../../frontend/phase11/admin/permissions/app.js'), 'utf8').replace(/^import .*;$/gm, '');
const settle = async () => {for (let i = 0; i < 15; i++) await Promise.resolve();};
class Element {
  constructor(tag) {this.tagName = tag; this.children = []; this.value = ''; this.hidden = false; this.open = false; this.disabled = false; this.textContent = ''; this.listeners = {};}
  append(...children) {this.children.push(...children);}
  replaceChildren(...children) {this.children = children;}
  setAttribute(key, value) {this[key] = value;}
  toggleAttribute(key, value) {this[key] = value;}
  addEventListener(key, callback) {this.listeners[key] = callback;}
  showModal() {this.open = true;}
  close() {this.open = false;}
  focus() {}
}
function environment(options = {}) {
  const ids = new Map(); const calls = []; const writes = [];
  const get = id => {if (!ids.has(id)) ids.set(id, new Element(id)); return ids.get(id);};
  const user = {id:'7', username:'root', name:'<script>not markup</script>', role:'administrator', active:true};
  const page = options.page || [user];
  class BusinessApi {async request(descriptor, request) {
    calls.push(descriptor);
    if (descriptor.op === 'access') {if (options.noControl) throw {status:404}; return {id:'7'};}
    if (descriptor.op === 'users') {if (options.readError) throw options.readError; return page;}
    if (descriptor.op === 'write') {writes.push(request.data); if (options.write) return options.write(); return {ok:true};}
  }}
  const context = {document:{getElementById:get, createElement:tag=>new Element(tag)},
    BusinessApi, mountNavigation:async()=>({user:{id:'7', displayName:'root'}, role:options.role || 'administrator'}),
    adminApi:{getAdminUsers:args=>({op:'users', args}), postAdminUsersUser:args=>({op:'write',args})},
    sharedApi:{getPrivateAgentControlAccess:()=>({op:'access'})}, console};
  vm.runInNewContext(source, context);
  return {get, calls, writes, user, options};
}
test('personnel workspace renders inert text and only exposes explicitly permitted control shortcut', async()=>{
  const env = environment(); await settle();
  assert.equal(env.get('workspace').hidden, false);
  assert.equal(env.get('people').children[0].children[0].textContent, '<script>not markup</script>');
  assert.equal(env.get('control-entry').hidden, false);
  const denied = environment({noControl:true}); await settle(); assert.equal(denied.get('control-entry').hidden, true);
});
test('ordinary accounts cannot read users and a 403 clears the personnel data', async()=>{
  const normal = environment({role:'user'}); await settle(); assert.equal(normal.get('workspace').hidden, true); assert.equal(normal.calls.length, 0);
  const denied = environment({readError:{status:403}}); await settle(); assert.equal(denied.get('people').children.length, 0); assert.equal(denied.get('workspace').hidden, true);
});
test('role change requires nonblank reason, serializes writes and uses existing admin endpoint', async()=>{
  let finish; const env = environment({write:()=>new Promise(resolve=>{finish=resolve;})}); await settle();
  const row = env.get('people').children[0]; row.children[1].children[0].value = 'moderator'; row.children[3].children[0].onclick();
  assert.equal(env.get('confirm').open, true);
  await env.get('change-form').onsubmit({preventDefault(){}}); assert.equal(env.writes.length, 0);
  env.get('reason').value = 'requested test role change'; const pending = env.get('change-form').onsubmit({preventDefault(){}}); await settle();
  await env.get('change-form').onsubmit({preventDefault(){}}); assert.equal(env.writes.length, 1);
  assert.deepEqual(JSON.parse(JSON.stringify(env.writes[0])), {action:'role', reason:'requested test role change', role:'moderator'});
  finish(); await pending; assert.equal(env.get('confirm').open, false);
});
test('uncertain writes never retry automatically and warn to reconcile via audit first', async()=>{
  const env = environment({write:async()=>{throw {uncertain:true};}}); await settle();
  env.get('people').children[0].children[3].children[2].onclick(); env.get('reason').value = 'revoke test';
  await env.get('change-form').onsubmit({preventDefault(){}}); await settle();
  assert.equal(env.writes.length, 1); assert.equal(env.get('confirm').open, false); assert.match(env.get('status').textContent, /审计/);
});
test('filter is local and pagination is bounded by the server page size', async()=>{
  const page = Array.from({length:50}, (_,i)=>({id:String(i+1), username:'person_'+i, name:'Person '+i, role:'user', active:true}));
  const env = environment({page}); await settle(); assert.equal(env.get('more').hidden, false);
  env.get('filter').value = 'person_49'; env.get('filter').oninput(); assert.equal(env.get('people').children.length, 1);
  await env.get('more').onclick(); assert.equal(env.calls.filter(c=>c.op==='users').at(-1).args.query.after, '50');
});
