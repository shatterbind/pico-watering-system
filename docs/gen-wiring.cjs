#!/usr/bin/env node
// Generates docs/wiring.svg, a breadboard-style wiring diagram.
// Pin numbers follow the Raspberry Pi Pico 2 physical pinout (USB at the top).
// Run: node docs/gen-wiring.cjs
'use strict';
const fs = require('fs');
const path = require('path');

const W = 1260, H = 800;
const PICO = { x0: 470, x1: 630, y0: 120, y1: 672 };
const row = i => 150 + (i - 1) * 26; // pin row 1..20 from the USB end
const LEFT = ['GP0', 'GP1', 'GND', 'GP2', 'GP3', 'GP4', 'GP5', 'GND', 'GP6', 'GP7', 'GP8', 'GP9', 'GND', 'GP10', 'GP11', 'GP12', 'GP13', 'GND', 'GP14', 'GP15'];
const RIGHT = ['VBUS', 'VSYS', 'GND', '3V3_EN', '3V3', 'ADC_VREF', 'GP28', 'AGND', 'GP27', 'GP26', 'RUN', 'GP22', 'GND', 'GP21', 'GP20', 'GP19', 'GP18', 'GND', 'GP17', 'GP16'];
const USED = new Set(['L11', 'L12', 'L13', 'L14', 'L15', 'L16', 'L17', 'L20', 'R2', 'R3', 'R5', 'R8', 'R10', 'R12', 'R13', 'R14', 'R15']);

const C = { v5: '#ef6c00', v3: '#d32f2f', gnd: '#212121', dc: '#8e24aa', cs: '#1e88e5', clk: '#fbc02d', din: '#43a047',
  rst: '#6d4c41', busy: '#757575', sda: '#00897b', scl: '#3949ab', sqw: '#c2185b', aout: '#00acc1', pump: '#5d4037', vsys: '#f4511e' };

const out = [];
const add = s => out.push(s);
const wire = (pts, color, w = 4) => add(`<polyline points="${pts.map(p => p.join(',')).join(' ')}" fill="none" stroke="${color}" stroke-width="${w}" stroke-linejoin="round" stroke-linecap="round"/>`);
const dot = (x, y, color) => add(`<circle cx="${x}" cy="${y}" r="5" fill="${color}"/>`);
const text = (x, y, s, o = {}) => add(`<text x="${x}" y="${y}" font-size="${o.size || 13}" font-weight="${o.bold ? 700 : 400}" fill="${o.fill || '#1b1b1b'}" text-anchor="${o.anchor || 'start'}"${o.mono ? ' font-family="ui-monospace,Consolas,monospace"' : ''}>${s}</text>`);
const pin = (x, y, label, side) => { // header pin on a module edge
  add(`<rect x="${x - 6}" y="${y - 6}" width="12" height="12" rx="2" fill="#c9a227" stroke="#7a5f00"/>`);
  text(side === 'left' ? x + 12 : x - 12, y + 4, label, { size: 12, anchor: side === 'left' ? 'start' : 'end', mono: true });
};
const board = (x0, y0, x1, y1, title, sub, fill = '#1565c0') => {
  add(`<rect x="${x0}" y="${y0}" width="${x1 - x0}" height="${y1 - y0}" rx="8" fill="${fill}" stroke="#0d1b2a" stroke-width="2"/>`);
  text((x0 + x1) / 2, y0 + 22, title, { anchor: 'middle', bold: true, fill: '#fff', size: 14 });
  if (sub) text((x0 + x1) / 2, y0 + 40, sub, { anchor: 'middle', fill: '#e3f2fd', size: 11 });
};

// ---------- background, rails ----------
add(`<rect width="${W}" height="${H}" fill="#fafafa"/>`);
const rail = (y, color, label) => { add(`<line x1="14" y1="${y}" x2="${W - 14}" y2="${y}" stroke="${color}" stroke-width="5"/>`); text(W - 18, y - 6, label, { anchor: 'end', size: 12, bold: true, fill: color }); };
rail(40, C.v5, '5 V (pump supply)');
rail(60, C.v3, '3.3 V');
rail(80, C.gnd, 'GND');
rail(700, C.v3, '3.3 V');
rail(720, C.gnd, 'GND');
wire([[14, 60], [14, 700]], C.v3, 5);
wire([[26, 80], [26, 720]], C.gnd, 5);

// ---------- wires (drawn before modules so pins sit on top) ----------
// Battery -> diode -> VSYS
wire([[630, row(2)], [640, row(2)], [640, 96], [310, 96], [310, 168], [300, 168]], C.vsys);
wire([[60, 198], [45, 198], [45, 80]], C.gnd); dot(45, 80, C.gnd);
// Pico power to rails
wire([[630, row(3)], [650, row(3)], [650, 80]], C.gnd); dot(650, 80, C.gnd);
wire([[630, row(5)], [660, row(5)], [660, 60]], C.v3); dot(660, 60, C.v3);
// E-paper (straight to the left header)
wire([[300, row(11)], [470, row(11)]], C.dc);
wire([[300, row(12)], [470, row(12)]], C.cs);
wire([[300, row(13)], [470, row(13)]], C.gnd);
wire([[300, row(14)], [470, row(14)]], C.clk);
wire([[300, row(15)], [470, row(15)]], C.din);
wire([[300, row(16)], [470, row(16)]], C.rst);
wire([[300, row(17)], [470, row(17)]], C.busy);
wire([[300, row(18)], [320, row(18)], [320, 700]], C.v3); dot(320, 700, C.v3);
// Soil sensor
wire([[700, row(7)], [680, row(7)], [680, 60]], C.v3); dot(680, 60, C.v3);
wire([[700, row(8)], [630, row(8)]], C.gnd);
wire([[700, row(10)], [630, row(10)]], C.aout);
// Relay control
wire([[900, row(12)], [630, row(12)]], '#ff7043');
wire([[900, row(13)], [630, row(13)]], C.gnd);
wire([[920, 418], [920, 40]], C.v5); dot(920, 40, C.v5);
// DS3231
wire([[700, row(14)], [630, row(14)]], C.scl);
wire([[700, row(15)], [630, row(15)]], C.sda);
wire([[700, 540], [665, 540], [665, 700]], C.v3); dot(665, 700, C.v3);
wire([[700, 566], [680, 566], [680, 720]], C.gnd); dot(680, 720, C.gnd);
wire([[700, 592], [695, 592], [695, 752], [440, 752], [440, row(20)], [470, row(20)]], C.sqw);
// Pump circuit
wire([[1060, 440], [1080, 440], [1080, 40]], C.v5); dot(1080, 40, C.v5);
wire([[1060, 470], [1150, 470], [1150, 530]], C.pump);
wire([[1150, 590], [1150, 625], [1236, 625], [1236, 80]], C.gnd); dot(1236, 80, C.gnd);
wire([[1120, 120], [1120, 40]], C.v5); dot(1120, 40, C.v5);
wire([[1200, 120], [1200, 80]], C.gnd); dot(1200, 80, C.gnd);

// Schottky diode on the VSYS wire (anode left, cathode right: battery -> VSYS)
add(`<polygon points="380,86 380,106 398,96" fill="#212121"/><line x1="399" y1="85" x2="399" y2="107" stroke="#212121" stroke-width="3"/>`);
text(389, 124, 'Schottky', { anchor: 'middle', size: 11 });

// ---------- Pico 2 ----------
add(`<rect x="${PICO.x0}" y="${PICO.y0}" width="${PICO.x1 - PICO.x0}" height="${PICO.y1 - PICO.y0}" rx="10" fill="#2e7d32" stroke="#1b5e20" stroke-width="2"/>`);
add(`<rect x="530" y="106" width="40" height="22" rx="4" fill="#b0bec5" stroke="#546e7a"/>`);
text(550, 146, 'USB', { anchor: 'middle', size: 10, fill: '#e8f5e9' });
add(`<rect x="515" y="360" width="70" height="70" rx="4" fill="#212121"/>`);
text(550, 392, 'RP2350', { anchor: 'middle', size: 11, fill: '#eee', bold: true });
text(550, 408, 'Pico 2', { anchor: 'middle', size: 11, fill: '#bbb' });
for (let i = 1; i <= 20; i++) {
  const y = row(i);
  for (const [side, x, name, num] of [['L', PICO.x0, LEFT[i - 1], i], ['R', PICO.x1, RIGHT[i - 1], 41 - i]]) {
    const used = USED.has(side + i);
    add(`<circle cx="${x}" cy="${y}" r="6" fill="${used ? '#ffd54f' : '#e0e0e0'}" stroke="#795548" stroke-width="1.5"/>`);
    const tx = side === 'L' ? x + 11 : x - 11;
    text(tx, y + 4, name, { size: 11, anchor: side === 'L' ? 'start' : 'end', fill: used ? '#ffffff' : '#a5d6a7', bold: used, mono: true });
    text(side === 'L' ? x - 10 : x + 10, y - 7, String(num), { size: 9, anchor: side === 'L' ? 'end' : 'start', fill: '#9e9e9e' });
  }
}

// ---------- modules ----------
board(60, 118, 300, 218, 'Li-ion 1S cell', 'with charger / protection board', '#455a64');
pin(300, 168, '+', 'right'); pin(60, 198, '−', 'left');

board(60, 290, 300, 610, '2.13" e-paper', 'SPI, 122×250, B/W/Y/R');
[['DC', 11], ['CS', 12], ['GND', 13], ['CLK', 14], ['DIN', 15], ['RST', 16], ['BUSY', 17], ['VCC', 18]].forEach(([n, r]) => pin(300, row(r), n, 'right'));

board(700, 280, 860, 400, 'Soil sensor', 'capacitive, analog', '#6d4c41');
[['VCC', 7], ['GND', 8], ['AOUT', 10]].forEach(([n, r]) => pin(700, row(r), n, 'left'));

board(900, 418, 1060, 492, 'Relay module', '', '#37474f');
pin(900, row(12), 'IN', 'left'); pin(900, row(13), 'GND', 'left'); pin(920, 418, 'VCC', 'left');
pin(1060, 440, 'COM', 'right'); pin(1060, 470, 'NO', 'right');

board(700, 476, 860, 650, 'DS3231 RTC', 'I2C, coin cell', '#283593');
[['SCL', 14], ['SDA', 15]].forEach(([n, r]) => pin(700, row(r), n, 'left'));
pin(700, 540, 'VCC', 'left'); pin(700, 566, 'GND', 'left'); pin(700, 592, 'SQW', 'left');

board(1100, 120, 1220, 200, '5 V supply', 'for the pump', '#bf360c');
pin(1120, 120, '+', 'left'); pin(1200, 120, '−', 'left');

add(`<circle cx="1150" cy="560" r="30" fill="#78909c" stroke="#37474f" stroke-width="3"/>`);
text(1150, 565, 'M', { anchor: 'middle', bold: true, size: 18, fill: '#fff' });
text(1150, 610, 'Pump', { anchor: 'middle', size: 12 });

// ---------- legend ----------
const notes = [
  'Wires that cross without a dot are not connected. Dots mark rail connections.',
  'All grounds are common, including the pump supply. Never power the pump from the Pico.',
  'GP29 / ADC3 reads VSYS through the Pico\'s on-board divider (no wire needed).',
  'Relay: active-LOW by default (USE_RELAY_GND_ON). Check it accepts a 3.3 V input.',
];
text(40, 770, notes[0] + '   ' + notes[1], { size: 11, fill: '#555' });
text(40, 785, notes[2] + '   ' + notes[3], { size: 11, fill: '#555' });

const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${W} ${H}" width="${W}" height="${H}" font-family="Segoe UI, Helvetica, Arial, sans-serif">\n${out.join('\n')}\n</svg>\n`;
fs.writeFileSync(path.join(__dirname, 'wiring.svg'), svg);
console.log('wrote docs/wiring.svg');
