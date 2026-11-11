// Exercise the exact embedded installer source with an isolated event bus and
// HTTP/log mocks. No IDE process, user configuration or network is touched.
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const cpp = fs.readFileSync(path.resolve(__dirname, '../../device/src/backend/hooks_installer.cpp'), 'utf8');
const source = cpp.match(/kOpencodePluginSource = R"dutyon_src\(([\s\S]*?)\)dutyon_src";/)[1];
const payloads = [], logs = [];
let timers = 0, failFetch = false;
const context = vm.createContext({
  fs: { mkdirSync() {}, readFileSync: () => '', writeFileSync() {}, appendFileSync: (_, value) => logs.push(value) },
  os: { homedir: () => '/isolated' }, path, AbortController,
  setTimeout() { ++timers; return timers; }, clearTimeout() { --timers; },
  fetch: async (_, options) => {
    if (failFetch) throw new Error('unavailable');
    payloads.push(JSON.parse(options.body)); return {status: 200};
  }
});
vm.runInContext(source.replace(/^import .*$/gm, '').replace('export default', 'globalThis.plugin ='), context);
(async () => {
  const hooks = await context.plugin.server();
  const event = (type, properties) => hooks.event({event: {type, properties}});
  const since = start => payloads.slice(start).map(e => e.hook_event_name);
  await event('session.created', {info: {id: 'parent', directory: 'D:/工程/demo'}});
  await event('session.created', {info: {id: 'child', parentID: 'parent'}});
  await event('session.status', {sessionID: 'parent', status: {type: 'busy'}});
  let start = payloads.length;
  await event('message.updated', {info: {id: 'msg', role: 'user', sessionID: 'parent'}});
  for (const text of ['SECRET_PROMPT', 'SECRET_PROMPT_MORE'])
    await event('message.part.updated', {part: {type: 'text', messageID: 'msg', text}});
  assert.deepEqual(since(start), [], 'text stream must not reset an active turn');
  const part = {id: 'part', sessionID: 'child', type: 'tool', tool: 'bash', callID: 'call'};
  start = payloads.length;
  for (const status of ['pending', 'running', 'running', 'completed', 'completed', 'running'])
    await event('message.part.updated', {part: {...part, state: {status, output: 'SECRET_OUTPUT'}}});
  assert.deepEqual(since(start), ['UserPromptSubmit', 'PreToolUse', 'PostToolUse']);
  assert.equal(payloads.at(-1).parent_session_id, 'opencode-parent');
  assert.equal(payloads.at(-1).tool_use_id, 'call');
  assert.equal(payloads.at(-1).project_name, 'demo');
  await event('permission.asked', {id: 'p1', sessionID: 'child', tool: {callID: 'c2'}});
  await event('permission.replied', {requestID: 'p1', sessionID: 'child', reply: 'reject'});
  assert.equal(payloads.at(-1).hook_event_name, 'PermissionResolved');
  assert.equal(payloads.at(-1).request_id, 'permission:p1');
  assert.equal(payloads.at(-1).tool_use_id, 'c2');
  for (const version of ['', '.v2']) {
    start = payloads.length;
    await event('question' + version + '.asked', {id: 'q' + version, sessionID: 'child'});
    await event('question' + version + '.asked', {id: 'q' + version, sessionID: 'child'});
    await event('question' + version + '.rejected', {requestID: 'q' + version, sessionID: 'child'});
    assert.deepEqual(since(start), ['PermissionRequest', 'PermissionResolved']);
  }
  start = payloads.length;
  await event('permission.replied', {requestID: 'early-reply', sessionID: 'inactive', reply: 'once'});
  await event('permission.asked', {id: 'early-reply', sessionID: 'inactive'});
  await event('permission.replied', {requestID: 'early-reply', sessionID: 'inactive', reply: 'once'});
  assert.deepEqual(since(start), ['PermissionResolved'], 'late asked must not reopen an inactive resolved request');
  await event('message.part.updated', {part: {...part, callID: 'failed', state: {status: 'error'}}});
  assert.equal(payloads.at(-1).hook_event_name, 'PostToolUseFailure');
  await event('session.status', {sessionID: 'parent', status: {type: 'idle'}});
  start = payloads.length;
  await event('session.status', {sessionID: 'parent', status: {type: 'retry'}});
  assert.deepEqual(since(start), ['UserPromptSubmit', 'PreModelRequest'], 'retry after Stop must reopen work');
  await event('session.deleted', {info: {id: 'child'}});
  assert.equal(payloads.at(-1).hook_event_name, 'SessionEnd');
  await event('session.created', {info: {id: 'child', directory: 'D:/new'}});
  assert.equal(payloads.at(-1).parent_session_id, undefined, 'deleted session metadata must be cleared');
  failFetch = true;
  await event('session.status', {sessionID: 'parent', status: {type: 'idle'}});
  assert.equal(timers, 0, 'failed fetch must release its deadline timer');
  assert.ok(!JSON.stringify(payloads).includes('SECRET_'));
  assert.ok(!logs.join('').includes('SECRET_'));
  console.log('All OpenCode event-stream regression checks passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
