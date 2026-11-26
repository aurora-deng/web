"use strict";
// Node-only DOM contract checks: no browser, dependency installation, or real HTTP.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const crypto = require("node:crypto");
const root = path.resolve(__dirname, "../..");
const assetRoot = process.argv[2] || path.join(root, "frontend/phase11/agent-control");
const source = fs.readFileSync(path.join(assetRoot, "app.js"), "utf8");
const html = fs.readFileSync(path.join(assetRoot, "index.html"), "utf8");
const schema = {type: "object", additionalProperties: false, properties: {taskId: {type: "string"}}};
const skills = [
  {name: "fleet.get_task_status", version: 1, effect: "read", parameters: schema, timeoutMs: 1500, availability: "configured"},
  {name: "study.log_progress", version: 1, effect: "external", parameters: {type: "object"}, timeoutMs: 2500, availability: "configured"},
  {name: "fleet.task.reassign", version: 1, effect: "proposal", parameters: schema, timeoutMs: 1000, availability: "configured"},
  {name: "metrics.query", version: 1, effect: "read", parameters: schema, timeoutMs: 4000, availability: "unavailable"}
];
function environment(options = {}) {
  const ids = new Map(), calls = [];
  class Element {
    constructor(tag) { this.tag = tag; this.children = []; this.value = ""; this.checked = false; this.disabled = false; this._text = ""; }
    set textContent(value) { this._text = String(value); this.children = []; }
    get textContent() { return this._text + this.children.map(child => child.textContent).join(""); }
    append(...children) {
      this.children.push(...children);
      if (this.tag === "select" && !this.value && children.length) this.value = children[0].value;
    }
    replaceChildren(...children) { this.children = []; this._text = ""; if (this.tag === "select") this.value = ""; this.append(...children); }
    prepend(child) { this.children.unshift(child); }
    set innerHTML(value) { register(value); }
  }
  function register(markup) {
    for (const match of markup.matchAll(/<([a-z]+)\b[^>]*\bid="([^"]+)"[^>]*>/g)) {
      const element = new Element(match[1]); element.disabled = /\bdisabled\b/.test(match[0]); ids.set(match[2], element);
    }
    if (ids.has("ai-operation")) ids.get("ai-operation").value = "ai.model.activate";
  }
  register(html);
  const main = new Element("main");
  const context = vm.createContext({
    document: {getElementById: id => ids.get(id) || null, createElement: tag => new Element(tag), querySelector: selector => selector === "main" ? main : null},
    crypto, Date, console,
    fetch: async (url, request) => {
      const route = url.split("/").pop(), body = request.body === undefined ? undefined : JSON.parse(request.body);
      calls.push({route, request, body});
      if (options.handle) { const custom = await options.handle(route, request, body); if (custom) return custom; }
      const results = {access: {id: "7", approver: false, administrator: false, csrfToken: "synthetic-csrf", ...options.access}, catalog: {skills}, requests: {requests: options.requests || []}, proposals: {proposals: options.proposals || []}};
      return {ok: true, status: 200, json: async () => body === undefined ? results[route] || {} : {ok: true}};
    }
  });
  vm.runInContext(source, context, {filename: "agent-control/app.js"});
  return {ids, calls, context, evaluate: value => vm.runInContext(value, context)};
}
async function settle() { for (let i = 0; i < 10; ++i) await new Promise(resolve => setImmediate(resolve)); }
function buttons(element) { return element.children.flatMap(child => child.tag === "button" ? [child] : buttons(child)); }
function choose(env, name) { env.ids.get("skill").value = name; env.evaluate("select()"); }
let passed = 0;
async function test(name, callback) { await callback(); ++passed; console.log("PASS " + name); }
(async () => {
  await test("UTF-8 text and honest account boundary", async () => {
    assert.match(source, /真实账号/); assert.doesNotMatch(source, /\?{3,}|preview-completed|demo-note|agent-progress-once/);
    assert.match(html, /真实账号数据边界/); assert.doesNotMatch(html, /隔离测试数据库|聊天与学习内容是合成样例|demo-chat/);
    assert.match(html, /id="identity"/); assert.match(html, /id="schema"/);
    for (const id of ["plan", "invoke"]) assert.match(html, new RegExp('<button[^>]*id="' + id + '"[^>]*disabled'));
  });
  await test("startup completes with real HTML element ids", async () => {
    const env = environment(); await settle(); assert.match(env.ids.get("identity").textContent, /当前账号 7/);
    assert.equal(env.ids.get("plan").disabled, false); assert.match(env.ids.get("schema").textContent, /additionalProperties/);
    assert.equal(JSON.parse(env.ids.get("parameters").value).taskId, "");
    assert.deepEqual(env.calls.map(call => call.route), ["access", "catalog", "requests", "proposals"]);
  });
  await test("failed authentication keeps actions disabled", async () => {
    const env = environment({handle: async route => route === "access" ? {ok: false, status: 403, json: async () => ({error: {code: "denied"}})} : null});
    await settle(); assert.equal(env.ids.get("invoke").disabled, true); assert.equal(env.calls.length, 1);
  });
  await test("unavailable skill cannot issue invocation", async () => {
    const env = environment(); await settle(); choose(env, "metrics.query");
    assert.equal(env.ids.get("invoke").disabled, true); await env.evaluate("run(false)");
    assert.equal(env.calls.filter(call => call.route === "invoke").length, 0);
  });
  await test("read uses same-origin, version, and CSRF", async () => {
    const env = environment(); await settle(); env.ids.get("parameters").value = '{"taskId":"actual-task"}';
    await env.evaluate("run(false)"); const call = env.calls.find(call => call.route === "invoke");
    assert.equal(call.request.credentials, "same-origin"); assert.equal(call.request.headers["X-CSRF-Token"], "synthetic-csrf");
    assert.deepEqual(call.body, {skillId: "fleet.get_task_status", version: 1, parameters: {taskId: "actual-task"}});
  });
  await test("plan does not invoke or submit writes", async () => {
    const env = environment(); await settle(); choose(env, "study.log_progress"); await env.evaluate("run(true)");
    assert.equal(env.calls.filter(call => call.request.method === "POST").map(call => call.route).join(), "plan");
  });
  await test("unconfirmed write creates no external request", async () => {
    const env = environment(); await settle(); choose(env, "study.log_progress"); await env.evaluate("run(false)");
    assert.equal(env.calls.filter(call => call.request.method === "POST").length, 0);
  });
  await test("confirmed write only submits approval, retry retains key", async () => {
    const env = environment(); await settle(); choose(env, "study.log_progress"); env.ids.get("confirmed").checked = true;
    const parameters = JSON.parse(env.ids.get("parameters").value); parameters.subject = "systems"; parameters.itemId = "actual-question";
    env.ids.get("parameters").value = JSON.stringify(parameters); await env.evaluate("run(false)"); await env.evaluate("run(false)");
    const posts = env.calls.filter(call => call.request.method === "POST"); assert.equal(posts.length, 2);
    assert.ok(posts.every(call => call.route === "requests")); assert.equal(posts[0].body.idempotencyKey, posts[1].body.idempotencyKey);
    assert.equal(posts[0].body.idempotencyKey, parameters.idempotencyKey); assert.match(parameters.idempotencyKey, /^agent-/);
  });
  await test("new selection gets a distinct write key", async () => {
    const env = environment(); await settle(); choose(env, "study.log_progress"); const first = JSON.parse(env.ids.get("parameters").value);
    choose(env, "study.log_progress"); assert.notEqual(JSON.parse(env.ids.get("parameters").value).idempotencyKey, first.idempotencyKey);
  });
  await test("invalid JSON creates no request and restores buttons", async () => {
    const env = environment(); await settle(); env.ids.get("parameters").value = "{"; await env.evaluate("run(false)");
    assert.equal(env.calls.filter(call => call.request.method === "POST").length, 0); assert.equal(env.ids.get("invoke").disabled, false);
  });
  await test("concurrent click does not duplicate invocation", async () => {
    const env = environment(); await settle(); await env.evaluate("Promise.all([run(false), run(false)])");
    assert.equal(env.calls.filter(call => call.route === "invoke").length, 1);
  });
  await test("requester cannot see self-approval or execute others' requests", async () => {
    const env = environment({access: {approver: true}, requests: [{id: "own", actor: "7", operation: "study.log_progress", state: "pending", expires: 9999999999}, {id: "other", actor: "9", operation: "study.log_progress", state: "approved", expires: 9999999999}]});
    await settle(); const labels = buttons(env.ids.get("proposals")).map(button => button.textContent);
    assert.deepEqual(labels, ["撤销申请"]);
  });
  await test("approver checks parameters before deciding others' requests", async () => {
    const env = environment({access: {approver: true}, requests: [{id: "other", actor: "9", operation: "study.log_progress", state: "pending", expires: 9999999999, parameters: {itemId: "inspect-me"}, reason: "review"}]});
    await settle(); assert.match(env.ids.get("proposals").textContent, /inspect-me/);
    const approve = buttons(env.ids.get("proposals")).find(button => button.textContent === "批准"); await approve.onclick();
    assert.deepEqual(env.calls.find(call => call.route === "decide").body, {requestId: "other", action: "approve"});
  });
  await test("approved owner executes explicitly and refreshes", async () => {
    const env = environment({requests: [{id: "own", actor: "7", operation: "study.log_progress", state: "approved", expires: 9999999999}]});
    await settle(); const execute = buttons(env.ids.get("proposals")).find(button => button.textContent === "执行已批准申请");
    await execute.onclick(); assert.deepEqual(env.calls.find(call => call.route === "execute").body, {requestId: "own"});
    assert.equal(env.calls.filter(call => call.route === "requests").length, 2);
  });
  await test("AI reconcile follows submitted state, not executing", async () => {
    const env = environment({requests: [{id: "own", actor: "7", operation: "ai.model.activate", state: "submitted", expires: 9999999999}]});
    await settle(); const reconcile = buttons(env.ids.get("proposals")).find(button => button.textContent === "核实异步结果");
    assert.ok(reconcile); await reconcile.onclick(); assert.deepEqual(env.calls.find(call => call.route === "reconcile").body, {requestId: "own"});
  });
  await test("AI requires confirmation and preserves request key on retry", async () => {
    const env = environment({access: {administrator: true}}); await settle();
    env.ids.get("ai-node").value = "2"; env.ids.get("ai-version").value = "3"; env.ids.get("ai-reason").value = "验证版本";
    await env.ids.get("ai-request").onclick(); assert.equal(env.calls.filter(call => call.request.method === "POST").length, 0);
    env.ids.get("ai-confirmed").checked = true; await env.ids.get("ai-request").onclick(); await env.ids.get("ai-request").onclick();
    const posts = env.calls.filter(call => call.request.method === "POST"); assert.equal(posts.length, 2); assert.equal(posts[0].body.idempotencyKey, posts[1].body.idempotencyKey);
  });
  await test("non-JSON gateway errors and refresh failures stay visible", async () => {
    const env = environment({handle: async route => route === "requests" ? {ok: false, status: 502, json: async () => { throw new Error("HTML"); }} : null});
    await settle(); await env.ids.get("refresh").onclick(); assert.match(env.ids.get("result").textContent, /有效 JSON/);
  });
  console.log(JSON.stringify({tests: passed, failures: 0, browserLaunched: false, externalNetwork: false}));
})().catch(error => { console.error(error); process.exitCode = 1; });
