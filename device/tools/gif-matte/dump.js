// 把 GIF 的某一帧（可选局部、放大）导出成 BMP，透明处画成指定底色，便于检查抠图。
// 用法：node dump.js <gif> <帧号> <out.bmp> [x y w h scale] [底色 rrggbb]
'use strict';
const fs = require('fs');
const { readGif } = require('./gifio.js');
const [,, gifPath, frameNo, out, X = 0, Y = 0, W, H, S = 1, bg = 'ff00ff'] = process.argv;
const g = readGif(gifPath);
const f = g.frames[+frameNo];
const pal = f.lct || g.gct;
const x0 = +X, y0 = +Y, w = +(W || g.width), h = +(H || g.height), s = +S;
const bgc = [parseInt(bg.slice(0, 2), 16), parseInt(bg.slice(2, 4), 16), parseInt(bg.slice(4, 6), 16)];
const ow = w * s, oh = h * s, row = (ow * 3 + 3) & ~3;
const buf = Buffer.alloc(54 + row * oh);
buf.write('BM'); buf.writeUInt32LE(buf.length, 2); buf.writeUInt32LE(54, 10);
buf.writeUInt32LE(40, 14); buf.writeInt32LE(ow, 18); buf.writeInt32LE(-oh, 22);
buf.writeUInt16LE(1, 26); buf.writeUInt16LE(24, 28);
for (let y = 0; y < oh; y++) for (let x = 0; x < ow; x++) {
  const cx = x0 + Math.floor(x / s) - f.x, cy = y0 + Math.floor(y / s) - f.y;
  let c = bgc;
  if (cx >= 0 && cy >= 0 && cx < f.w && cy < f.h) {
    const i = f.indices[cy * f.w + cx];
    if (!(f.hasTrans && i === f.trans)) c = [pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]];
  }
  const o = 54 + y * row + x * 3;
  buf[o] = c[2]; buf[o + 1] = c[1]; buf[o + 2] = c[0];
}
fs.writeFileSync(out, buf);
