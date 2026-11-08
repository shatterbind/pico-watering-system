// Shared helpers for the e-paper dashboard tools (Node.js, no dependencies).
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const ROOT = path.resolve(__dirname, '../../../..');
const DRIVER = path.join(ROOT, 'lib/pico-epd-driver');
const TEMPLATE_H = path.join(DRIVER, 'src/canvas/images/watering_template.h');
const TEMPLATE_PNG = path.join(DRIVER, 'assets/watering_template.png');
const FONTS = path.join(DRIVER, 'src/canvas/fonts');

const W = 122, H = 250, ROW_BYTES = 31; // 2 bpp, rows byte-padded
// Panel palette, indexed by the 2-bit value: BLACK=0 WHITE=1 YELLOW=2 RED=3
const PALETTE = [[0, 0, 0], [255, 255, 255], [255, 205, 0], [215, 25, 25]];
const COLOR = { BLACK: 0, WHITE: 1, YELLOW: 2, RED: 3 };

// ---------- template header (source of truth) ----------
function readTemplate(file = TEMPLATE_H) {
  const text = fs.readFileSync(file, 'utf8');
  const start = text.indexOf('{');
  const bytes = [...text.slice(start).matchAll(/0x([0-9a-fA-F]{2})/g)].map(m => parseInt(m[1], 16));
  if (bytes.length !== ROW_BYTES * H) throw new Error(`expected ${ROW_BYTES * H} bytes, got ${bytes.length}`);
  const px = [];
  for (let y = 0; y < H; y++) {
    const row = [];
    for (let x = 0; x < W; x++) row.push((bytes[y * ROW_BYTES + (x >> 2)] >> (6 - 2 * (x & 3))) & 3);
    px.push(row);
  }
  return { text, start, bytes, px };
}

// Writes pixels back into the header (keeping its layout and comments) and regenerates the PNG.
function writeTemplate(tpl, file = TEMPLATE_H, pngFile = TEMPLATE_PNG) {
  const out = tpl.bytes.slice();
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const k = y * ROW_BYTES + (x >> 2), s = 6 - 2 * (x & 3);
    out[k] = (out[k] & ~(3 << s)) | (tpl.px[y][x] << s);
  }
  let n = 0;
  const body = tpl.text.slice(tpl.start).replace(/0x[0-9a-fA-F]{2}/g,
    () => '0x' + out[n++].toString(16).toUpperCase().padStart(2, '0'));
  fs.writeFileSync(file, tpl.text.slice(0, tpl.start) + body);
  writePng(pngFile, pixelsToRgb(tpl.px));
}

function pixelsToRgb(px, scale = 1) {
  const w = px[0].length * scale, h = px.length * scale, rgb = Buffer.alloc(w * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const c = PALETTE[px[(y / scale) | 0][(x / scale) | 0]];
    rgb[(y * w + x) * 3] = c[0]; rgb[(y * w + x) * 3 + 1] = c[1]; rgb[(y * w + x) * 3 + 2] = c[2];
  }
  return { w, h, rgb };
}

// ---------- PNG (8-bit RGB) ----------
const CRC = (() => { const t = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = b => { let c = ~0; for (const x of b) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return (~c) >>> 0; };
function writePng(file, { w, h, rgb }) {
  const raw = Buffer.alloc(h * (w * 3 + 1));
  for (let y = 0; y < h; y++) rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3);
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]));
}

// ---------- fonts (driver bitmap fonts, proportional) ----------
const fontCache = {};
function loadFont(name) { // e.g. "tahoma_8"
  if (fontCache[name]) return fontCache[name];
  const text = fs.readFileSync(path.join(FONTS, `${name}_font.h`), 'utf8');
  const height = +text.match(/CHAR_HEIGHT\s+(\d+)/)[1];
  const glyphs = {};
  for (const m of text.matchAll(/^\s*(\d+), \/\*N\*\/(.*)\/\/ Char (\d+)/gm)) {
    const b = [...m[2].matchAll(/0x([0-9a-fA-F]{2})/g)].map(x => parseInt(x[1], 16));
    glyphs[+m[3]] = { w: +m[1], b, bpr: b.length / height };
  }
  return (fontCache[name] = { name, height, glyphs });
}
// Same as canvas_get_text_width(): glyph widths + gap between glyphs.
function textWidth(text, font, gap = 1) {
  let w = 0, first = true;
  for (const ch of text) { const g = font.glyphs[ch.charCodeAt(0)]; if (!g) continue; if (!first) w += gap; w += g.w; first = false; }
  return w;
}

// ---------- main.c defines ----------
function readDefines(file = path.join(ROOT, 'main.c')) {
  const raw = {};
  const src = fs.readFileSync(file, 'utf8').replace(/\\\r?\n/g, ' ');
  for (const line of src.split(/\r?\n/)) {
    const m = line.match(/^\s*#define\s+([A-Z_][A-Z0-9_]*)\s+(.+?)\s*(\/\/.*|\/\*.*)?$/);
    if (m) raw[m[1]] = m[2].trim();
  }
  const cache = {};
  const value = name => {
    if (name in cache) return cache[name];
    if (!(name in raw)) return undefined;
    let v = raw[name];
    if (/^".*"$/.test(v)) return (cache[name] = JSON.parse(v));
    if (/^FONT_TAHOMA_\d+$/.test(v)) return (cache[name] = v);
    const expr = v.replace(/\b([A-Z_][A-Z0-9_]*)\b/g, id => { const r = value(id); return typeof r === 'number' ? r : (r === undefined ? id : JSON.stringify(r)); })
      .replace(/(\d+(\.\d+)?)f\b/g, '$1').replace(/\btrue\b/g, '1').replace(/\bfalse\b/g, '0');
    try { v = Function(`"use strict";return (${expr});`)(); } catch { v = raw[name]; }
    return (cache[name] = v);
  };
  return new Proxy({}, { get: (_, k) => value(k), has: (_, k) => k in raw });
}

module.exports = { ROOT, DRIVER, TEMPLATE_H, TEMPLATE_PNG, W, H, PALETTE, COLOR,
  readTemplate, writeTemplate, pixelsToRgb, writePng, loadFont, textWidth, readDefines };
