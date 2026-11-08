#!/usr/bin/env node
// Renders what the firmware draws on the e-paper, using the real template header, the
// driver's bitmap fonts and the SLOT_* / LABEL_* / text defines from main.c.
// It mirrors the drawing code in main.c (dashboard section of main(), draw_battery,
// draw_temperature); keep it in sync when that code changes.
//
//   node preview.js [--state watered|optimal|error|skip-bor|low-battery] [--moisture 34] [--raw 3076] [--temp 21.1]
//                   [--vsys 3.92|usb] [--time "11-08 01:16"] [--reset BOR] [--scale 3] [--out file.png]
//   --reset: reset-reason code shown in the footer (empty = normal RTC alarm wake)
//
// Prints width warnings like the firmware does. Default output: build/dashboard_preview.png
'use strict';
const path = require('path');
const { ROOT, W, H, COLOR, readTemplate, pixelsToRgb, writePng, loadFont, textWidth, readDefines } = require('./lib');

const opt = { state: 'watered', moisture: 34, raw: 3076, temp: 21.1, vsys: 3.92, time: '11-08 01:16', scale: 3,
  out: path.join(ROOT, 'build/dashboard_preview.png') };
const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i += 2) opt[argv[i].replace(/^--/, '')] = argv[i + 1];
const D = readDefines();
const px = readTemplate().px;
const warnings = [];

const fontOf = id => loadFont(String(D[id] ?? id).toLowerCase().replace(/^font_/, '')); // FONT_SMALL -> tahoma_8
const SMALL = fontOf('FONT_SMALL'), MEDIUM = fontOf('FONT_MEDIUM'), LARGE = fontOf('FONT_LARGE');

const set = (x, y, c) => { if (x >= 0 && y >= 0 && x < W && y < H) px[y][x] = c; };
const fillRect = (x, y, w, h, c) => { for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) set(x + i, y + j, c); };
// draw_text / draw_text_with_bg: glyph rows are little-endian bytes, bit k = column k.
function drawText(x, y, text, font, color, bg = null, gap = 1) {
  for (const ch of text) {
    const g = font.glyphs[ch.charCodeAt(0)]; if (!g) continue;
    for (let r = 0; r < font.height; r++) {
      let row = 0; for (let k = 0; k < g.bpr; k++) row |= g.b[r * g.bpr + k] << (8 * k);
      for (let k = 0; k < g.w + gap; k++) {
        if (k < g.w && ((row >> k) & 1)) set(x + k, y + r, color);
        else if (bg !== null) set(x + k, y + r, bg);
      }
    }
    x += g.w + gap;
  }
}
const centered = (cx, y, t, f, c) => drawText(cx - (textWidth(t, f) >> 1), y, t, f, c);

// ---- section labels ----
drawText(D.LABEL_PLANT_X, D.LABEL_PLANT_Y, D.LABEL_PLANT_TEXT, SMALL, COLOR.BLACK);
drawText(D.LABEL_MOISTURE_X, D.LABEL_MOISTURE_Y, D.LABEL_MOISTURE_TEXT, SMALL, COLOR.BLACK);
centered(D.LABEL_DRY_CX, D.LABEL_SCALE_Y, 'DRY', SMALL, COLOR.BLACK);
centered(D.LABEL_OK_CX, D.LABEL_SCALE_Y, 'OK', SMALL, COLOR.BLACK);
centered(D.LABEL_WET_CX, D.LABEL_SCALE_Y, 'WET', SMALL, COLOR.BLACK);
drawText(D.LABEL_TEMP_X, D.LABEL_TEMP_Y, D.LABEL_TEMP_TEXT, SMALL, COLOR.BLACK);
drawText(D.LABEL_REFRESH_X, D.LABEL_REFRESH_Y, D.LABEL_REFRESH_TEXT, SMALL, COLOR.BLACK);

// ---- header ----
fillRect(D.SLOT_TITLE_X - 1, D.SLOT_TITLE_Y, D.TITLE_AREA_END_X - D.SLOT_TITLE_X + 2, D.TITLE_FONT_HEIGHT, COLOR.WHITE);
if (textWidth(D.HEADER_TITLE_TEXT, SMALL) > D.TITLE_AREA_END_X - D.SLOT_TITLE_X + 1) warnings.push('HEADER_TITLE_TEXT is too wide, it overlaps the battery');
drawText(D.SLOT_TITLE_X, D.SLOT_TITLE_Y, D.HEADER_TITLE_TEXT, SMALL, COLOR.BLACK, COLOR.WHITE);

const usb = String(opt.vsys).toLowerCase() === 'usb';
const vsys = usb ? 5.0 : Number(opt.vsys);
const bars = vsys >= D.BATTERY_3_BARS_V ? 3 : vsys >= D.BATTERY_2_BARS_V ? 2 : vsys >= D.BATTERY_1_BAR_V ? 1 : 0;
for (let i = 0; i < bars; i++) fillRect(D.BATTERY_BAR_X0 + i * D.BATTERY_BAR_STEP, D.BATTERY_BAR_Y0, D.BATTERY_BAR_WIDTH, D.BATTERY_BAR_HEIGHT, bars > 1 ? COLOR.BLACK : COLOR.RED);
drawText(D.SLOT_BATTERY_X, D.SLOT_BATTERY_Y, vsys >= D.VSYS_USB_V ? 'USB' : `${vsys.toFixed(2)}V`, SMALL, bars <= 1 ? COLOR.RED : COLOR.BLACK, COLOR.WHITE);

// ---- plant ----
drawText(D.SLOT_PLANT_NAME_X, D.SLOT_PLANT_NAME_Y, D.PLANT_NAME_TEXT, MEDIUM, COLOR.BLACK);
drawText(D.SLOT_PLANT_SUB_X, D.SLOT_PLANT_SUB_Y, D.PLANT_SUB_TEXT, SMALL, COLOR.RED);

// ---- soil moisture ----
const ok = opt.state !== 'error';
const water = opt.state === 'watered';
const pct = Math.max(0, Math.min(100, Math.trunc(Number(opt.moisture))));
drawText(D.SLOT_MOISTURE_PCT_X, D.SLOT_MOISTURE_PCT_Y, ok ? `${pct}%` : '--%', LARGE, COLOR.BLACK);
// draw_text_right: raw ADC value column (shown also on a sensor fault)
const right = (rx, y, t, f, c) => drawText(rx - textWidth(t, f) + 1, y, t, f, c);
right(D.SLOT_MOISTURE_RAW_RIGHT_X, D.SLOT_MOISTURE_RAW_LABEL_Y, D.MOISTURE_RAW_LABEL_TEXT, SMALL, COLOR.BLACK);
right(D.SLOT_MOISTURE_RAW_RIGHT_X, D.SLOT_MOISTURE_RAW_Y, String(opt.raw), SMALL, COLOR.BLACK);
// decide_pomp() outcome -> status text (states: watered, optimal, error, skip-bor, low-battery)
const STATUS = {
  error: D.STATUS_SENSOR_ERROR_TEXT,
  watered: D.STATUS_WATERED_FORMAT.replace('%u', String(Math.round(D.TIME_POMP_ON / 1000))),
  'skip-bor': D.STATUS_SKIP_BROWNOUT_TEXT,
  'low-battery': D.STATUS_SKIP_LOW_BATTERY_TEXT,
  optimal: D.STATUS_OPTIMAL_TEXT,
};
const status = STATUS[opt.state] ?? STATUS.optimal;
if (textWidth(status, SMALL) > D.STATUS_MAX_WIDTH) warnings.push(`status "${status}" is wider than the card (${textWidth(status, SMALL)} > ${D.STATUS_MAX_WIDTH} px)`);
drawText(D.SLOT_MOISTURE_STATUS_X, D.SLOT_MOISTURE_STATUS_Y, status, SMALL, opt.state === 'optimal' ? COLOR.BLACK : COLOR.RED);
const fillW = Math.trunc(D.MOISTURE_BAR_MAX_WIDTH * pct / 100);
if (ok && fillW > 0) fillRect(D.MOISTURE_BAR_X, D.MOISTURE_BAR_Y, fillW, D.MOISTURE_BAR_HEIGHT, pct < D.MIN_HUMIDITY ? COLOR.RED : COLOR.YELLOW);

// ---- temperature (draw_temperature) ----
const tv = Number(opt.temp).toFixed(1);
drawText(D.SLOT_TEMP_VAL_X, D.SLOT_TEMP_VAL_Y, tv, LARGE, COLOR.BLACK);
const rx = D.SLOT_TEMP_VAL_X + textWidth(tv, LARGE) + 2, ry = D.SLOT_TEMP_VAL_Y + 1;
fillRect(rx + 1, ry, 3, 1, COLOR.BLACK); fillRect(rx + 1, ry + 4, 3, 1, COLOR.BLACK);
fillRect(rx, ry + 1, 1, 3, COLOR.BLACK); fillRect(rx + 4, ry + 1, 1, 3, COLOR.BLACK);
drawText(rx + 6, D.SLOT_TEMP_VAL_Y, 'C', LARGE, COLOR.BLACK);

// ---- footer ----
drawText(D.SLOT_FOOTER_TIME_X, D.SLOT_FOOTER_TIME_Y, opt.time, SMALL, COLOR.RED);
if (opt.reset) right(D.SLOT_FOOTER_RESET_RIGHT_X, D.SLOT_FOOTER_RESET_Y, opt.reset, SMALL, COLOR.RED);
const d = D.WAKE_INTERVAL_DAYS, h = D.WAKE_INTERVAL_HOURS;
const interval = d > 0 && h > 0 ? `${d}d ${h}h` : d > 0 ? `${d}d` : `${h}h`;
drawText(D.SLOT_FOOTER_CYCLE_X, D.SLOT_FOOTER_CYCLE_Y, D.FOOTER_CYCLE_FORMAT.replace('%s', interval), SMALL, COLOR.BLACK);

writePng(opt.out, pixelsToRgb(px, Number(opt.scale)));
for (const w of warnings) console.log('WARNING: ' + w);
console.log(`wrote ${opt.out} (${W * opt.scale}x${H * opt.scale}, state=${opt.state})`);
