'use strict';
// Duty On 官网与云空间。纯原生脚本，只访问本站接口。

const $ = id => document.getElementById(id);
const el = (tag, cls, text) => {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
};
const fmtSize = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MiB' : Math.max(n / 1024, 0.1).toFixed(1) + ' KiB';
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)').matches;

// ---------------------------------------------------------------- 主题色
const INKS = ['amber', 'ice', 'white', 'green', 'pink'];
function setInk(name) {
  if (!INKS.includes(name)) name = 'amber';
  document.documentElement.dataset.ink = name;
  document.querySelectorAll('.ink').forEach(b => b.setAttribute('aria-checked', String(b.dataset.ink === name)));
  try { localStorage.setItem('dutyon-ink', name); } catch (_) { /* 隐私模式下不保存 */ }
}
document.querySelectorAll('.ink').forEach(b => b.addEventListener('click', () => setInk(b.dataset.ink)));
let savedInk = 'amber';
try { savedInk = localStorage.getItem('dutyon-ink') || 'amber'; } catch (_) { /* 同上 */ }
setInk(savedInk);

// ---------------------------------------------------------------- 首屏的设备屏演示
const STATUS = new Map([
  ['idle', ['lamp-idle', '空闲']],
  ['thinking', ['lamp-thinking', '思考中']],
  ['tool', ['lamp-tool', '执行中']],
  ['working', ['lamp-working', '忙碌']],
  ['alert', ['lamp-alert', '需要确认']],
]);
const DEMO_PROJECTS = [['traeSprite', 'A'], ['ndmp', 'T'], ['megrez-cloud', 'C'], ['docs-site', 'X']];
// 一轮演示：每一步是四个项目各自的状态
const DEMO_STEPS = [
  ['tool', 'thinking', 'working', 'idle'],
  ['thinking', 'tool', 'working', 'idle'],
  ['tool', 'alert', 'working', 'idle'],
  ['tool', 'alert', 'thinking', 'idle'],
  ['thinking', 'tool', 'idle', 'idle'],
  ['idle', 'thinking', 'idle', 'idle'],
  ['idle', 'idle', 'idle', 'idle'],
];
function renderDemo(step) {
  const list = $('scr-tiles');
  list.replaceChildren();
  let overall = 'sleeping';
  DEMO_PROJECTS.forEach(([name, tag], i) => {
    const status = step[i];
    const [lampClass, label] = STATUS.get(status);
    if (status === 'alert') overall = 'alert';
    else if (status !== 'idle' && overall !== 'alert') overall = 'working';
    const li = el('li', 'tile');
    li.dataset.status = status;
    const lamp = el('span', 'lamp ' + lampClass, status === 'alert' ? '!' : undefined);
    lamp.setAttribute('aria-hidden', 'true');
    li.append(lamp, el('span', 'name', name), el('span', 'tag tag-' + tag, tag), el('span', 'state', label));
    list.append(li);
  });
  $('screen').dataset.state = overall;
}
let demoIndex = 0;
renderDemo(DEMO_STEPS[0]);
if (!reducedMotion) setInterval(() => { demoIndex = (demoIndex + 1) % DEMO_STEPS.length; renderDemo(DEMO_STEPS[demoIndex]); }, 3200);

function tickClock() {
  const now = new Date();
  const two = n => String(n).padStart(2, '0');
  $('scr-hm').textContent = two(now.getHours()) + ':' + two(now.getMinutes());
  $('scr-ss').textContent = two(now.getSeconds());
  $('scr-week').textContent = '星期' + '日一二三四五六'[now.getDay()];
  $('scr-day').textContent = (now.getMonth() + 1) + '月' + now.getDate() + '日';
}
tickClock();
setInterval(tickClock, 1000);

const CHARACTERS = ['nico', 'nietzsche', 'nito', 'ni-j', 'nipsilon'];
let charIndex = 0;
$('scr-stage').addEventListener('click', () => {
  charIndex = (charIndex + 1) % CHARACTERS.length;
  const img = $('scr-char');
  img.classList.add('swap');
  const next = new Image();
  next.onload = () => { img.src = next.src; img.classList.remove('swap'); };
  next.src = '/img/' + CHARACTERS[charIndex] + '.png';
});

// ---------------------------------------------------------------- 接口
let token = '';
try { token = sessionStorage.getItem('dutyon-token') || ''; } catch (_) { /* 无存储时每次重新登录 */ }
function saveToken(value) {
  token = value;
  try { value ? sessionStorage.setItem('dutyon-token', value) : sessionStorage.removeItem('dutyon-token'); } catch (_) { /* 同上 */ }
}
let toastTimer = 0;
function toast(message, error = false) {
  const box = $('status');
  clearTimeout(toastTimer);
  box.textContent = message;
  box.hidden = !message;
  box.classList.toggle('error', error);
  if (message && !error) toastTimer = setTimeout(() => { box.hidden = true; }, 5000);
}
async function api(path, options) {
  if (!options) options = Object.create(null);
  const headers = Object.assign(Object.create(null), options.headers);
  if (token) headers.Authorization = 'Bearer ' + token;
  let body;
  if (options.json !== undefined) { headers['Content-Type'] = 'application/json'; body = JSON.stringify(options.json); }
  const res = await fetch(path, { method: options.method || 'GET', headers, body, cache: 'no-store', credentials: 'omit' });
  if (!res.ok) {
    let message = '请求失败（' + res.status + '）';
    try { const data = await res.json(); if (typeof data.error === 'string') message = data.error; } catch (_) { /* 非 JSON 响应 */ }
    if (res.status === 401 && token) { saveToken(''); showSpace(false); }
    const failure = new Error(message);
    failure.status = res.status;
    throw failure;
  }
  return res;
}
// 上传要显示进度，fetch 做不到，用 XMLHttpRequest
function putFile(path, file, conditions, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('PUT', '/api/sync/file?path=' + encodeURIComponent(path));
    xhr.setRequestHeader('Authorization', 'Bearer ' + token);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    for (const [key, value] of Object.entries(conditions)) xhr.setRequestHeader(key, value);
    xhr.upload.onprogress = e => { if (e.lengthComputable) onProgress(e.loaded / e.total); };
    xhr.onerror = () => reject(new Error('网络中断，上传没有完成'));
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) return resolve();
      let message = '上传失败（' + xhr.status + '）';
      try { const data = JSON.parse(xhr.responseText); if (typeof data.error === 'string') message = data.error; } catch (_) { /* 非 JSON 响应 */ }
      reject(new Error(message));
    };
    xhr.send(file);
  });
}

// ---------------------------------------------------------------- 下载
const DOWNLOADS = [
  ['installer', '安装程序', '第一次安装选这个。双击运行，按提示装好即可。'],
  ['bundle', '安装程序 ZIP', '浏览器或杀毒软件拦截 exe 时用：解压后运行里面的安装程序。'],
  ['update', '更新包', '已安装的桌宠会自己检查并升级，一般不用手动下载。'],
];
async function loadReleases() {
  const box = $('releases');
  try {
    const releases = await (await api('/api/app/releases')).json();
    box.replaceChildren();
    for (const [kind, title, what] of DOWNLOADS) {
      const release = releases[kind];
      if (!release) continue;
      const card = el('a', 'dl' + (kind === 'installer' ? ' first' : ''));
      card.href = release.downloadUrl;
      card.setAttribute('download', '');
      const meta = el('span', 'meta');
      meta.append(el('span', '', 'v' + release.version), el('span', '', fmtSize(release.size)));
      card.append(el('strong', '', title), el('span', 'what', what), meta, el('span', 'get', '下载'));
      box.append(card);
    }
    if (!box.children.length) { box.append(el('p', 'muted', '安装包还没有发布，过一阵再来。')); return; }
    const main = releases.installer || releases.bundle;
    if (main) {
      const cta = $('hero-download');
      cta.textContent = '下载 Windows 版 v' + main.version;
      cta.href = main.downloadUrl;
      cta.setAttribute('download', '');
      $('hero-fine').textContent = fmtSize(main.size) + '，原生 C++ 单进程，不带浏览器内核。';
      if (main.notes) $('release-note').textContent = '本版说明：' + main.notes;
    }
  } catch (_) {
    box.replaceChildren(el('p', 'muted', '暂时取不到下载列表，刷新页面再试一次。'));
  }
}

// ---------------------------------------------------------------- 云空间
// 按角色展示。数据来自三处：
//   /api/profile        用量
//   /api/sync/manifest  文件清单（animations/ 下是角色的动画和声音，live2d/ 下是模型）
//   /api/sync/config    桌宠设置的备份，其中 customCharacters 是角色列表，
//  stateAudio 是各状态绑定的声音
// 改角色 = 上传新文件 + 改设置 + 删旧文件。设置带版本号（rev），保存时用
// If-Match 防止和桌宠同时修改互相覆盖。
let manifest = [];
let config = Object.create(null);
let configRev = '';
let busy = false;
let openCharId = '';
let genCharId = '';  // 打开"重新生成"页的角色（页面原型在 gen.js）
const previews = new Map();  // path -> blob URL（退出登录时释放）
const IMAGE_EXT = /\.(gif|png|jpe?g)$/i;
const AUDIO_EXT = /\.(wav|mp3|ogg|m4a|flac)$/i;
const PREVIEW_LIMIT = 16 * 1048576;
const MAX_FILE = 50 * 1048576;
const STATES = [
  ['sleeping', '空闲中', 'lamp-idle', '所有任务都空闲时'],
  ['working', '忙碌中', 'lamp-working', '有任务正在跑时'],
  ['alert', '需要确认', 'lamp-alert', '有任务等你确认时'],
];

const characters = () => Array.isArray(config.customCharacters) ? config.customCharacters : [];
const fileOf = name => name ? manifest.find(f => f.path === 'animations/' + name) : undefined;
const soundOf = (id, state) => {
  const all = config.stateAudio;
  return all && all[id] && typeof all[id][state] === 'string' ? all[id][state] : '';
};
function referencedNames() {
  const names = new Set();
  for (const c of characters()) for (const [state] of STATES) if (c[state]) names.add(c[state]);
  const audio = config.stateAudio;
  if (audio && typeof audio === 'object')
    for (const key of Object.keys(audio))
      for (const state of Object.keys(audio[key] || Object.create(null))) names.add(audio[key][state]);
  return names;
}

function showSpace(logged) {
  $('auth-panel').hidden = logged;
  $('space-panel').hidden = !logged;
  if (!logged) {
    manifest = [];
    config = Object.create(null);
    configRev = '';
    openCharId = '';
    genCharId = '';
    for (const url of previews.values()) URL.revokeObjectURL(url);
    previews.clear();
    $('space-title').textContent = '云空间';
    $('space-sub').hidden = false;
  }
}
async function loadConfig() {
  const data = await (await api('/api/sync/config')).json();
  config = data.config && typeof data.config === 'object' ? data.config : Object.create(null);
  configRev = typeof data.rev === 'string' ? data.rev : '';
}
async function refresh() {
  const [profile, files] = await Promise.all([
    api('/api/profile').then(r => r.json()),
    api('/api/sync/manifest').then(r => r.json()),
    loadConfig(),
  ]);
  manifest = files;
  $('space-title').textContent = profile.username + ' 的云空间';
  $('space-sub').hidden = true;
  const ratio = profile.quotaBytes ? profile.usedBytes / profile.quotaBytes : 0;
  $('meter-fill').style.width = Math.min(100, ratio * 100).toFixed(1) + '%';
  $('meter').classList.toggle('full', ratio > 0.9);
  $('usage').textContent = '已用 ' + fmtSize(profile.usedBytes) + '，共 ' + fmtSize(profile.quotaBytes) + '。';
  showSpace(true);
  render();
}

// 改设置：取最新一版 → 改 → 带版本号保存；别处刚改过（412）就重读再改一次
async function saveConfig(mutate) {
  for (let attempt = 0; attempt < 3; attempt++) {
    const next = JSON.parse(JSON.stringify(config));
    mutate(next);
    const headers = Object.create(null);
    if (configRev) headers['If-Match'] = '"' + configRev + '"';
    try {
      const data = await (await api('/api/sync/config', { method: 'PUT', json: next, headers })).json();
      config = next;
      configRev = typeof data.rev === 'string' ? data.rev : '';
      return;
    } catch (e) {
      if (e.status !== 412) throw e;
      await loadConfig();
    }
  }
  throw new Error('设置一直在被别处修改，没有保存成功。稍等一会儿再试。');
}
async function deleteCloudFile(name) {
  if (!name) return;
  try { await api('/api/sync/file?path=' + encodeURIComponent('animations/' + name), { method: 'DELETE' }); } catch (_) { /* 旧文件删不掉不影响结果，会出现在"没有归属的文件"里 */ }
}

// ---- 预览、试听、下载 ----
const lazy = 'IntersectionObserver' in window ? new IntersectionObserver(items => {
  for (const item of items) {
    if (!item.isIntersecting) continue;
    lazy.unobserve(item.target);
    loadPreview(item.target, item.target.dataset.path);
  }
}, { rootMargin: '200px' }) : null;
async function loadPreview(box, path) {
  try {
    let url = previews.get(path);
    if (!url) {
      url = URL.createObjectURL(await (await api('/api/sync/file?path=' + encodeURIComponent(path))).blob());
      previews.set(path, url);
    }
    const img = el('img');
    img.alt = '';
    img.src = url;
    box.replaceChildren(img);
  } catch (_) { /* 预览失败就保留文字占位 */ }
}
function previewBox(cls, file, placeholder) {
  const box = el('div', cls, placeholder);
  if (file && IMAGE_EXT.test(file.path) && file.size <= PREVIEW_LIMIT) {
    box.dataset.path = file.path;
    if (previews.has(file.path) || !lazy) loadPreview(box, file.path); else lazy.observe(box);
  }
  return box;
}
async function downloadFile(file) {
  const blob = await (await api('/api/sync/file?path=' + encodeURIComponent(file.path))).blob();
  const url = URL.createObjectURL(blob);
  const a = el('a');
  a.href = url;
  a.download = file.path.split('/').pop();
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}
async function playFile(file, button) {
  const playing = document.querySelector('audio[data-playing]');
  if (playing) { playing.pause(); playing.remove(); }
  if (button.dataset.on) { delete button.dataset.on; button.textContent = '试听'; return; }
  document.querySelectorAll('button[data-on]').forEach(b => { delete b.dataset.on; b.textContent = '试听'; });
  const blob = await (await api('/api/sync/file?path=' + encodeURIComponent(file.path))).blob();
  const audio = new Audio(URL.createObjectURL(blob));
  audio.dataset.playing = '1';
  audio.hidden = true;
  document.body.append(audio);
  const stop = () => { URL.revokeObjectURL(audio.src); audio.remove(); delete button.dataset.on; button.textContent = '试听'; };
  audio.onended = stop;
  audio.onpause = stop;
  button.dataset.on = '1';
  button.textContent = '停止';
  await audio.play();
}

// ---- 小部件 ----
function action(text, handler, cls) {
  const button = el('button', 'btn' + (cls ? ' ' + cls : ''), text);
  button.type = 'button';
  button.addEventListener('click', () => {
    if (busy) return;
    Promise.resolve(handler(button)).catch(e => toast(e.message, true));
  });
  return button;
}
// 两步确认：第一次点变成珊瑚色的"确认…"，三秒内再点才执行
function confirmButton(text, confirmText, handler, cls) {
  const button = el('button', 'btn' + (cls ? ' ' + cls : ''), text);
  button.type = 'button';
  let armed = 0;
  button.addEventListener('click', async () => {
    if (busy) return;
    if (!armed) {
      button.textContent = confirmText;
      button.classList.add('btn-danger');
      armed = setTimeout(() => { armed = 0; button.textContent = text; button.classList.remove('btn-danger'); }, 3000);
      return;
    }
    clearTimeout(armed);
    armed = 0;
    await run(handler);
  });
  return button;
}
function filePicker(text, accept, handler, cls) {
  const label = el('label', 'btn file-btn' + (cls ? ' ' + cls : ''), text);
  const input = el('input');
  input.type = 'file';
  input.accept = accept;
  input.addEventListener('change', () => {
    const file = input.files[0];
    input.value = '';
    if (file) run(() => handler(file));
  });
  label.append(input);
  return label;
}
// 所有改动都走这里：串行执行、出错提示、完成后重画
async function run(task) {
  if (busy) return;
  busy = true;
  try { await task(); await refresh(); }
  catch (e) { toast(e.message, true); await refresh().catch(() => undefined); }
  finally { busy = false; $('progress').hidden = true; $('progress-fill').style.width = '0'; }
}
async function upload(path, file) {
  if (file.size > MAX_FILE) throw new Error(file.name + ' 超过 50 MiB，没有上传。请压缩后再试。');
  $('progress').hidden = false;
  toast('正在上传 ' + file.name);
  await putFile(path, file, { 'If-None-Match': '*' }, part => { $('progress-fill').style.width = (part * 100).toFixed(1) + '%'; });
}
const extOf = name => { const m = /\.[A-Za-z0-9]+$/.exec(name); return m ? m[0].toLowerCase() : ''; };

// ---- 角色的增删改 ----
async function createCharacter() {
  const id = 'char_' + Date.now();
  await saveConfig(cfg => {
    if (!Array.isArray(cfg.customCharacters)) cfg.customCharacters = [];
    cfg.customCharacters.push({ id, name: '新角色', sleeping: '', working: '', alert: '' });
  });
  openCharId = id;
  toast('角色建好了。先起个名字，再给三种状态各选一段动画。');
}
async function renameCharacter(id, name) {
  name = name.trim();
  if (!name) throw new Error('名字不能是空的。');
  await saveConfig(cfg => { const c = (cfg.customCharacters || []).find(x => x.id === id); if (c) c.name = name; });
  toast('已改名为 ' + name);
}
async function setAnimation(id, state, file) {
  if (!IMAGE_EXT.test(file.name)) throw new Error('动画只支持 GIF、PNG、JPG。');
  const old = (characters().find(c => c.id === id) || Object.create(null))[state];
  const name = id + '_' + state + '_' + Date.now() + extOf(file.name);
  await upload('animations/' + name, file);
  await saveConfig(cfg => { const c = (cfg.customCharacters || []).find(x => x.id === id); if (c) c[state] = name; });
  await deleteCloudFile(old);
  toast('动画已更换，桌宠会在一分钟内用上。');
}
async function clearAnimation(id, state) {
  const old = (characters().find(c => c.id === id) || Object.create(null))[state];
  await saveConfig(cfg => { const c = (cfg.customCharacters || []).find(x => x.id === id); if (c) c[state] = ''; });
  await deleteCloudFile(old);
  toast('动画已移除。');
}
async function setSound(id, state, file) {
  if (!AUDIO_EXT.test(file.name)) throw new Error('声音只支持 WAV、MP3、OGG、FLAC、M4A。');
  const old = soundOf(id, state);
  const name = 'audio_' + id.replace(/[^A-Za-z0-9_]/g, '_') + '_' + state + '_' + Date.now() + extOf(file.name);
  await upload('animations/' + name, file);
  await saveConfig(cfg => {
    if (!cfg.stateAudio || typeof cfg.stateAudio !== 'object') cfg.stateAudio = Object.create(null);
    if (!cfg.stateAudio[id]) cfg.stateAudio[id] = Object.create(null);
    cfg.stateAudio[id][state] = name;
  });
  await deleteCloudFile(old);
  toast('声音已更换，桌宠会在一分钟内用上。');
}
async function clearSound(id, state) {
  const old = soundOf(id, state);
  await saveConfig(cfg => { if (cfg.stateAudio && cfg.stateAudio[id]) delete cfg.stateAudio[id][state]; });
  await deleteCloudFile(old);
  toast('声音已移除。');
}
async function deleteCharacter(id) {
  const c = characters().find(x => x.id === id);
  if (!c) return;
  if (config.activeCharacterId === id) throw new Error('桌宠正在用这个角色。先在桌宠里换成别的角色，再回来删除。');
  const names = STATES.map(([state]) => c[state]).concat(STATES.map(([state]) => soundOf(id, state))).filter(Boolean);
  await saveConfig(cfg => {
    cfg.customCharacters = (cfg.customCharacters || []).filter(x => x.id !== id);
    if (cfg.stateAudio) delete cfg.stateAudio[id];
    if (cfg.stateAudioMuted) for (const key of Object.keys(cfg.stateAudioMuted)) if (key.startsWith(id + ':')) delete cfg.stateAudioMuted[key];
  });
  for (const name of names) await deleteCloudFile(name);
  openCharId = '';
  toast('已删除角色 ' + c.name);
}

// ---- 画面 ----
function render() {
  const generating = genCharId ? characters().find(c => c.id === genCharId) : undefined;
  const open = characters().find(c => c.id === openCharId);
  $('view-gen').hidden = !generating;
  $('view-home').hidden = !!open || !!generating;
  $('view-char').hidden = !open || !!generating;
  if (generating) renderGen(generating);
  else if (open) renderCharacter(open);
  else renderHome();
}

function renderHome() {
  const list = $('char-list');
  list.replaceChildren();
  for (const c of characters()) {
    const card = el('li');
    const button = el('button', 'char');
    button.type = 'button';
    const shown = fileOf(c.working) || fileOf(c.sleeping) || fileOf(c.alert);
    const lamps = el('span', 'char-states');
    for (const [state, label, lamp] of STATES) {
      const dot = el('span', 'lamp ' + lamp + (c[state] ? '' : ' off'), lamp === 'lamp-alert' ? '!' : undefined);
      dot.title = label + (c[state] ? '：有动画' : '：还没有动画') + (soundOf(c.id, state) ? '，有声音' : '');
      lamps.append(dot);
    }
    const sounds = STATES.filter(([state]) => soundOf(c.id, state)).length;
    const meta = el('span', 'sub', (sounds ? sounds + ' 个声音' : '没有声音') + (config.activeCharacterId === c.id ? '，桌宠正在用' : ''));
    button.append(previewBox('char-thumb', shown, (c.name || '?').slice(0, 1)), el('span', 'char-name', c.name || '没有名字'), lamps, meta);
    button.addEventListener('click', () => { openCharId = c.id; render(); window.scrollTo({ top: $('space').offsetTop, behavior: reducedMotion ? 'auto' : 'smooth' }); });
    // "重新生成"放在卡片右上角，和卡片按钮并列（按钮里不能再套按钮）
    const regen = el('button', 'char-regen', '重新生成');
    regen.type = 'button';
    regen.addEventListener('click', () => openGen(c.id));
    card.className = 'char-card';
    card.append(button, regen);
    list.append(card);
  }
  const add = el('li');
  const addButton = el('button', 'char char-new');
  addButton.type = 'button';
  addButton.append(el('span', 'char-plus', '+'), el('span', 'char-name', '新建角色'), el('span', 'sub', '用自己的动画做一个'));
  addButton.addEventListener('click', () => run(createCharacter));
  add.append(addButton);
  list.append(add);

  // Live2D：按模型文件夹合成一行
  const models = new Map();
  for (const file of manifest) {
    if (!file.path.startsWith('live2d/')) continue;
    const parts = file.path.split('/');
    const name = parts.length > 2 ? parts[1] : file.path.slice('live2d/'.length);
    let entry = models.get(name);
    if (!entry) { entry = { name, files: [], size: 0 }; models.set(name, entry); }
    entry.files.push(file);
    entry.size += file.size;
  }
  const modelList = $('model-list');
  modelList.replaceChildren();
  for (const entry of Array.from(models.values()).sort((a, b) => a.name.localeCompare(b.name, 'zh-CN'))) {
    const row = el('li', 'file');
    const thumb = el('div', 'thumb model', 'L2D');
    const body = el('div');
    body.append(el('div', 'name', entry.name), el('div', 'sub', entry.files.length + ' 个文件，' + fmtSize(entry.size)));
    const acts = el('div', 'acts');
    acts.append(confirmButton('删除', '确认删除 ' + entry.files.length + ' 个文件', async () => {
      for (const file of entry.files) await api('/api/sync/file?path=' + encodeURIComponent(file.path), { method: 'DELETE' });
      toast('已从云端删除模型 ' + entry.name);
    }));
    row.append(thumb, body, acts);
    modelList.append(row);
  }
  if (!modelList.children.length) modelList.append(el('li', 'empty', '还没有上传 Live2D 模型。桌宠里导入的模型会自动备份到这里。'));

  // 没有归属的文件：animations/ 下没被任何角色用到的
  const used = referencedNames();
  const loose = manifest.filter(f => f.path.startsWith('animations/') && !used.has(f.path.slice('animations/'.length)));
  $('loose').hidden = !loose.length;
  $('loose-count').textContent = loose.length ? loose.length + ' 个，' + fmtSize(loose.reduce((n, f) => n + f.size, 0)) : '';
  const looseList = $('loose-list');
  looseList.replaceChildren();
  for (const file of loose) {
    const name = file.path.slice('animations/'.length);
    const row = el('li', 'file');
    const body = el('div');
    body.append(el('div', 'name', name), el('div', 'sub', (AUDIO_EXT.test(name) ? '声音，' : '图片，') + fmtSize(file.size)));
    const acts = el('div', 'acts');
    if (AUDIO_EXT.test(name)) acts.append(action('试听', button => playFile(file, button)));
    acts.append(action('下载', () => downloadFile(file)), confirmButton('删除', '确认删除', async () => {
      await api('/api/sync/file?path=' + encodeURIComponent(file.path), { method: 'DELETE' });
      toast('已删除 ' + name);
    }));
    row.append(previewBox('thumb', file, (extOf(name).slice(1) || '?').toUpperCase().slice(0, 4)), body, acts);
    looseList.append(row);
  }
}

function renderCharacter(c) {
  const nameInput = $('char-name');
  if (document.activeElement !== nameInput) nameInput.value = c.name || '';
  $('char-active').hidden = config.activeCharacterId !== c.id;
  const list = $('char-states');
  list.replaceChildren();
  for (const [state, label, lamp, when] of STATES) {
    const item = el('li', 'state');
    const head = el('div', 'state-head');
    const dot = el('span', 'lamp ' + lamp, lamp === 'lamp-alert' ? '!' : undefined);
    dot.setAttribute('aria-hidden', 'true');
    head.append(dot, el('b', '', label), el('span', 'muted', when));

    const anim = fileOf(c[state]);
    const animSlot = el('div', 'slot');
    const animBody = el('div', 'slot-body');
    animBody.append(el('div', 'slot-title', '动画'),
      el('div', 'sub', anim ? fmtSize(anim.size) : c[state] ? '文件还没有传到云端' : '还没有动画'));
    const animActs = el('div', 'row');
    animActs.append(filePicker(c[state] ? '更换动画' : '选择动画', '.gif,.png,.jpg,.jpeg', file => setAnimation(c.id, state, file), c[state] ? '' : 'btn-ink'));
    if (anim) animActs.append(action('下载', () => downloadFile(anim)));
    if (c[state]) animActs.append(confirmButton('移除', '确认移除', () => clearAnimation(c.id, state)));
    animBody.append(animActs);
    animSlot.append(previewBox('state-preview', anim, '空'), animBody);

    const soundName = soundOf(c.id, state);
    const sound = fileOf(soundName);
    const soundSlot = el('div', 'slot');
    const soundBody = el('div', 'slot-body');
    soundBody.append(el('div', 'slot-title', '声音'),
      el('div', 'sub', sound ? fmtSize(sound.size) + '，切到这个状态时播放' : soundName ? '文件还没有传到云端' : '还没有声音'));
    const soundActs = el('div', 'row');
    if (sound) soundActs.append(action('试听', button => playFile(sound, button)));
    soundActs.append(filePicker(soundName ? '更换声音' : '选择声音', '.wav,.mp3,.ogg,.m4a,.flac', file => setSound(c.id, state, file)));
    if (soundName) soundActs.append(confirmButton('移除', '确认移除', () => clearSound(c.id, state)));
    soundBody.append(soundActs);
    soundSlot.append(el('div', 'state-preview sound' + (sound ? ' on' : ''), '♪'), soundBody);

    const body = el('div', 'state-body');
    body.append(animSlot, soundSlot);
    item.append(head, body);
    list.append(item);
  }
}

// ---------------------------------------------------------------- 事件
$('auth-form').addEventListener('submit', async e => {
  e.preventDefault();
  const act = e.submitter && e.submitter.value === 'register' ? 'register' : 'login';
  const username = $('username').value.trim();
  const password = $('password').value;
  if (!/^[A-Za-z0-9_-]{3,32}$/.test(username)) { toast('用户名要 3–32 位，只能用字母、数字、下划线和连字符。', true); $('username').focus(); return; }
  if (password.length < 6) { toast('密码至少 6 位。', true); $('password').focus(); return; }
  const buttons = $('auth-form').querySelectorAll('button');
  buttons.forEach(b => { b.disabled = true; });
  try {
    const res = await (await api('/api/auth/' + act, { method: 'POST', json: { username, password } })).json();
    saveToken(res.token);
    $('password').value = '';
    await refresh();
    toast(act === 'register' ? '账号建好了，已登录。' : '已登录。');
  } catch (err) { toast(err.message, true); }
  finally { buttons.forEach(b => { b.disabled = false; }); }
});
$('logout').addEventListener('click', async () => {
  if (busy) return;
  try { await api('/api/auth/logout', { method: 'POST' }); } catch (_) { /* 令牌已失效也照样退出 */ }
  saveToken('');
  showSpace(false);
  toast('已退出登录。');
});
$('folder').addEventListener('change', e => {
  const files = Array.from(e.target.files);
  e.target.value = '';
  if (!files.length) return;
  run(async () => {
    let done = 0;
    for (const file of files) {
      const path = 'live2d/' + file.webkitRelativePath;
      const existing = manifest.find(f => f.path === path);
      if (existing) continue;  // 已有的文件不重复传
      await upload(path, file);
      done++;
    }
    toast(done ? '已上传 ' + done + ' 个文件，桌宠会在一分钟内同步到本机。' : '这个模型云端已经有了。');
  });
});
$('char-back').addEventListener('click', () => { openCharId = ''; render(); });
$('char-rename').addEventListener('click', () => { const id = openCharId; const name = $('char-name').value; run(() => renameCharacter(id, name)); });
$('char-name').addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); $('char-rename').click(); } });
{
  // 删除角色：两步确认（按钮在 HTML 里，这里接上同一套行为）
  const button = $('char-delete');
  let armed = 0;
  button.addEventListener('click', () => {
    if (busy) return;
    if (!armed) {
      button.textContent = '确认删除这个角色';
      button.classList.add('btn-danger');
      armed = setTimeout(() => { armed = 0; button.textContent = '删除角色'; button.classList.remove('btn-danger'); }, 3000);
      return;
    }
    clearTimeout(armed);
    armed = 0;
    button.textContent = '删除角色';
    button.classList.remove('btn-danger');
    const id = openCharId;
    run(() => deleteCharacter(id));
  });
}

loadReleases();
if (token) refresh().catch(e => toast(e.message, true));
