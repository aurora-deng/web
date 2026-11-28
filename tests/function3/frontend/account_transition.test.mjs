import test from 'node:test';
import assert from 'node:assert/strict';
import {accountEntry, readAccountSession, workspaceReturn} from '../../../frontend/phase11/shared/AccountSession/AccountSession.js';
import {TreePageTransition} from '../../../frontend/phase11/tree/TreePageTransition/TreePageTransition.js';

test('workspace login return stays within declared local workspaces and preserves the selected panel', () => {
  const origin = 'http://localhost:18091';
  assert.equal(workspaceReturn('/phase11/admin/permissions/', origin), '/phase11/admin/permissions/');
  assert.equal(workspaceReturn('/phase11/agent-control/', origin), '/phase11/agent-control/');
  const path = '/phase11/study/#download-settings';
  const entry = new URL(accountEntry(path, {change:true, origin}), origin);
  assert.equal(entry.searchParams.get('returnTo'), path);
  assert.equal(entry.searchParams.get('account'), 'change');
  for (const input of ['//other.test/', 'https://other.test/phase11/', '/phase11/../file', '/phase11//other.test/', '/phase11/%2f%2fexample.test', '/phase11/callback/']) {
    assert.equal(workspaceReturn(input,origin), '');
  }
  assert.equal(workspaceReturn('/phase11/tree/?account=change&topic=study#rank',origin),'/phase11/tree/?topic=study#rank');
});

test('concurrent workspace session reads share only a pending request', async () => {
  let requests=0, finish;
  const first = readAccountSession(() => {++requests;return new Promise(resolve => {finish=resolve;});});
  const second = readAccountSession(() => {++requests;});
  assert.equal(first,second);
  await Promise.resolve();
  finish({user:{id:'7'}});
  assert.equal((await first).user.id,'7');
  assert.equal((await readAccountSession(() => {++requests;return {user:{id:'8'}};})).user.id,'8');
  assert.equal(requests,2);
});

test('tree navigation blurs at the swap without an opacity flash and honors reduced motion', async () => {
  const element = () => ({append(){}, remove(){}, setAttribute(){}, removeAttribute(){}, style:{}});
  const spans = [];
  const root = {createElement:element,body:element(),documentElement:{scrollHeight:2000}};
  const surface = {...element(), animate(frames, options) {spans.push([frames,options]);return {cancel(){}, finished:Promise.resolve()};}};
  let current = 'tree';
  let reduce = false;
  const viewport = {scrollY:0,innerHeight:700,scrollTo(){}, matchMedia:()=>({matches:reduce})};
  const transition = new TreePageTransition({root,viewport,surface,current:()=>current,apply:value=>{current=value;}});
  assert.equal(await transition.navigate('feed',{direction:1,source:'wheel'}), true);
  assert.equal(current,'feed');
  assert.equal(spans.length,2);
  assert.equal(spans[0][0].at(-1).filter,spans[1][0][0].filter);
  assert(spans[0][0].at(-1).opacity>=.6);
  reduce=true;
  assert.equal(await transition.navigate('rank',{direction:1,source:'wheel'}), true);
  assert.equal(spans.length,2);
  transition.dispose();
});
