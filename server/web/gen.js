'use strict';
// 角色动画与声音生成：页面原型。
// 后台还没有接入，这里的进度和结果都是演示数据，不会修改角色、不会上传文件。
// 方案见 docs/generation-design.md。依赖 app.js 里的 $、el、toast、characters、
// fileOf、previewBox、render、STATES、genCharId 等。

const GEN_POOL = ['nico', 'nietzsche', 'nito', 'ni-j', 'nipsilon'];
const GEN_EMOTIONS = [['lazy', '慵懒'], ['cheerful', '元气'], ['anxious', '着急'], ['gentle', '温柔'], ['calm', '平静']];
const GEN_STRENGTH = ['轻', '中', '强'];
const GEN_STAGES = ['排队中', '生成视频', '去背景', '首尾衔接', '压缩成 GIF', 'AI 检查'];
// 演示用的 AI 质检结论：每个状态的 3 个备选各一条
const GEN_QA = [
  { score: 92, notes: [['ok', '形象和参考图一致'], ['ok', '循环处看不出接缝']] },
  { score: 78, notes: [['ok', '形象一致'], ['warn', '第 3 秒手部有残影']] },
  { score: 85, notes: [['warn', '循环处轻微跳一下'], ['ok', '抠图干净，发丝没有白边']] },
];
const GEN_TEXT = {
  sleeping: {
    descs: [
      '趴在桌上打盹，脸埋在手臂里，呼吸时肩膀轻轻起伏，偶尔动一下耳朵。眼睛一直闭着，动作很慢。',
      '靠在椅背上睡着，头一点一点往下垂，快要碰到桌面时又轻轻抬起来，循环往复。',
      '抱着枕头侧趴在桌上，嘴角动了一下像在说梦话，头顶飘着小小的 Z。',
    ],
    lines: ['没事啦，我先眯一会儿。', '都忙完了吗……那我睡一下下。', '有事再叫我哦，呼……'],
    emotion: 'lazy', strength: 1,
  },
  working: {
    descs: [
      '坐直在桌前快速写字，写完一行翻一下纸，偶尔点点头，表情专注。',
      '双手在键盘上飞快地敲，身体跟着节奏轻轻晃，屏幕的光映在脸上。',
      '一手拿笔一手翻文件，看到关键处眼睛一亮，用笔在纸上圈一下。',
    ],
    lines: ['开工！交给我吧。', '正在跑，别急，马上就好。', '嗯嗯，这一段我盯着呢。'],
    emotion: 'cheerful', strength: 2,
  },
  alert: {
    descs: [
      '突然抬头看向镜头，举起一只手用力挥动，表情有点着急，头顶冒出一个感叹号。',
      '站起来双手扶着桌子探身向前，眨眨眼睛，像在等你回答。',
      '拿起一张纸举到镜头前晃了晃，指着上面的内容，嘴巴在说"快看"。',
    ],
    lines: ['快来看看，这里需要你点头！', '等你确认哦，别让我等太久～', '这一步我不敢自己做主，你来看看？'],
    emotion: 'anxious', strength: 3,
  },
};

let gen = null;
const genTimers = new Set();

function genWait(ms) {
  return new Promise(resolve => {
    const id = setTimeout(() => { genTimers.delete(id); resolve(); }, ms);
    genTimers.add(id);
  });
}
function genChar() { return characters().find(c => c.id === genCharId); }
function genChip(id, text, tone) {
  const chip = $(id);
  chip.textContent = text || '';
  chip.className = 'gen-state' + (tone ? ' ' + tone : '');
}

function openGen(id) {
  for (const t of genTimers) clearTimeout(t);
  genTimers.clear();
  gen = {
    ref: '', refUploaded: false, traits: [], descs: null, describing: false, rewriting: '',
    cands: null, voice: null, cloning: false, cloned: false, lines: null,
  };
  genCharId = id;
  render();
  window.scrollTo({ top: $('space').offsetTop, behavior: reducedMotion ? 'auto' : 'smooth' });
}
function closeGen() {
  for (const t of genTimers) clearTimeout(t);
  genTimers.clear();
  if (gen && gen.refUploaded) URL.revokeObjectURL(gen.ref);
  if (gen && gen.voice) URL.revokeObjectURL(gen.voice.url);
  gen = null;
  openCharId = genCharId;
  genCharId = '';
  render();
}

// app.js 的 render() 在生成页打开时调用这里
function renderGen(c) {
  if (!gen) openGen(c.id);
  $('gen-title').textContent = '重新生成：' + (c.name || '没有名字');
  renderGenImage(c);
  renderGenDescs();
  renderGenCands(c);
  renderGenVoice();
}

// ---- 1. 角色形象 ----
function renderGenImage(c) {
  const box = $('gen-ref');
  if (gen.refUploaded) {
    const img = el('img');
    img.alt = '上传的角色图';
    img.src = gen.ref;
    box.replaceChildren(img);
    genChip('gen-image-state', '用上传的图', 'ok');
  } else {
    const file = fileOf(c.working) || fileOf(c.sleeping) || fileOf(c.alert);
    const preview = previewBox('gen-ref-inner', file, '还没有图片，请上传一张');
    box.replaceChildren(preview);
    genChip('gen-image-state', file ? '用现在的动画' : '需要一张图', file ? 'ok' : 'warn');
  }
  const traits = $('gen-traits');
  traits.replaceChildren();
  for (const t of gen.traits) traits.append(el('li', '', t));
}

// ---- 2. 动作描述 ----
function renderGenDescs() {
  const list = $('gen-descs');
  $('gen-describe').disabled = gen.describing;
  $('gen-describe').textContent = gen.describing ? '正在看图…' : gen.descs ? '全部重新写' : '让 AI 看图写描述';
  if (gen.describing) genChip('gen-describe-state', '正在看图', 'busy');
  else if (gen.descs) genChip('gen-describe-state', '已写好，可以改', 'ok');
  else genChip('gen-describe-state', '');
  list.replaceChildren();
  if (!gen.descs) return;
  for (const [state, label, lamp] of STATES) {
    const item = el('li', 'gen-desc');
    const head = el('div', 'gen-desc-head');
    const dot = el('span', 'lamp ' + lamp, lamp === 'lamp-alert' ? '!' : undefined);
    dot.setAttribute('aria-hidden', 'true');
    const redo = el('button', 'btn btn-quiet', gen.rewriting === state ? '正在重写…' : '重写这一段');
    redo.type = 'button';
    redo.disabled = !!gen.rewriting;
    redo.addEventListener('click', () => rewriteDesc(state));
    head.append(dot, el('b', '', label), redo);
    const text = el('textarea');
    text.rows = 3;
    text.maxLength = 300;
    text.value = gen.descs[state];
    text.setAttribute('aria-label', label + '的动作描述');
    text.addEventListener('input', () => { gen.descs[state] = text.value; });
    item.append(head, text);
    list.append(item);
  }
}
async function describeImage() {
  const c = genChar();
  if (!c) return;
  if (!gen.refUploaded && !(fileOf(c.working) || fileOf(c.sleeping) || fileOf(c.alert))) {
    toast('先上传一张角色图。', true);
    return;
  }
  const g = gen;
  gen.describing = true;
  renderGenDescs();
  await genWait(1600);
  if (gen !== g) return;
  gen.describing = false;
  gen.traits = ['画风：二次元 Q 版，赛璐璐上色', '构图：全身入画，桌椅等道具也在画面里', '会保留：发型、服装、配色、道具位置'];
  gen.descs = { sleeping: GEN_TEXT.sleeping.descs[0], working: GEN_TEXT.working.descs[0], alert: GEN_TEXT.alert.descs[0] };
  gen.descIdx = { sleeping: 0, working: 0, alert: 0 };
  renderGenImage(c);
  renderGenDescs();
}
async function rewriteDesc(state) {
  const g = gen;
  gen.rewriting = state;
  renderGenDescs();
  await genWait(1000);
  if (gen !== g) return;
  gen.descIdx[state] = (gen.descIdx[state] + 1) % GEN_TEXT[state].descs.length;
  gen.descs[state] = GEN_TEXT[state].descs[gen.descIdx[state]];
  gen.rewriting = '';
  renderGenDescs();
}

// ---- 3. 生成动画 ----
function renderGenCands(c) {
  const box = $('gen-cands');
  const running = gen.cands && STATES.some(([state]) => gen.cands[state].some(x => x.stage < GEN_STAGES.length));
  $('gen-animate').disabled = running;
  $('gen-animate').textContent = running ? '正在生成…' : gen.cands ? '全部重新生成' : '生成全部动画';
  if (running) genChip('gen-animate-state', '生成中', 'busy');
  else if (gen.cands) genChip('gen-animate-state', '已完成，挑一个', 'ok');
  else genChip('gen-animate-state', '');
  box.replaceChildren();
  if (!gen.cands) return;
  for (const [state, label, lamp] of STATES) {
    const group = el('section', 'gen-group');
    const head = el('div', 'gen-group-head');
    const dot = el('span', 'lamp ' + lamp, lamp === 'lamp-alert' ? '!' : undefined);
    dot.setAttribute('aria-hidden', 'true');
    const current = el('div', 'gen-current');
    current.append(previewBox('gen-current-thumb', fileOf(c[state]), '无'), el('span', 'sub', '当前主动画'));
    head.append(dot, el('b', '', label), current);
    const grid = el('ol', 'gen-grid');
    gen.cands[state].forEach((cand, i) => grid.append(renderCand(state, cand, i)));
    group.append(head, grid);
    box.append(group);
  }
}
function renderCand(state, cand, i) {
  const card = el('li', 'gen-cand' + (cand.picked ? ' picked' : ''));
  const done = cand.stage >= GEN_STAGES.length;
  const view = el('div', 'gen-cand-view');
  if (done) {
    const img = el('img');
    img.alt = '备选 ' + (i + 1);
    img.src = '/img/' + cand.art + '.png';
    view.append(img);
    if (cand.picked) view.append(el('span', 'gen-picked', '主动画'));
  } else {
    view.append(el('span', 'gen-stage', GEN_STAGES[cand.stage]));
  }
  const bar = el('div', 'gen-bar');
  const fill = el('span');
  fill.style.width = Math.round(cand.stage / GEN_STAGES.length * 100) + '%';
  if (done) fill.className = 'done';
  bar.append(fill);
  const body = el('div', 'gen-cand-body');
  if (done) {
    const qa = GEN_QA[i % GEN_QA.length];
    const score = el('div', 'gen-score', '备选 ' + (i + 1));
    score.append(el('b', qa.score >= 85 ? 'good' : 'fair', qa.score + ' 分'));
    const notes = el('ul', 'gen-notes');
    for (const [tone, text] of qa.notes) notes.append(el('li', tone, text));
    const acts = el('div', 'row');
    const pick = el('button', 'btn' + (cand.picked ? '' : ' btn-ink'), cand.picked ? '已设为主动画' : '设为主动画');
    pick.type = 'button';
    pick.disabled = cand.picked;
    pick.addEventListener('click', () => pickCand(state, i));
    const redo = el('button', 'btn', '重做');
    redo.type = 'button';
    redo.addEventListener('click', () => runCand(state, i, true));
    acts.append(pick, redo);
    body.append(score, notes, acts);
  } else {
    body.append(el('div', 'gen-score', '备选 ' + (i + 1)), el('p', 'sub', '第 ' + (cand.stage + 1) + ' 步，共 ' + GEN_STAGES.length + ' 步'));
  }
  card.append(view, bar, body);
  return card;
}
async function runCand(state, i, redo) {
  const g = gen;
  const cand = gen.cands[state][i];
  cand.stage = 0;
  cand.picked = false;
  if (redo) cand.art = GEN_POOL[(GEN_POOL.indexOf(cand.art) + 2) % GEN_POOL.length];
  renderGenCands(genChar());
  await genWait(300 + i * 500);
  for (let s = 1; s <= GEN_STAGES.length; s++) {
    await genWait(500 + (s === 1 ? 900 : 0) + Math.round(Math.random() * 300));
    if (gen !== g) return;
    cand.stage = s;
    renderGenCands(genChar());
  }
}
function generateAll() {
  if (!gen.descs) { toast('先让 AI 写好三段动作描述。', true); return; }
  gen.cands = {};
  STATES.forEach(([state], si) => {
    gen.cands[state] = [0, 1, 2].map(i => ({ stage: 0, picked: false, art: GEN_POOL[(si + i) % GEN_POOL.length] }));
  });
  for (const [state] of STATES) for (let i = 0; i < 3; i++) runCand(state, i, false);
}
function pickCand(state, i) {
  gen.cands[state].forEach((x, k) => { x.picked = k === i; });
  renderGenCands(genChar());
  toast('预览版不会改动角色。接入后，这一段会替换成"' + STATES.find(s => s[0] === state)[1] + '"的主动画，原来的留在备选里。');
}

// ---- 4. 声音 ----
function renderGenVoice() {
  const file = $('gen-voice-file');
  if (gen.voice) {
    file.replaceChildren(el('b', '', gen.voice.name),
      el('span', gen.voice.duration && gen.voice.duration < 10 ? 'warn' : 'sub',
 gen.voice.duration ? gen.voice.duration.toFixed(1) + ' 秒' + (gen.voice.duration < 10 ? '，太短了，建议 10 秒以上' : '') : '正在读取时长…'));
  } else {
    file.replaceChildren(el('span', 'sub', '还没有录音'));
  }
  const clone = $('gen-clone');
  clone.disabled = gen.cloning;
  clone.textContent = gen.cloning ? '正在' + gen.cloneStage + '…' : gen.cloned ? '重新克隆' : '克隆声音';
  if (gen.cloning) genChip('gen-voice-state', '克隆中', 'busy');
  else if (gen.cloned) genChip('gen-voice-state', '声音已克隆', 'ok');
  else genChip('gen-voice-state', '');
  const list = $('gen-lines');
  list.replaceChildren();
  if (!gen.cloned) return;
  for (const [state, label, lamp] of STATES) list.append(renderLine(state, label, lamp));
}
function renderLine(state, label, lamp) {
  const line = gen.lines[state];
  const item = el('li', 'gen-line');
  const head = el('div', 'gen-desc-head');
  const dot = el('span', 'lamp ' + lamp, lamp === 'lamp-alert' ? '!' : undefined);
  dot.setAttribute('aria-hidden', 'true');
  const idea = el('button', 'btn btn-quiet', 'AI 帮我写一句');
  idea.type = 'button';
  idea.addEventListener('click', () => {
    line.idea = (line.idea + 1) % GEN_TEXT[state].lines.length;
    line.text = GEN_TEXT[state].lines[line.idea];
    renderGenVoice();
  });
  head.append(dot, el('b', '', label), idea);

  const text = el('input');
  text.maxLength = 40;
  text.value = line.text;
  text.placeholder = '想让她说的话，40 字以内';
  text.setAttribute('aria-label', label + '的台词');
  text.addEventListener('input', () => { line.text = text.value; });

  const emotions = el('div', 'gen-emotions');
  emotions.setAttribute('role', 'radiogroup');
  emotions.setAttribute('aria-label', '情绪');
  for (const [key, name] of GEN_EMOTIONS) {
    const chip = el('button', 'gen-emo', name);
    chip.type = 'button';
    chip.setAttribute('role', 'radio');
    chip.setAttribute('aria-checked', String(line.emotion === key));
    chip.addEventListener('click', () => { line.emotion = key; renderGenVoice(); });
    emotions.append(chip);
  }
  const strength = el('label', 'gen-strength', '强度');
  const range = el('input');
  range.type = 'range';
  range.min = '1';
  range.max = '3';
  range.step = '1';
  range.value = String(line.strength);
  const strengthText = el('span', '', GEN_STRENGTH[line.strength - 1]);
  range.addEventListener('input', () => { line.strength = Number(range.value); strengthText.textContent = GEN_STRENGTH[line.strength - 1]; });
  strength.append(range, strengthText);

  const speak = el('button', 'btn btn-ink', line.speaking ? '正在生成…' : line.takes.length ? '再生成两版' : '生成');
  speak.type = 'button';
  speak.disabled = line.speaking;
  speak.addEventListener('click', () => speakLine(state));
  const controls = el('div', 'gen-line-controls');
  controls.append(emotions, strength, speak);

  const takes = el('ol', 'gen-takes');
  line.takes.forEach((take, k) => {
    const row = el('li', 'gen-take' + (line.pick === k ? ' picked' : ''));
    const emo = GEN_EMOTIONS.find(e => e[0] === take.emotion)[1];
    row.append(el('span', 'gen-take-name', '版本 ' + (k + 1)),
      el('span', 'sub', take.seconds.toFixed(1) + ' 秒，' + emo + '（' + GEN_STRENGTH[take.strength - 1] + '）'));
    const play = el('button', 'btn', '试听');
    play.type = 'button';
    play.addEventListener('click', () => {
      // 原型没有合成服务：播放上传的原声，并说明
      const audio = new Audio(gen.voice.url);
      audio.play().catch(() => undefined);
      setTimeout(() => audio.pause(), 3000);
      toast('演示：现在播放的是你上传的原声。接入后会是克隆声音说的这句话。');
    });
    const use = el('button', 'btn' + (line.pick === k ? '' : ' btn-ink'), line.pick === k ? '已选用' : '用这个');
    use.type = 'button';
    use.disabled = line.pick === k;
    use.addEventListener('click', () => {
      line.pick = k;
      renderGenVoice();
      toast('预览版不会改动角色。接入后，这句话会设为"' + label + '"的声音。');
    });
    const acts = el('div', 'row');
    acts.append(play, use);
    row.append(acts);
    takes.append(row);
  });
  item.append(head, text, controls, takes);
  return item;
}
async function cloneVoice() {
  if (!gen.voice) { toast('先上传一段录音。', true); return; }
  if (!$('gen-consent').checked) { toast('需要先确认这是你的声音，或者已经得到声音主人的同意。', true); return; }
  if (gen.voice.duration && gen.voice.duration < 10) { toast('录音太短，建议 10 秒以上，克隆出来才像。', true); return; }
  const g = gen;
  gen.cloning = true;
  for (const stage of ['降噪', '切掉静音', '提取音色']) {
    gen.cloneStage = stage;
    renderGenVoice();
    await genWait(900);
    if (gen !== g) return;
  }
  gen.cloning = false;
  gen.cloned = true;
  toast('声音克隆好了。给三种状态各写一句台词，选好情绪再生成。');
  if (!gen.lines) {
    gen.lines = Object.create(null);
    for (const [state] of STATES) {
      const t = GEN_TEXT[state];
      gen.lines[state] = { text: t.lines[0], idea: 0, emotion: t.emotion, strength: t.strength, takes: [], pick: -1, speaking: false };
    }
  }
  renderGenVoice();
}
async function speakLine(state) {
  const line = gen.lines[state];
  if (!line.text.trim()) { toast('先写一句台词。', true); return; }
  const g = gen;
  line.speaking = true;
  renderGenVoice();
  await genWait(1400);
  if (gen !== g) return;
  line.speaking = false;
  const base = Math.min(3, 0.9 + line.text.trim().length * 0.17);
  line.takes = [0, 1].map(k => ({ seconds: base + k * 0.2, emotion: line.emotion, strength: line.strength }));
  line.pick = -1;
  renderGenVoice();
}

// ---- 事件 ----
$('gen-back').addEventListener('click', closeGen);
$('gen-image-file').addEventListener('change', e => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file) return;
  if (!/\.(png|jpe?g|webp|gif)$/i.test(file.name)) { toast('角色图只支持 PNG、JPG、WebP、GIF。', true); return; }
  if (gen.refUploaded) URL.revokeObjectURL(gen.ref);
  gen.ref = URL.createObjectURL(file);
  gen.refUploaded = true;
  renderGenImage(genChar());
});
$('gen-describe').addEventListener('click', describeImage);
$('gen-animate').addEventListener('click', generateAll);
$('gen-voice-upload').addEventListener('change', e => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file) return;
  if (gen.voice) URL.revokeObjectURL(gen.voice.url);
  gen.voice = { name: file.name, url: URL.createObjectURL(file), duration: 0 };
  gen.cloned = false;
  renderGenVoice();
  const probe = new Audio();
  probe.preload = 'metadata';
  probe.onloadedmetadata = () => { if (gen && gen.voice) { gen.voice.duration = probe.duration; renderGenVoice(); } };
  probe.onerror = () => toast('这个文件读不出时长，可能不是浏览器支持的音频格式。', true);
  probe.src = gen.voice.url;
});
$('gen-clone').addEventListener('click', cloneVoice);
$('char-regen').addEventListener('click', () => { if (openCharId) openGen(openCharId); });
