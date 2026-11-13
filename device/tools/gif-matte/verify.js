// 比较原始动图与抠图结果：逐帧核对只有"变透明"的改动，并找出抠得较深的位置
// （被抠像素离原始透明区域超过 DEEP 像素），用来排查误伤画面内部的白色。
// 用法：node verify.js <原始.gif> <结果.gif> [DEEP=20]
'use strict';
const { readGif } = require('./gifio.js');
const [,, aPath, bPath, DEEP = 20] = process.argv;
const a = readGif(aPath), b = readGif(bPath);
if (a.frames.length !== b.frames.length) throw new Error('frame count differs');
let changed = 0, notTrans = 0, geom = 0, worst = 0;
const deep = Object.create(null);
const srcFrames = a.frames;
srcFrames.forEach((f, k) => {
  const g = b.frames[k], W = f.w, H = f.h, N = W * H;
  if (f.w !== g.w || f.h !== g.h || f.x !== g.x || f.y !== g.y || f.delay !== g.delay || f.disposal !== g.disposal) geom++;
  const d = new Uint16Array(N), tr = i => f.indices[i] === f.trans;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x;
    d[i] = tr(i) ? 0 : Math.min(60000, x > 0 ? d[i - 1] + 1 : 1, y > 0 ? d[i - W] + 1 : 1);
  }
  for (let y = H - 1; y >= 0; y--) for (let x = W - 1; x >= 0; x--) {
    const i = y * W + x;
    if (!tr(i)) d[i] = Math.min(d[i], x < W - 1 ? d[i + 1] + 1 : 1, y < H - 1 ? d[i + W] + 1 : 1);
  }
  let c = 0, sx = 0, sy = 0;
  for (let i = 0; i < N; i++) {
    if (f.indices[i] === g.indices[i]) continue;
    changed++;
    if (g.indices[i] !== g.trans) notTrans++;
    if (d[i] > worst) worst = d[i];
    if (d[i] > +DEEP) { c++; sx += i % W + f.x; sy += (i / W | 0) + f.y; }
  }
  if (c) deep[k] = `${c}px@${Math.round(sx / c)},${Math.round(sy / c)}`;
});
console.log(`frames ${a.frames.length}, geometry/timing mismatches ${geom}, changed ${changed}, changed-but-not-transparent ${notTrans}, deepest ${worst}px`);
console.log('deep spots by frame: ' + JSON.stringify(deep));
