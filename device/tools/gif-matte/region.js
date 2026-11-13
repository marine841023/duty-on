// 逐帧列出某个窗口内的浅色小块，以及它们在抠图结果里被抠掉了多少。
// 用来查"同一处白色有的帧抠了、有的帧没抠"。
// 用法：node region.js <原始.gif> <结果.gif> <x0> <y0> <x1> <y1>
'use strict';
const { readGif } = require('./gifio.js');
const [,, aPath, bPath, X0, Y0, X1, Y1] = process.argv;
const a = readGif(aPath), b = readGif(bPath);
const x0 = +X0, y0 = +Y0, x1 = +X1, y1 = +Y1;
const lines = [];
a.frames.forEach((f, k) => {
  const g = b.frames[k], W = f.w, H = f.h, n = W * H, pal = f.lct || a.gct;
  const trans = new Uint8Array(n), core = new Uint8Array(n), light = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    const c = f.indices[i];
    if (c === f.trans) { trans[i] = 1; continue; }
    const r = pal[c * 3], gg = pal[c * 3 + 1], bl = pal[c * 3 + 2];
    const mn = Math.min(r, gg, bl), mx = Math.max(r, gg, bl);
    if (mn >= 200 && mx - mn <= 24) core[i] = 1;
    if (mn >= 125 && mx - mn <= 34) light[i] = 1;
  }
  const dist = new Uint16Array(n);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x;
    dist[i] = trans[i] ? 0 : Math.min(60000, x > 0 ? dist[i - 1] + 1 : 1, y > 0 ? dist[i - W] + 1 : 1);
  }
  for (let y = H - 1; y >= 0; y--) for (let x = W - 1; x >= 0; x--) {
    const i = y * W + x;
    if (!trans[i]) dist[i] = Math.min(dist[i], x < W - 1 ? dist[i + 1] + 1 : 1, y < H - 1 ? dist[i + W] + 1 : 1);
  }
  const lab = new Uint8Array(n), parts = [];
  let kept = 0, gone = 0;
  for (let s = 0; s < n; s++) {
    if (!light[s] || lab[s]) continue;
    const st = [s]; lab[s] = 1;
    let area = 0, nCore = 0, minD = 60000, sx = 0, sy = 0, rm = 0, inWin = 0;
    while (st.length) {
      const i = st.pop(); area++; nCore += core[i]; if (dist[i] < minD) minD = dist[i];
      const x = i % W, y = (i / W) | 0; sx += x; sy += y;
      if (g.indices[i] === g.trans) rm++;
      const cx = x + f.x, cy = y + f.y;
      if (cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1) inWin++;
      for (const j of [i - 1, i + 1, i - W, i + W]) {
 if (j < 0 || j >= n || lab[j] || !light[j]) continue;
 if ((j === i - 1 && x === 0) || (j === i + 1 && x === W - 1)) continue;
        lab[j] = 1; st.push(j);
      }
    }
    if (inWin < area * 0.5 || nCore < 4) continue;
    const tag = rm > area * 0.5 ? 'X' : '-';
    if (tag === 'X') gone += nCore; else kept += nCore;
    parts.push(`${tag}(${Math.round(sx / area) + f.x},${Math.round(sy / area) + f.y} a${area} c${nCore} d${minD})`);
  }
  lines.push({ k, kept, gone, parts });
});
console.log('kept white per frame: ' + lines.map(l => l.kept).join(','));
console.log('removed white per frame: ' + lines.map(l => l.gone).join(','));
for (const l of lines) if (l.k % 6 === 0) console.log(`f${l.k}: ` + l.parts.join(' '));
