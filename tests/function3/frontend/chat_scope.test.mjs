import test from 'node:test';
import assert from 'node:assert/strict';
import {createChatScope,assertChatAssembly} from '../../../frontend/phase11/chat/ChatScope/ChatScope.js';

test('controller sees declared ports only and resolves cooperating commands lazily',()=>{
  const context={state:{account:1},secret:{key:'not-a-port'},types:{Formatter:class{}},actions:{}};
  const scope=createChatScope(context,{resources:['state'],commands:['send'],types:['Formatter']});
  assert.equal(scope.state,context.state);assert.equal(scope.secret,undefined);assert.equal(scope.actions.removeUser,undefined);
  assert.throws(()=>scope.actions.send(),/unavailable/);
  context.actions.send=value=>'accepted '+value;
  assert.equal(scope.actions.send('one'),'accepted one');
  context.actions.send=value=>'new account '+value;
  assert.equal(scope.actions.send('two'),'new account two');
  assert.throws(()=>{scope.actions.send=()=>{};},TypeError);
});
test('missing required dependencies fail at assembly instead of a later button click',()=>{
  assert.throws(()=>createChatScope({types:{},actions:{}},{resources:['database']}),/Missing chat dependency/);
});
test('completed composition rejects a declared command with no installed provider',()=>{
  const context={actions:{}};const contracts=[{commands:['send']},{commands:['search']}];
  assert.throws(()=>assertChatAssembly(context,contracts),/Missing chat command: send/);
  context.actions.send=()=>{};
  assert.throws(()=>assertChatAssembly(context,contracts),/Missing chat command: search/);
  context.actions.search=()=>{};assert.doesNotThrow(()=>assertChatAssembly(context,contracts));
});
test('all module subscriptions are revoked by the same page lifecycle',()=>{
  const lifecycle=new AbortController(),target=new EventTarget();let calls=0;
  const scope=createChatScope({types:{},actions:{},runtime:{lifecycle}},{events:true});
  scope.listen(target,'message',()=>calls++);target.dispatchEvent(new Event('message'));
  lifecycle.abort();target.dispatchEvent(new Event('message'));assert.equal(calls,1);
});
