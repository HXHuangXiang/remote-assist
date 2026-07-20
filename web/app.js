'use strict';
let ws = null, decoder = null, cfg = null, authed = false;
let canvas, ctx, logEl, statusEl, pwEl;

function log(msg) {
  logEl.textContent = new Date().toLocaleTimeString() + ' ' + msg;
  console.log(msg);
}
function setStatus(s) { statusEl.textContent = s; }

function connect() {
  const host = location.hostname || '127.0.0.1';
  const port = location.port || (location.protocol === 'https:' ? '443' : '7980');
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const url = proto + '://' + host + ':' + port + '/ws';
  log('connecting ' + url);
  if (ws) { ws.close(); ws = null; }
  ws = new WebSocket(url);
  ws.binaryType = 'arraybuffer';
  ws.onopen = function() { ws.send(JSON.stringify({t:'auth', token: pwEl.value || ''})); };
  ws.onmessage = function(ev) {
    if (typeof ev.data === 'string') {
      let m;
      try { m = JSON.parse(ev.data); } catch(e) { return; }
      handleText(m);
    } else {
      handleBinary(new Uint8Array(ev.data));
    }
  };
  ws.onclose = function() { log('disconnected'); setStatus('未连接'); authed = false; if (decoder) { decoder.close(); decoder = null; } };
  ws.onerror = function() { log('ws error'); };
}

function handleText(m) {
  if (m.t === 'auth') {
    authed = m.ok;
    setStatus(m.ok ? '已连接' : '鉴权失败');
    log(m.ok ? 'auth ok' : ('auth fail: ' + (m.reason || '')));
    if (m.ok && canvas) canvas.focus();
    return;
  }
  if (m.t === 'cfg') { setupDecoder(m); return; }
  if (m.t === 'error') { log('error: ' + (m.msg || '')); return; }
}

function setupDecoder(c) {
  cfg = c;
  if (decoder) { decoder.close(); decoder = null; }
  if (typeof VideoDecoder === 'undefined') {
    log('浏览器不支持 WebCodecs,请用 Chrome/Edge 113+');
    return;
  }
  decoder = new VideoDecoder({
    output: function(frame) { ctx.drawImage(frame, 0, 0, canvas.width, canvas.height); frame.close(); },
    error: function(e) { log('decoder error: ' + e); }
  });
  try {
    decoder.configure({ codec: c.codec, width: c.w, height: c.h });
  } catch(e) {
    log('configure failed: ' + e.message + ' (codec=' + c.codec + ')');
    return;
  }
  canvas.width = c.w; canvas.height = c.h;
  log('cfg ' + c.codec + ' ' + c.w + 'x' + c.h + '@' + c.fps);
}

// 扫描 Annex-B 流判断是否含 IDR(NAL type 5),用于 EncodedVideoChunk.type。
function isKeyFrame(data) {
  let i = 0;
  while (i + 4 < data.length) {
    let sc = 0;
    if (data[i] === 0 && data[i+1] === 0 && data[i+2] === 0 && data[i+3] === 1) sc = 4;
    else if (data[i] === 0 && data[i+1] === 0 && data[i+2] === 1) sc = 3;
    if (sc > 0) {
      const nt = data[i+sc] & 0x1F;
      if (nt === 5) return true;
      i += sc + 1;
    } else { i++; }
  }
  return false;
}

function handleBinary(data) {
  if (!decoder || !cfg) return;
  if (decoder.decodeQueueSize > 8) return;  // 简单背压,丢旧帧
  const chunk = new EncodedVideoChunk({
    type: isKeyFrame(data) ? 'key' : 'delta',
    timestamp: Math.floor(performance.now() * 1000),
    data: data
  });
  try { decoder.decode(chunk); } catch(e) { log('decode throw'); }
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN && authed) {
    ws.send(JSON.stringify(obj));
  }
}
function btnName(b) {
  if (b === 1) return 'middle';
  if (b === 2) return 'right';
  return 'left';
}

// KeyboardEvent.code -> Windows Set-1 scancode(DIK)。覆盖常见键,扩展键未标 extended。
const codeToSc = {
  Escape:0x01, Digit1:0x02, Digit2:0x03, Digit3:0x04, Digit4:0x05, Digit5:0x06, Digit6:0x07,
  Digit7:0x08, Digit8:0x09, Digit9:0x0A, Digit0:0x0B, Minus:0x0C, Equal:0x0D, Backspace:0x0E,
  Tab:0x0F, KeyQ:0x10, KeyW:0x11, KeyE:0x12, KeyR:0x13, KeyT:0x14, KeyY:0x15, KeyU:0x16,
  KeyI:0x17, KeyO:0x18, KeyP:0x19, BracketLeft:0x1A, BracketRight:0x1B, Enter:0x1C, ControlLeft:0x1D,
  KeyA:0x1E, KeyS:0x1F, KeyD:0x20, KeyF:0x21, KeyG:0x22, KeyH:0x23, KeyJ:0x24, KeyK:0x25,
  KeyL:0x26, Semicolon:0x27, Quote:0x28, Backquote:0x29, ShiftLeft:0x2A, Backslash:0x2B,
  KeyZ:0x2C, KeyX:0x2D, KeyC:0x2E, KeyV:0x2F, KeyB:0x30, KeyN:0x31, KeyM:0x32, Comma:0x33,
  Period:0x34, Slash:0x35, ShiftRight:0x36, NumpadMultiply:0x37, AltLeft:0x38, Space:0x39,
  CapsLock:0x3A, F1:0x3B, F2:0x3C, F3:0x3D, F4:0x3E, F5:0x3F, F6:0x40, F7:0x41, F8:0x42,
  F9:0x43, F10:0x44, NumLock:0x45, ScrollLock:0x46, Numpad7:0x47, Numpad8:0x48, Numpad9:0x49,
  NumpadSubtract:0x4A, Numpad4:0x4B, Numpad5:0x4C, Numpad6:0x4D, NumpadAdd:0x4E, Numpad1:0x4F,
  Numpad2:0x50, Numpad3:0x51, Numpad0:0x52, NumpadDecimal:0x53,
  ArrowLeft:0x4B, ArrowUp:0x48, ArrowRight:0x4D, ArrowDown:0x50,
  ControlRight:0x1D, AltRight:0x38, Delete:0x53, Insert:0x52, Home:0x47, End:0x4F,
  PageUp:0x49, PageDown:0x51
};

function normXY(e) {
  const r = canvas.getBoundingClientRect();
  return { x: (e.clientX - r.left) / r.width, y: (e.clientY - r.top) / r.height };
}

window.addEventListener('DOMContentLoaded', function() {
  canvas = document.getElementById('screen');
  ctx = canvas.getContext('2d');
  logEl = document.getElementById('log');
  statusEl = document.getElementById('status');
  pwEl = document.getElementById('pw');
  document.getElementById('connect').addEventListener('click', connect);
  pwEl.addEventListener('keydown', function(e) { if (e.key === 'Enter') connect(); });

  window.addEventListener('keydown', function(e) {
    if (!authed) return;
    const sc = codeToSc[e.code];
    if (sc !== undefined) { e.preventDefault(); send({t:'key', sc: sc, down: true}); }
  });
  window.addEventListener('keyup', function(e) {
    if (!authed) return;
    const sc = codeToSc[e.code];
    if (sc !== undefined) { e.preventDefault(); send({t:'key', sc: sc, down: false}); }
  });
  canvas.addEventListener('mousemove', function(e) { if(!authed) return; const p = normXY(e); send({t:'move', x:p.x, y:p.y}); });
  canvas.addEventListener('mousedown', function(e) { if(!authed) return; const p = normXY(e); send({t:'mouse', x:p.x, y:p.y, btn:btnName(e.button), down:true}); e.preventDefault(); });
  canvas.addEventListener('mouseup', function(e) { if(!authed) return; const p = normXY(e); send({t:'mouse', x:p.x, y:p.y, btn:btnName(e.button), down:false}); });
  canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });
  canvas.addEventListener('wheel', function(e) { if(!authed) return; send({t:'wheel', delta: Math.round(e.deltaY)}); e.preventDefault(); });
});

