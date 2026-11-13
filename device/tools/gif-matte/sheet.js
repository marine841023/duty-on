// 逐帧拼图：把 GIF 每隔 N 帧的同一局部并排导出成一张 BMP，用来比较帧与帧之间
// 抠图是否一致。透明处画成品红。
// 用法：node sheet.js <gif> <out.bmp> <x> <y> <w> <h> [scale=3] [step=4] [cols=8]
'use strict';
const fs = require('fs');
const { readGif } = require('./gifio.js');
const [,, gifPath, out, X, Y, W, H, S = 3, STEP = 4, COLS = 8] = process.argv;
const g = readGif(gifPath);
const x0 = +X, y0 = +Y, w = +W, h = +H, s = +S, step = +STEP, cols = +COLS;
const picks = [];
for (let i = 0; i < g.frames.length; i += step) picks.push(i);
const rows = Math.ceil(picks.length / cols), gap = 4;
const ow = cols * (w * s + gap), oh = rows * (h * s + gap), row = (ow * 3 + 3) & ~3;
const buf = Buffer.alloc(54 + row * oh, 40);
buf.write('BM'); buf.writeUInt32LE(buf.length, 2); buf.writeUInt32LE(54, 10);
buf.writeUInt32LE(40, 14); buf.writeInt32LE(ow, 18); buf.writeInt32LE(-oh, 22);
buf.writeUInt16LE(1, 26); buf.writeUInt16LE(24, 28);
buf.writeUInt32LE(0, 30); buf.writeUInt32LE(0, 34); buf.writeUInt32LE(0, 38); buf.writeUInt32LE(0, 42);
buf.writeUInt32LE(0, 46); buf.writeUInt32LE(0, 50);
picks.forEach((fi, k) => {
  const f = g.frames[fi], pal = f.lct || g.gct;
  const ox = (k % cols) * (w * s + gap), oy = Math.floor(k / cols) * (h * s + gap);
  for (let y = 0; y < h * s; y++) for (let x = 0; x < w * s; x++) {
    const cx = x0 + Math.floor(x / s) - f.x, cy = y0 + Math.floor(y / s) - f.y;
    let c = [255, 0, 255];
    if (cx >= 0 && cy >= 0 && cx < f.w && cy < f.h) {
      const i = f.indices[cy * f.w + cx];
      if (!(f.hasTrans && i === f.trans)) c = [pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]];
    }
    const o = 54 + (oy + y) * row + (ox + x) * 3;
    buf[o] = c[2]; buf[o + 1] = c[1]; buf[o + 2] = c[0];
  }
});
fs.writeFileSync(out, buf);
console.log('frames: ' + picks.join(','));
