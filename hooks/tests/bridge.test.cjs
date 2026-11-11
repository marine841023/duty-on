// End-to-end normalization tests. Run with Node.js on Windows; no HTTP server
// or real user configuration is touched. Exercise Windows PowerShell 5.1, the
// runtime actually used by installed hooks (including its byte encoding).
const {spawnSync} = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const assert = require('node:assert/strict');

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'dutyon-bridge-测试-'));
const bridge = path.resolve(__dirname, '../trae-hook-bridge.ps1');
const capture = path.join(root, 'capture.json');
const quote = value => `'${value.replaceAll("'", "''")}'`;
let count = 0;
function run(input, ide = 'codex', event = '', extraEnv = {}) {
  if (fs.existsSync(capture)) fs.unlinkSync(capture);
  const script = `
function Invoke-RestMethod {
  param($Uri, $Method, $Body, $ContentType, $TimeoutSec, $ErrorAction)
  if ($Body -isnot [byte[]]) { throw 'Body must be explicit UTF-8 bytes' }
  if ($ContentType -ne 'application/json; charset=utf-8') { throw 'Missing UTF-8 charset' }
  [IO.File]::WriteAllBytes($env:BRIDGE_CAPTURE, $Body)
}
& ${quote(bridge)} -Ide ${quote(ide)} -HookEvent ${quote(event)}
`;
  const result = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand',
    Buffer.from(script, 'utf16le').toString('base64')], {
    input: typeof input === 'string' ? input : JSON.stringify(input),
    encoding: 'utf8', timeout: 10000,
    env: {...process.env, USERPROFILE: root, BRIDGE_CAPTURE: capture,
      TRAE_PROJECT_DIR: 'D:/wrong/trae', QODER_PROJECT_DIR: 'D:/wrong/qoder',
      CODEX_PROJECT_DIR: 'D:/fallback/codex', ...extraEnv}
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
  const reply = ide === 'cursor' && ['preToolUse', 'subagentStart'].includes(event) ? '{"permission":"allow"}' :
    ide === 'cursor' && event === 'beforeSubmitPrompt' ? '{"continue":true}' : '';
  assert.equal(result.stdout.trim(), reply, 'only Cursor observer acknowledgement may use stdout');
  count++;
  return fs.existsSync(capture) ? JSON.parse(fs.readFileSync(capture, 'utf8')) : null;
}
try {
  let payload = run({session_id: 'session-中文', hook_event_name: 'PreToolUse',
    cwd: 'D:\\工程\\测试项目\\', tool_name: 'Bash', tool_use_id: 'call-1',
    turn_id: 'turn-1', agent_id: 'agent-1', parent_session_id: 'parent-1', request_id: 'request-1',
    prompt: 'SECRET_PROMPT', tool_input: {command: 'SECRET_COMMAND'}});
  assert.ok(payload, 'valid Codex event must be forwarded');
  assert.equal(payload.session_id, 'session-中文');
  assert.equal(payload.project_path, 'D:\\工程\\测试项目\\');
  assert.equal(payload.project_name, '测试项目');
  assert.equal(payload.turn_id, 'turn-1');
  assert.equal(payload.tool_use_id, 'call-1');
  assert.equal(payload.agent_id, 'agent-1');
  assert.equal(payload.parent_session_id, 'parent-1');
  assert.equal(payload.request_id, 'request-1');
  assert.equal(payload.ide, 'codex');
  assert.equal(payload.prompt, undefined);
  assert.equal(payload.tool_input, undefined);

  payload = run({session_id: 's', hook_event_name: 'SessionStart', source: 'compact',
    project_path: 'D:/explicit/project', cwd: 'D:/different/cwd'});
  assert.equal(payload.project_path, 'D:/explicit/project');
  assert.equal(payload.source, 'compact');
  payload = run({session_id: 's', hook_event_name: 'Stop'});
  assert.equal(payload.project_path, 'D:/fallback/codex', 'fallback must use only matching IDE env');
  payload = run({conversation_id: 'cursor-id', workspace_roots: ['D:/cursor/workspace'],
    toolInfo: {name: 'AskUserQuestion'}}, 'cursor', 'preToolUse');
  assert.equal(payload.hook_event_name, 'PreToolUse');
  assert.equal(payload.session_id, 'cursor-id');
  assert.equal(payload.project_name, 'workspace');
  assert.equal(payload.tool_name, 'AskUserQuestion');
  payload = run({conversation_id: 'conversation', session_id: 'cross-conversation',
    generation_id: 'generation', subagent_id: 'child', parent_conversation_id: 'parent'}, 'cursor', 'subagentStart');
  assert.equal(payload.session_id, 'conversation');
  assert.equal(payload.agent_id, 'child');
  assert.equal(payload.parent_session_id, 'parent');
  assert.equal(payload.turn_id, 'generation');
  for (const name of ['sessionEnd', 'postToolUseFailure', 'subagentStop', 'preCompact']) {
    payload = run({conversation_id: 'c'}, 'cursor', name);
    assert.equal(payload.hook_event_name, name[0].toUpperCase() + name.slice(1));
  }
  assert.equal(run('{broken', 'cursor', 'preToolUse'), null, 'unavailable observer must still respond');
  payload = run('\uFEFF' + JSON.stringify({session_id: 'bom', hook_event_name: 'Interrupt', cwd: 'D:/p'}));
  assert.equal(payload.hook_event_name, 'Interrupt');
  for (const input of ['{broken SECRET_INVALID_JSON', 'null', '[]', '"scalar"', '',
    {hook_event_name: 'Stop'}, {session_id: 'no-event'}]) assert.equal(run(input), null);
  assert.equal(run({workspace_roots: ['D:/p']}, 'cursor', 'stop'), null,
    'missing Cursor id must not merge into a fabricated session');
  const log = fs.readFileSync(path.join(root, '.dutyon/hooks/bridge.log'), 'utf8');
  assert.ok(!log.includes('SECRET_'), 'diagnostics must not persist raw input');
  console.log(`All ${count} Windows bridge scenarios passed`);
} finally {
  fs.rmSync(root, {recursive: true, force: true});
}
