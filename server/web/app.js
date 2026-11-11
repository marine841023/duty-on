'use strict';
const $ = id => document.getElementById(id);
let token = sessionStorage.getItem('dutyon-token') || '';
let manifest = [];
let busy = false;
const size = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MiB' : (n / 1024).toFixed(1) + ' KiB';
function status(message, error = false) { $('status').textContent = message; $('status').hidden = !message; $('status').classList.toggle('error', error); }
async function api(path, method = 'GET', body, raw = false, conditions = {}) {
  const headers = {...conditions, ...(token ? {Authorization: 'Bearer ' + token} : {})};
  if (body !== undefined) headers['Content-Type'] = raw ? 'application/octet-stream' : 'application/json';
  const res = await fetch(path, {method, headers, body: body === undefined ? undefined : raw ? body : JSON.stringify(body), cache: 'no-store', credentials: 'omit'});
  if (!res.ok) {
    let message = '请求失败 (' + res.status + ')';
    try { const data = await res.json(); if (typeof data.error === 'string') message = data.error; } catch (_) {}
    if (res.status === 401 && token) { token = ''; sessionStorage.removeItem('dutyon-token'); showAccount(false); }
    throw new Error(message);
  }
  return res;
}
function showAccount(logged) { $('auth-panel').hidden = logged; $('space-panel').hidden = !logged; if (!logged) { manifest = []; $('file-list').replaceChildren(); } }
function button(text, action) { const el = document.createElement('button'); el.textContent = text; el.className = 'secondary'; el.onclick = () => action().catch(e => status(e.message, true)); return el; }
async function refresh() {
  const [p, m] = await Promise.all([api('/api/profile').then(r => r.json()), api('/api/sync/manifest').then(r => r.json())]);
  $('greeting').textContent = p.username + ' 的云空间'; $('usage').textContent = '已使用 ' + size(p.usedBytes) + ' / ' + size(p.quotaBytes);
  $('quota').max = p.quotaBytes; $('quota').value = p.usedBytes;
  manifest = m; showAccount(true); render();
}
function render() {
  $('file-list').replaceChildren();
  const query = $('search').value.trim().toLowerCase();
  for (const file of manifest.filter(f => f.path.toLowerCase().includes(query))) {
    const row = document.createElement('div'); row.className = 'file-row';
    const name = document.createElement('div'); name.className = 'file-name'; name.textContent = file.path;
    const meta = document.createElement('small'); meta.textContent = size(file.size); name.append(meta);
    const actions = document.createElement('div'); actions.className = 'file-actions';
    actions.append(button('下载', async () => {
      const blob = await (await api('/api/sync/file?path=' + encodeURIComponent(file.path))).blob();
      const url = URL.createObjectURL(blob); const a = document.createElement('a'); a.href = url; a.download = file.path.split('/').pop(); a.click(); setTimeout(() => URL.revokeObjectURL(url), 30000);
    }), button('删除', async () => {
      if (busy || !confirm('从云端删除 ' + file.path + '？本机副本仍会保留。')) return;
      busy = true;
      try { await api('/api/sync/file?path=' + encodeURIComponent(file.path), 'DELETE'); await refresh(); status('已从云端删除'); }
      finally { busy = false; }
    }));
    row.append(name, actions); $('file-list').append(row);
  }
  if (!$('file-list').children.length) { const p = document.createElement('p'); p.className = 'empty'; p.textContent = query ? '没有匹配的文件' : '还没有文件，上传第一份内容吧。'; $('file-list').append(p); }
}
async function upload(input, directory) {
  if (busy || !input.files.length) return;
  busy = true;
  const selected = Array.from(input.files);
  let completed = 0;
  try {
    if (selected.some(f => f.size > 50 * 1048576)) throw new Error('单文件不能超过 50 MiB，请缩小文件后重试。');
    for (const file of selected) {
      const path = (directory ? 'live2d/' : 'animations/') + (directory ? file.webkitRelativePath : file.name);
      const existing = manifest.find(f => f.path === path);
      if (existing && !confirm('云端已有 ' + path + '，是否覆盖？')) continue;
      status('正在上传 ' + (completed + 1) + '/' + selected.length + '：' + file.name);
      await api('/api/sync/file?path=' + encodeURIComponent(path), 'PUT', file, true,
        existing ? {'If-Match': '"' + existing.sha256 + '"'} : {'If-None-Match': '*'}); completed++;
    }
    await refresh(); status('已上传 ' + completed + ' 个文件。在桌宠中选择“从云端恢复”即可下载。');
  } catch (e) { status('已上传 ' + completed + ' 个文件。' + e.message, true); await refresh().catch(() => {}); }
  finally { busy = false; input.value = ''; }
}
$('auth-form').onsubmit = async e => {
  e.preventDefault(); const action = e.submitter?.value || 'login';
  const buttons = $('auth-form').querySelectorAll('button'); buttons.forEach(b => b.disabled = true);
  try { const res = await (await api('/api/auth/' + action, 'POST', {username: $('username').value.trim(), password: $('password').value})).json(); token = res.token; sessionStorage.setItem('dutyon-token', token); $('password').value = ''; await refresh(); status(action === 'register' ? '账号已创建' : '登录成功'); }
  catch (e) { status(e.message, true); } finally { buttons.forEach(b => b.disabled = false); }
};
$('logout').onclick = async () => { if (busy) return; try { await api('/api/auth/logout', 'POST'); token = ''; sessionStorage.removeItem('dutyon-token'); showAccount(false); status('已退出登录'); } catch (e) { status(e.message, true); } };
$('files').onchange = e => upload(e.target, false); $('folder').onchange = e => upload(e.target, true);
$('refresh').onclick = () => refresh().catch(e => status(e.message, true)); $('search').oninput = render;
(async () => {
  try {
    const releases = await (await api('/api/app/releases')).json(); $('releases').replaceChildren();
    for (const [kind, label] of [['installer', '下载安装程序'], ['bundle', '下载安装包 ZIP'], ['update', '下载更新包']]) {
      const release = releases[kind]; if (!release) continue;
      const link = document.createElement('a'); link.className = 'download'; link.href = release.downloadUrl; link.download = '';
      const title = document.createElement('strong'); title.textContent = label; const meta = document.createElement('span'); meta.textContent = 'v' + release.version + ' · ' + size(release.size); link.append(title, meta); $('releases').append(link);
    }
    if (!$('releases').children.length) $('releases').textContent = '安装包尚未发布，请稍后再来。';
    const current = releases.installer || releases.update; if (current) $('release-note').textContent = current.notes;
  } catch (e) { $('releases').textContent = '暂时无法获取下载，请刷新页面重试。'; }
  if (token) await refresh().catch(e => status(e.message, true));
})();
