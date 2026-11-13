// 极简 GIF 读写（索引级，不重新量化颜色）。
// 读：逐帧解出调色板索引、矩形、延时、处置方式、透明索引。
// 写：原样的调色板与参数，LZW 重新编码索引。
'use strict';
const fs = require('fs');

function lzwDecode(minCode, data, pixelCount) {
  const out = new Uint8Array(pixelCount);
  const clear = 1 << minCode, eoi = clear + 1;
  let codeSize = minCode + 1, next = eoi + 1;
  const prefix = new Int32Array(4096), suffix = new Uint8Array(4096), stack = new Uint8Array(4097);
  let bits = 0, acc = 0, pos = 0, op = 0, prev = -1, first = 0;
  while (op < pixelCount) {
    while (bits < codeSize) {
      if (pos >= data.length) return out;
      acc |= data[pos++] << bits; bits += 8;
    }
    let code = acc & ((1 << codeSize) - 1);
    acc >>>= codeSize; bits -= codeSize;
    if (code === clear) { codeSize = minCode + 1; next = eoi + 1; prev = -1; continue; }
    if (code === eoi) break;
    if (prev === -1) { out[op++] = code; prev = code; first = code; continue; }
    let cur = code, sp = 0;
    if (code >= next) { stack[sp++] = first; cur = prev; }
    while (cur >= clear) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
    first = cur; stack[sp++] = cur;
    while (sp > 0 && op < pixelCount) out[op++] = stack[--sp];
    if (next < 4096) {
      prefix[next] = prev; suffix[next] = first; next++;
      if (next === (1 << codeSize) && codeSize < 12) codeSize++;
    }
    prev = code;
  }
  return out;
}

function lzwEncode(minCode, indices) {
  const clear = 1 << minCode, eoi = clear + 1;
  const bytes = [];
  let acc = 0, bits = 0, codeSize = minCode + 1, next = eoi + 1;
  const emit = c => {
    acc |= c << bits; bits += codeSize;
    while (bits >= 8) { bytes.push(acc & 255); acc >>>= 8; bits -= 8; }
  };
  let dict = new Map();
  emit(clear);
  let cur = indices[0];
  for (let i = 1; i < indices.length; i++) {
    const k = indices[i], key = (cur << 8) | k;
    const hit = dict.get(key);
    if (hit !== undefined) { cur = hit; continue; }
    emit(cur);
    if (next < 4096) {
      dict.set(key, next++);
      if (next > (1 << codeSize) && codeSize < 12) codeSize++;
    } else {
      emit(clear); dict = new Map(); codeSize = minCode + 1; next = eoi + 1;
    }
    cur = k;
  }
  emit(cur); emit(eoi);
  if (bits > 0) bytes.push(acc & 255);
  return Uint8Array.from(bytes);
}

function readGif(path) {
  const b = fs.readFileSync(path);
  let p = 6;
  const gif = { header: b.subarray(0, 6), width: b.readUInt16LE(6), height: b.readUInt16LE(8) };
  const packed = b[10];
  gif.lsd = b.subarray(6, 13); p = 13;
  if (packed & 0x80) { const n = 3 << ((packed & 7) + 1); gif.gct = b.subarray(p, p + n); p += n; }
  gif.frames = []; gif.extras = [];
  let gce = null;
  const subBlocks = () => { const parts = []; while (b[p] !== 0) { parts.push(b.subarray(p + 1, p + 1 + b[p])); p += 1 + b[p]; } p++; return Buffer.concat(parts); };
  while (p < b.length) {
    const t = b[p++];
    if (t === 0x3B) break;
    if (t === 0x21) {
      const label = b[p++];
      if (label === 0xF9) {
        gce = { packed: b[p + 1], delay: b.readUInt16LE(p + 2), trans: b[p + 4] };
        p += 6;
      } else {
        const start = p - 2; subBlocks();
        gif.extras.push(b.subarray(start, p));  // NETSCAPE 循环等，原样保留
      }
    } else if (t === 0x2C) {
      const f = { x: b.readUInt16LE(p), y: b.readUInt16LE(p + 2), w: b.readUInt16LE(p + 4), h: b.readUInt16LE(p + 6) };
      const fp = b[p + 8]; p += 9;
      f.interlaced = !!(fp & 0x40);
      if (fp & 0x80) { const n = 3 << ((fp & 7) + 1); f.lct = b.subarray(p, p + n); p += n; }
      f.minCode = b[p++];
      f.indices = lzwDecode(f.minCode, subBlocks(), f.w * f.h);
      f.delay = gce ? gce.delay : 0;
      f.disposal = gce ? (gce.packed >> 2) & 7 : 0;
      f.hasTrans = gce ? !!(gce.packed & 1) : false;
      f.trans = gce ? gce.trans : 0;
      gce = null;
      gif.frames.push(f);
    } else break;
  }
  return gif;
}

function writeGif(path, gif) {
  const parts = [gif.header, gif.lsd];
  if (gif.gct) parts.push(gif.gct);
  for (const e of gif.extras) parts.push(e);
  for (const f of gif.frames) {
    const gce = Buffer.from([0x21, 0xF9, 4, (f.disposal << 2) | (f.hasTrans ? 1 : 0), 0, 0, f.trans, 0]);
    gce.writeUInt16LE(f.delay, 4);
    const id = Buffer.alloc(10); id[0] = 0x2C;
    id.writeUInt16LE(f.x, 1); id.writeUInt16LE(f.y, 3); id.writeUInt16LE(f.w, 5); id.writeUInt16LE(f.h, 7);
    if (f.lct) id[9] = 0x80 | (Math.log2(f.lct.length / 3) - 1);
    parts.push(gce, id);
    if (f.lct) parts.push(f.lct);
    const data = lzwEncode(f.minCode, f.indices);
    const blocks = [Buffer.from([f.minCode])];
    for (let i = 0; i < data.length; i += 255) {
      const n = Math.min(255, data.length - i);
      blocks.push(Buffer.from([n]), Buffer.from(data.subarray(i, i + n)));
    }
    blocks.push(Buffer.from([0]));
    parts.push(...blocks);
  }
  parts.push(Buffer.from([0x3B]));
  fs.writeFileSync(path, Buffer.concat(parts));
}

module.exports = { readGif, writeGif };
