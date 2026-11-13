// 抠掉动图里没抠干净的白底：发丝之间被头发围住的白块，以及轮廓外沿的白边。
// 只把判定为残留背景的像素改成透明色索引，其余像素、调色板、帧时序原样保留。
//
// 用法：node matte.js <in.gif> <out.gif> [--mark <帧号> <out.bmp>]
//   --mark：不写 GIF，只把指定帧导出成 BMP，将被抠掉的像素标成品红，用于检查。
//
// 判定（每帧独立做）：
//   1. 白块：浅色中性像素（白底本身，加上它与黑发之间的灰色过渡）连成的小区域，
//      里面确实有近白像素，边界几乎全是深色发丝或透明，并且离透明区域很近
//      （在轮廓边缘），就是发丝间漏出来的背景。纸张连同灰色线条是一大片，超过
//      面积上限；茶杯边界是木色；袖子、衬衣离轮廓远——都不满足，会被保留。
//      距离只按原图的透明区域算，不随抠掉的白块向内蔓延——否则会顺着一路
//      吃进手环的高光、袖口这些画面内部的白色。边界上的"发丝"还要求颜色偏
//      中性：贴着青色发束的白条是头发高光，不是背景。另外：
//      又细又长的线条、以灰色为主的亮边（如手环的金属高光）不算白块。
//   3. 帧间一致：同一个白块在某些帧里会贴上椅子等别的颜色，单看那一帧不满足
//      上面的条件，结果有的帧抠了、有的帧没抠，播放时一闪一闪。所以先全部帧
//      各自判一遍，再回头看每帧里剩下的小白块：如果它所在的位置在前后几帧里
//      大半都被抠掉了，这一帧也照样抠掉。
//   2. 白边：紧贴透明区域、旁边 2 像素内有深色发丝的浅灰白像素（白底与黑发的
//      过渡色），一并去掉，否则会留下一圈亮线。
'use strict';
const fs = require('fs');
const { readGif, writeGif } = require('./gifio.js');

const P = {
  whiteMin: 200,     // 近白（白块的"核"）：三通道最小值
  whiteSat: 24,      // 近白：最大最小通道差
  regionMin: 125,    // 白块区域（含过渡灰）：三通道最小值
  regionSat: 34,
  coreMin: 4,        // 区域内至少有这么多近白像素才算白块（发丝细缝里的小白点也要算）
  darkLum: 78,       // 深色（发丝/描边）亮度上限
  edgeLum: 125,      // 白块边界上算作"发丝一侧"的亮度上限（含偏暗的过渡色）
  borderDarkTrans: 0.85,  // 边界中 深色+透明 的最低占比
  borderDark: 0.30,       // 边界中 深色 的最低占比
  nearTrans: 8,      // 白块到原始透明区域的最大距离（像素）
  voteNear: 12,      // 帧间一致时放宽到这个距离
  bigNear: 3, // 离大片白色（纸张）这么近的小块可能是被发丝隔出来的纸面
  thinArea: 30,      // 面积达到这个数、却细得几乎没有"内部"的，是高光线条不是白块
  thinInterior: 0.15,
  coreFrac: 0.45,    // 白块里近白像素的最低占比（金属亮边之类以灰为主，白底残留以白为主）
  fracArea: 40,      // 上一条只对这么大以上的块生效：细缝里的小白块被过渡灰稀释，占比天然偏低
  edgeSat: 45,// 边界上算作"发丝"的颜色必须偏中性（青色、红色发束不算）
  voteSpan: 8,       // 帧间一致：参考前后各多少帧（动画首尾相接）
  voteOverlap: 0.5,  // 帧间一致：白块有这么大比例落在邻帧已抠掉的位置上就跟着抠
  maxArea: 700,      // 白块面积上限（纸张、袖子这类大片白色不动）
  fringeMin: 132,    // 白边：三通道最小值
  fringeSat: 34,
  fringeRounds: 2,
};

// vote：画布坐标的掩码（邻帧抠掉过的位置），第一遍为 null
function processFrame(gif, f, vote) {
  const W = f.w, H = f.h, n = W * H, pal = f.lct || gif.gct, T = f.trans;
  const idx = f.indices;
  const lum = new Uint8Array(n), light = new Uint8Array(n), fringe = new Uint8Array(n);
  const sat = new Uint8Array(n);
  const core = new Uint8Array(n);
  const trans = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    const c = idx[i];
    if (f.hasTrans && c === T) { trans[i] = 1; continue; }
    const r = pal[c * 3], g = pal[c * 3 + 1], b = pal[c * 3 + 2];
    const mn = Math.min(r, g, b), mx = Math.max(r, g, b);
    lum[i] = (r * 77 + g * 150 + b * 29) >> 8;
    sat[i] = mx - mn;
    if (mn >= P.whiteMin && mx - mn <= P.whiteSat) core[i] = 1;
    if (mn >= P.regionMin && mx - mn <= P.regionSat) light[i] = 1;
    if (mn >= P.fringeMin && mx - mn <= P.fringeSat) fringe[i] = 1;
  }
  const removed = new Uint8Array(n);
  const isTrans = (x, y) => x < 0 || y < 0 || x >= W || y >= H || trans[y * W + x] === 1;

  // 到透明区域的距离（两遍倒角距离，足够判断"近不近"）
  const dist = new Uint16Array(n);
  const computeDist = () => {
    const INF = 60000;
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const i = y * W + x;
      if (trans[i]) { dist[i] = 0; continue; }
      let d = INF;
      d = Math.min(d, x > 0 ? dist[i - 1] + 1 : 1, y > 0 ? dist[i - W] + 1 : 1);
      dist[i] = d;
    }
    for (let y = H - 1; y >= 0; y--) for (let x = W - 1; x >= 0; x--) {
      const i = y * W + x;
      if (trans[i]) continue;
      dist[i] = Math.min(dist[i], x < W - 1 ? dist[i + 1] + 1 : 1, y < H - 1 ? dist[i + W] + 1 : 1);
    }
  };

  // 1. 白块
  const label = new Int32Array(n);
  const stack = new Int32Array(n);
  computeDist();  // 只按原始透明区域算一次
  // 到"大片浅色区域"（纸张等，面积超过上限）的距离
  const distBig = new Uint16Array(n).fill(60000);
  {
    const seen = new Uint8Array(n);
    for (let s = 0; s < n; s++) {
      if (!light[s] || trans[s] || seen[s]) continue;
      const comp = [s]; seen[s] = 1;
      for (let q = 0; q < comp.length; q++) {
        const c = comp[q], x = c % W;
        if (x > 0 && light[c - 1] && !trans[c - 1] && !seen[c - 1]) { seen[c - 1] = 1; comp.push(c - 1); }
        if (x < W - 1 && light[c + 1] && !trans[c + 1] && !seen[c + 1]) { seen[c + 1] = 1; comp.push(c + 1); }
        if (c >= W && light[c - W] && !trans[c - W] && !seen[c - W]) { seen[c - W] = 1; comp.push(c - W); }
        if (c + W < n && light[c + W] && !trans[c + W] && !seen[c + W]) { seen[c + W] = 1; comp.push(c + W); }
      }
      if (comp.length > P.maxArea) for (const c of comp) distBig[c] = 0;
    }
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const c = y * W + x;
      if (x > 0 && distBig[c - 1] + 1 < distBig[c]) distBig[c] = distBig[c - 1] + 1;
      if (y > 0 && distBig[c - W] + 1 < distBig[c]) distBig[c] = distBig[c - W] + 1;
    }
    for (let y = H - 1; y >= 0; y--) for (let x = W - 1; x >= 0; x--) {
      const c = y * W + x;
      if (x < W - 1 && distBig[c + 1] + 1 < distBig[c]) distBig[c] = distBig[c + 1] + 1;
      if (y < H - 1 && distBig[c + W] + 1 < distBig[c]) distBig[c] = distBig[c + W] + 1;
    }
  }
  for (let round = 0; round < 1; round++) {
    label.fill(0);
    let changed = false, next = 0;
    for (let s = 0; s < n; s++) {
      if (!light[s] || trans[s] || label[s]) continue;
      next++;
      let nVote = 0;
      let minBig = 60000;
      let sp = 0, area = 0, nCore = 0, nDark = 0, nTrans = 0, nOther = 0, minD = 60000;
      const members = [];
      stack[sp++] = s; label[s] = next;
      while (sp) {
        const i = stack[--sp];
        members.push(i); area++; nCore += core[i];
        if (vote && vote[((i / W | 0) + f.y) * gif.width + (i % W) + f.x]) nVote++;
        if (dist[i] < minD) minD = dist[i];
 if (distBig[i] < minBig) minBig = distBig[i];
        const x = i % W, y = (i / W) | 0;
        for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
   if (!dx && !dy) continue;
   const nx = x + dx, ny = y + dy;
          if (isTrans(nx, ny)) { nTrans++; continue; }
          const j = ny * W + nx;
   if (light[j]) {
     if (!label[j] && (dx === 0 || dy === 0)) { label[j] = next; stack[sp++] = j; }
     continue;
          }
          if (lum[j] < P.edgeLum && sat[j] <= P.edgeSat) nDark++; else nOther++;
        }
      }
      const total = nDark + nTrans + nOther;
      if (!total || nCore < P.coreMin || area > P.maxArea) continue;
      // 细长线条：几乎没有"四邻都在块内"的像素
      const inSet = new Set(members);
      let interior = 0;
      for (const i of members) {
        if (inSet.has(i - 1) && inSet.has(i + 1) && inSet.has(i - W) && inSet.has(i + W)) interior++;
      }
      if (process.env.MATTE_DEBUG && !vote) console.log(JSON.stringify({ x: members[0] % W + f.x, y: (members[0] / W | 0) + f.y, area, nCore, interior, nDark, nTrans, nOther, minD }));
      // 这两条只约束"单看这一帧"的判定；邻帧已确认是白底的位置不受限（同一块白底在
      // 某些帧里稍大一点、被过渡灰稀释，占比会刚好掉到线下，结果那几帧漏抠、播放时闪）
      const lineLike = area >= P.thinArea && interior / area < P.thinInterior;
      const grayish = area >= P.fracArea && nCore / area < P.coreFrac;
      const byShape = !lineLike && !grayish && minD <= P.nearTrans &&
        (nDark + nTrans) / total >= P.borderDarkTrans && nDark / total >= P.borderDark;
      const byVote = minD <= P.voteNear && nVote / area >= P.voteOverlap;
      // 紧挨大片白色的小块：单看一帧分不清是纸面还是白底，只有邻帧在同一位置
      // 抠过（说明这里平时是发丝间的空隙，这一帧只是纸张恰好挡到旁边）才抠
      const nearBig = minBig <= P.bigNear;
      if ((byShape && !nearBig) || byVote) {
        for (const i of members) { trans[i] = 1; removed[i] = 1; }
        changed = true;
      }
    }
    if (!changed) break;
  }

  // 2. 白边：贴着透明、2 像素内有深色的浅色中性像素
  const hasDarkNear = (x, y) => {
    let dark = 0;  // 要有成片的深色才算发丝边缘（纸张边上零星的深色点不算）
    for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) {
      const nx = x + dx, ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
      const j = ny * W + nx;
      if (!trans[j] && lum[j] < P.darkLum && ++dark >= 4) return true;
    }
    return false;
  };
  for (let round = 0; round < P.fringeRounds; round++) {
    const kill = [];
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const i = y * W + x;
      if (trans[i] || !fringe[i]) continue;
      if (!(isTrans(x - 1, y) || isTrans(x + 1, y) || isTrans(x, y - 1) || isTrans(x, y + 1))) continue;
      if (hasDarkNear(x, y)) kill.push(i);
    }
    if (!kill.length) break;
    for (const i of kill) { trans[i] = 1; removed[i] = 1; }
  }

  // 3. 孤点：白块里夹着几个带点颜色的浅色像素（量化抖动出来的粉白点），不算进
  //    白块，抠完后会孤零零悬在透明里，播放时一闪一闪。把紧挨已抠像素、总共
  //    不超过几个像素、且全是浅色的小碎片一并去掉。
  {
    const seen = new Uint8Array(n);
    for (let s = 0; s < n; s++) {
      if (trans[s] || seen[s] || lum[s] < 130) continue;
      const comp = [s]; seen[s] = 1;
      let ok = true, touches = false;
      for (let q = 0; q < comp.length && ok; q++) {
        const c = comp[q], x = c % W, y = (c / W) | 0;
        for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
          if (!dx && !dy) continue;
          const nx = x + dx, ny = y + dy;
   if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
   const j = ny * W + nx;
   if (removed[j]) touches = true;
          if (trans[j] || seen[j]) continue;
   if (lum[j] < 130 || comp.length >= 12) { ok = false; continue; }
          seen[j] = 1; comp.push(j);
        }
      }
      if (ok && touches) for (const c of comp) { trans[c] = 1; removed[c] = 1; }
    }
  }

  let count = 0;
  for (let i = 0; i < n; i++) if (removed[i]) { count++; }
  return { removed, count };
}

// 全部帧：先各自判一遍，再做两轮帧间一致（第二轮让结论沿连续的漏抠帧传开）
function processAll(gif) {
  const N = gif.frames.length, CW = gif.width, CH = gif.height;
  for (const f of gif.frames)
    if (!f.hasTrans || f.interlaced) throw new Error('unsupported frame (no transparency / interlaced)');
  let results = gif.frames.map(f => processFrame(gif, f, null));
  for (let pass = 0; pass < 2; pass++) {
    const masks = results.map((r, k) => {
      const f = gif.frames[k], m = new Uint8Array(CW * CH);
      for (let i = 0; i < r.removed.length; i++)
        if (r.removed[i]) m[((i / f.w | 0) + f.y) * CW + (i % f.w) + f.x] = 1;
      return m;
    });
    results = gif.frames.map((f, k) => {
      const vote = new Uint8Array(CW * CH);
      for (let d = 1; d <= P.voteSpan; d++)
        for (const j of [(k + d) % N, (k - d + N * 4) % N]) {
   const m = masks[j];
          for (let i = 0; i < vote.length; i++) if (m[i]) vote[i] = 1;
        }
      return processFrame(gif, f, vote);
    });
  }
  return { results, counts: results.map(r => r.count) };
}

const [,, inPath, outPath, flag, markFrame, markOut] = process.argv;
const gif = readGif(inPath);
if (flag === '--mark') {
  const f = gif.frames[+markFrame];
  const { removed, count } = processAll(gif).results[+markFrame];
  const pal = f.lct || gif.gct, W = gif.width, H = gif.height, S = 2;
  const ow = W * S, oh = H * S, row = (ow * 3 + 3) & ~3;
  const buf = Buffer.alloc(54 + row * oh);
  buf.write('BM'); buf.writeUInt32LE(buf.length, 2); buf.writeUInt32LE(54, 10);
  buf.writeUInt32LE(40, 14); buf.writeInt32LE(ow, 18); buf.writeInt32LE(-oh, 22);
  buf.writeUInt16LE(1, 26); buf.writeUInt16LE(24, 28);
  for (let y = 0; y < oh; y++) for (let x = 0; x < ow; x++) {
    const cx = (x / S | 0) - f.x, cy = (y / S | 0) - f.y;
    let c = [32, 40, 48];
    if (cx >= 0 && cy >= 0 && cx < f.w && cy < f.h) {
      const i = cy * f.w + cx, k = f.indices[i];
      if (removed[i]) c = [255, 0, 255];
      else if (!(f.hasTrans && k === f.trans)) c = [pal[k * 3], pal[k * 3 + 1], pal[k * 3 + 2]];
    }
    const o = 54 + y * row + x * 3;
    buf[o] = c[2]; buf[o + 1] = c[1]; buf[o + 2] = c[0];
  }
  fs.writeFileSync(markOut, buf);
  console.log(`frame ${markFrame}: ${count} px marked`);
} else {
  const { results, counts } = processAll(gif);
  gif.frames.forEach((f, k) => {
    const removed = results[k].removed;
    for (let i = 0; i < removed.length; i++) if (removed[i]) f.indices[i] = f.trans;
  });
  writeGif(outPath, gif);
  const sum = counts.reduce((a, b) => a + b, 0);
  console.log(`${gif.frames.length} frames, removed ${sum} px total, per frame min ${Math.min(...counts)} max ${Math.max(...counts)}`);
}
