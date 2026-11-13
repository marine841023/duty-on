// 逐帧统计"轮廓边缘残留的近白像素"：贴近透明区域、又不属于大片白色（纸张等）
// 的近白像素数。用来找出哪些帧没抠干净，以及残留在画面的什么位置。
// 用法：node leftover.js <gif> [阈值=20]   列出残留数 >= 阈值 的帧及其残留块
'use strict';
const { readGif } = require('./gifio.js');
const [,, gifPath, TH = 20] = process.argv;
const g = readGif(gifPath);
const NEAR = 14, BIG = 700;
const perFrame = [];
g.frames.forEach((f, fi) => {
  const W = f.w, H = f.h, n = W * H, pal = f.lct || g.gct;
  const trans = new Uint8Array(n), core = new Uint8Array(n), light = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    const c = f.indices[i];
    if (c === f.trans) { trans[i] = 1; continue; }
    const r = pal[c * 3], gg = pal[c * 3 + 1], b = pal[c * 3 + 2];
    const mn = Math.min(r, gg, b), mx = Math.max(r, gg, b);
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
  const lab = new Uint8Array(n), blobs = [];
  let total = 0;
  for (let s = 0; s < n; s++) {
    if (!light[s] || lab[s]) continue;
    const st = [s]; lab[s] = 1;
    let area = 0, nCore = 0, minD = 60000, sx = 0, sy = 0;
    while (st.length) {
      const i = st.pop(); area++; nCore += core[i]; if (dist[i] < minD) minD = dist[i];
      const x = i % W, y = (i / W) | 0; sx += x; sy += y;
      for (const j of [i - 1, i + 1, i - W, i + W]) {
        if (j < 0 || j >= n || lab[j] || !light[j]) continue;
        if ((j === i - 1 && x === 0) || (j === i + 1 && x === W - 1)) continue;
        lab[j] = 1; st.push(j);
      }
    }
    if (area <= BIG && minD <= NEAR && nCore >= 6) {
      total += nCore;
      blobs.push(`(${Math.round(sx / area) + f.x},${Math.round(sy / area) + f.y}) core ${nCore} area ${area} d ${minD}`);
    }
  }
  perFrame.push({ fi, total, blobs });
});
console.log('per-frame leftover: ' + perFrame.map(p => p.total).join(','));
for (const p of perFrame) if (p.total >= +TH) console.log(`frame ${p.fi}: ${p.total}  ` + p.blobs.join(' | '));
