import assert from 'node:assert/strict';
import test from 'node:test';
import { WebAssistantHistory } from '../../../frontend/phase11/shared/WebAssistantPanel/WebAssistantHistory.js';
import { modelConnection, requestOwnModel, restoreModelMode, validateConnection } from '../../../frontend/phase11/shared/ModelConnection/ModelConnection.js';
import { storyText } from '../../../frontend/phase11/tree/StoryPreview/StoryPreview.js';

test('XiaoYan keeps both legacy threads and shares the selected conversation across entry points', () => {
  const records = new Map();
  const storage = { getItem: key => records.get(key), setItem: (key, value) => records.set(key, value) };
  const message = content => ({ role:'user', content });
  for (const surface of ['friend', 'sidebar']) storage.setItem(`nebula:web-chat:78:${surface}`, JSON.stringify([message(surface)]));
  const drawer = new WebAssistantHistory('78', storage);
  assert.equal(drawer.threads.length, 2);
  drawer.select('legacy-sidebar');
  drawer.append(drawer.current, message('继续在同一会话'));
  assert.deepEqual(new WebAssistantHistory('78', storage).current.messages.map(item => item.content), ['sidebar', '继续在同一会话']);
  assert.equal(new WebAssistantHistory('79', storage).threads.length, 0);
  drawer.setConnection({provider:'ollama',endpoint:'http://127.0.0.1:11434',model:'reference',key:'never-store'});
  const reopened = new WebAssistantHistory('78', storage);
  restoreModelMode(reopened.mode, reopened.ollama);
  assert.deepEqual(modelConnection(), {provider:'ollama',endpoint:'http://127.0.0.1:11434',model:'reference',key:''});
  drawer.setConnection({provider:'custom',endpoint:'https://secret-address.invalid',model:'custom',key:'never-store'});
  assert(!storage.getItem(drawer.key).includes('secret-address'));
  assert(!storage.getItem(drawer.key).includes('never-store'));
  restoreModelMode('custom');
  assert.equal(modelConnection().endpoint, '');
});

test('Ollama uses its native loopback wire and compatible providers receive only their own credentials', async context => {
  const calls = [];
  context.mock.method(globalThis, 'fetch', async (target, options) => {
    calls.push({target, options, body:JSON.parse(options.body)});
    return Response.json(target.endsWith('/api/chat') ? {message:{content:'local reply'}} : {choices:[{message:{content:'api reply'}}]});
  });
  const local = {provider:'ollama',endpoint:'http://127.0.0.1:11434',model:'test-local',key:'unused'};
  const api = {provider:'custom',endpoint:'https://provider.invalid/v1/chat/completions',model:'test-api',key:'sample-key'};
  const history = [{role:'user',content:'test message'}];
  assert.equal(await requestOwnModel(local, history), 'local reply');
  assert.equal(calls[0].target, 'http://127.0.0.1:11434/api/chat');
  assert.equal(calls[0].body.options.num_predict, 512);
  assert.equal(calls[0].options.headers.Authorization, undefined);
  assert.equal(await requestOwnModel(api, history), 'api reply');
  assert.equal(calls[1].options.headers.Authorization, 'Bearer sample-key');
  assert.equal(calls[1].options.redirect, 'error');
  assert.throws(() => validateConnection({...local,endpoint:'https://other.invalid/api/chat'}), /本机/);
  assert.throws(() => validateConnection({...api,endpoint:'https://user:secret@provider.invalid/v1/chat/completions'}), /地址/);
});

test('Stories preserve their authentic title and mark built-in samples without fake likes or counts', () => {
  assert.deepEqual(storyText({bundledExample:true,body:'【站内示例】学习复盘\n\n复盘内容',media:[]}), {
    example:true, image:undefined, voice:false, title:'学习复盘', excerpt:'复盘内容',
  });
  assert.equal(storyText({body:'【站内示例】普通用户发布的内容'}).example, false);
  assert.equal(storyText({body:'',media:[{kind:'audio'}]}).title, '语音故事');
});
