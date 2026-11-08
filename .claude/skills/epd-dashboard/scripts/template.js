#!/usr/bin/env node
// Inspect and edit the dashboard background (watering_template.h + .png) without Python.
// The .h header is the source of truth; every edit rewrites the header and regenerates the PNG.
//
//   node template.js show <y0> <y1> [x0 x1]      ASCII view (. white, # black, Y yellow, R red)
//   node template.js bbox <x0> <y0> <x1> <y1>    bounding box of non-white pixels in a region
//   node template.js clear <x0> <y0> <x1> <y1>   set a region to white
//   node template.js move <x0> <y0> <x1> <y1> <dx> <dy>   move a region (vacated pixels -> white)
//   node template.js check                       compare PNG pixels with the header
'use strict';
const fs = require('fs');
const zlib = require('zlib');
const { readTemplate, writeTemplate, TEMPLATE_PNG, W, H, PALETTE } = require('./lib');

const [cmd, ...args] = process.argv.slice(2);
const n = args.map(Number);
const tpl = readTemplate();
const CH = ['#', '.', 'Y', 'R']; // indexed by BLACK, WHITE, YELLOW, RED

const inRange = (x, y) => x >= 0 && y >= 0 && x < W && y < H;

switch (cmd) {
  case 'show': {
    const [y0, y1, x0 = 0, x1 = W - 1] = n;
    console.log('    ' + Array.from({ length: x1 - x0 + 1 }, (_, i) => ((x0 + i) % 10 === 0 ? '|' : ' ')).join('') + `  (| = x multiple of 10, from x=${x0})`);
    for (let y = y0; y <= y1; y++) console.log(String(y).padStart(3) + ' ' + tpl.px[y].slice(x0, x1 + 1).map(v => CH[v]).join(''));
    break;
  }
  case 'bbox': {
    const [x0, y0, x1, y1] = n;
    let bx0 = Infinity, by0 = Infinity, bx1 = -1, by1 = -1;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) if (tpl.px[y][x] !== 1) {
      bx0 = Math.min(bx0, x); by0 = Math.min(by0, y); bx1 = Math.max(bx1, x); by1 = Math.max(by1, y);
    }
    console.log(bx1 < 0 ? 'region is all white' : `x ${bx0}..${bx1}, y ${by0}..${by1}`);
    break;
  }
  case 'clear': {
    const [x0, y0, x1, y1] = n;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) tpl.px[y][x] = 1;
    writeTemplate(tpl); console.log(`cleared x ${x0}..${x1}, y ${y0}..${y1}`);
    break;
  }
  case 'move': {
    const [x0, y0, x1, y1, dx, dy] = n;
    const src = tpl.px.map(r => r.slice());
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) tpl.px[y][x] = 1;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) if (inRange(x + dx, y + dy)) tpl.px[y + dy][x + dx] = src[y][x];
    writeTemplate(tpl); console.log(`moved x ${x0}..${x1}, y ${y0}..${y1} by (${dx}, ${dy})`);
    break;
  }
  case 'check': {
    // Decode the PNG (8-bit RGB, any filter) and compare with the header.
    const b = fs.readFileSync(TEMPLATE_PNG); let o = 8, w, h, idat = [];
    while (o < b.length) { const len = b.readUInt32BE(o), t = b.toString('ascii', o + 4, o + 8), d = b.subarray(o + 8, o + 8 + len);
      if (t === 'IHDR') { w = d.readUInt32BE(0); h = d.readUInt32BE(4); if (d[8] !== 8 || d[9] !== 2) throw new Error('PNG must be 8-bit RGB'); }
      if (t === 'IDAT') idat.push(d); o += 12 + len; }
    const raw = zlib.inflateSync(Buffer.concat(idat)), st = w * 3, px = Buffer.alloc(h * st);
    for (let y = 0; y < h; y++) { const f = raw[y * (st + 1)];
      for (let x = 0; x < st; x++) { const a = x >= 3 ? px[y * st + x - 3] : 0, up = y ? px[(y - 1) * st + x] : 0, c = x >= 3 && y ? px[(y - 1) * st + x - 3] : 0;
        let v = raw[y * (st + 1) + 1 + x];
        if (f === 1) v += a; else if (f === 2) v += up; else if (f === 3) v += (a + up) >> 1;
        else if (f === 4) { const p = a + up - c, pa = Math.abs(p - a), pb = Math.abs(p - up), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? up : c; }
        px[y * st + x] = v & 255; } }
    let diff = 0;
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
      const i = (y * w + x) * 3, rgb = [px[i], px[i + 1], px[i + 2]];
      const near = PALETTE.map(c => (c[0] - rgb[0]) ** 2 + (c[1] - rgb[1]) ** 2 + (c[2] - rgb[2]) ** 2);
      if (near.indexOf(Math.min(...near)) !== tpl.px[y][x]) diff++;
    }
    console.log(diff ? `MISMATCH: ${diff} pixels differ between PNG and header` : 'PNG and header match');
    process.exitCode = diff ? 1 : 0;
    break;
  }
  default:
    console.log(fs.readFileSync(__filename, 'utf8').split('\n').slice(1, 10).join('\n'));
    process.exitCode = 1;
}
