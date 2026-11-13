// The Controls tab's live stick picture (finding 134): where each stick of the connected controller is
// (the hollow ring) and what the game makes of it with the dead zone chosen (the filled dot), with the
// part of the travel that does nothing shaded. The controller is read through the browser's Gamepad
// API (as pad.js does); the mapping is the game's: port/hle/stick.h (stickByte) and func_00220AE0
// with the action's squaring (gameEffect), so the dot shows the steering the rider gets.
'use strict';

const STICK_FIRST_POS = 178, STICK_FIRST_NEG = 76;

// The setting's value as stick.h reads it: -1 for "game", else the percentage.
function stickDeadZone(value) {
  return /^\d/.test(value) ? Math.min(95, parseInt(value, 10)) : -1;
}

// stick.h's stick_byte: an SDL axis value (-32768..32767) to the game's byte.
function stickByte(v, dead) {
  if (dead < 0) return Math.max(0, Math.min(255, (v >> 8) + 128));
  const mag = Math.min(32767, v >= 0 ? v : -(v + 1));
  const zone = Math.trunc((dead * 32767) / 100);
  if (mag <= zone) return 128;
  const span = 32767 - zone, past = mag - zone;
  if (v > 0) return STICK_FIRST_POS + Math.trunc(((255 - STICK_FIRST_POS) * past + Math.trunc(span / 2)) / span);
  return STICK_FIRST_NEG - Math.trunc((STICK_FIRST_NEG * past + Math.trunc(span / 2)) / span);
}

// What an action makes of a byte while riding: func_00220AE0's curve, then the square (v * v / 127)
// and func_0012DEB0's reading (1 is a digital press: full; more is /128). -1..1.
function gameEffect(b) {
  const f = Math.fround;
  let v;
  if (b >= 226) v = Math.min(127, Math.trunc(f(f(b - 225) * f(2.133333444595337)) + 64));
  else if (b >= 163) v = Math.min(64, b - 162);
  else if (b < 30) v = Math.max(-127, Math.trunc(f(f(b) * f(2.133333444595337)) - 127));
  else if (b < 92) v = Math.max(-64, Math.trunc(f(f(92 - b) * f(-1.0322580337524414))));
  else v = 0;
  if (Math.abs(v) <= 1) v = 0;
  const a = Math.abs(v);
  const s = a >= 127 ? 127 : Math.trunc((a * a) / 127);
  const out = s === 1 ? 1 : s / 128;
  return v < 0 ? -out : out;
}

const STICK_SIZE = 116;

// One stick's box: the travel (-1..1 both ways) as a square, the dead band shaded.
function drawStick(ctx, x0, label, ax, ay, dead, live) {
  const s = STICK_SIZE, h = s / 2, cx = x0 + h, cy = h + 4;
  ctx.fillStyle = '#130e25';
  ctx.beginPath(); ctx.roundRect(x0, 4, s, s, 8); ctx.fill();
  // the travel that does nothing: the first byte that moves anything, as a fraction of the travel
  let zone = 0;
  for (let i = 0; i <= 200; i++) if (gameEffect(stickByte(Math.round((i / 200) * 32767), dead)) === 0) zone = i / 200; else break;
  ctx.fillStyle = 'rgba(240, 124, 58, .16)';
  ctx.fillRect(cx - zone * h, 4, zone * s, s);
  ctx.fillRect(x0, cy - zone * h, s, zone * s);
  ctx.strokeStyle = 'rgba(188, 174, 212, .22)';
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(cx, 8); ctx.lineTo(cx, s); ctx.moveTo(x0 + 4, cy); ctx.lineTo(x0 + s - 4, cy); ctx.stroke();
  ctx.fillStyle = '#9d8fb8';
  ctx.font = '600 11.5px Archivo, sans-serif';
  ctx.textAlign = 'center';
  ctx.fillText(label, cx, s + 20);
  if (!live) return;
  const toV = (a) => Math.round(Math.max(-1, Math.min(1, a)) * (a >= 0 ? 32767 : 32768));
  const rx = gameEffect(stickByte(toV(ax), dead)), ry = gameEffect(stickByte(toV(ay), dead));
  ctx.strokeStyle = '#bcaed4';
  ctx.lineWidth = 2;
  ctx.beginPath(); ctx.arc(cx + ax * (h - 7), cy + ay * (h - 7), 6, 0, Math.PI * 2); ctx.stroke();
  ctx.fillStyle = rx || ry ? '#f07c3a' : '#9d8fb8';
  ctx.beginPath(); ctx.arc(cx + rx * (h - 7), cy + ry * (h - 7), 4.5, 0, Math.PI * 2); ctx.fill();
}

let stickLoop = 0;

// The row's picture: a canvas redrawn every frame while it is on the page.
function stickIndicator(getDeadZone) {
  const canvas = document.createElement('canvas');
  const w = STICK_SIZE * 2 + 28, hgt = STICK_SIZE + 30, ratio = window.devicePixelRatio || 1;
  canvas.width = w * ratio;
  canvas.height = hgt * ratio;
  canvas.style.width = `${w}px`;
  canvas.style.height = `${hgt}px`;
  canvas.className = 'sticks';
  const note = document.createElement('small');
  cancelAnimationFrame(stickLoop);
  const frame = () => {
    if (!canvas.isConnected) return;
    const ctx = canvas.getContext('2d');
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, w, hgt);
    const pad = [...(navigator.getGamepads?.() ?? [])].find(Boolean);
    const dead = stickDeadZone(getDeadZone());
    const a = pad ? pad.axes : [];
    drawStick(ctx, 0, t('stick.left'), a[0] ?? 0, a[1] ?? 0, dead, !!pad);
    drawStick(ctx, STICK_SIZE + 28, t('stick.right'), a[2] ?? 0, a[3] ?? 0, dead, !!pad);
    const text = t(pad ? 'stick.live' : 'stick.none');
    if (note.textContent !== text) note.textContent = text;
    stickLoop = requestAnimationFrame(frame);
  };
  stickLoop = requestAnimationFrame(frame);
  return { canvas, note };
}
