'use strict';
let pendingFrame = null, nextFrameInfo = null, activeFrame = null, imgLoading = false;
let pendingMove = null, moveQueued = false;
let ws = null, cfg = null, authed = false;
let canvas, ctx, logEl, statusEl, pwEl, monSel;
const pressedKeys = new Map();
const pressedButtons = new Set();
let lastPointer = { x: 0.5, y: 0.5 };
let decodeGeneration = 0;
let h264Decoder = null;
const h264Frames = new Map();
let h264NeedsKeyFrame = true;
let lastKeyFrameRequestAt = 0;
let wheelRemainder = 0;

function log(msg) { logEl.textContent = new Date().toLocaleTimeString() + ' ' + msg; console.log(msg); }
function setStatus(s) { statusEl.textContent = s; }

function fitCanvas() {
  if (!canvas.width || !canvas.height) return;
  const wrap = document.getElementById('screen-wrap');
  const cr = canvas.width / canvas.height;
  const wr = wrap.clientWidth / wrap.clientHeight;
  if (wr > cr) { canvas.style.height = wrap.clientHeight + 'px'; canvas.style.width = (wrap.clientHeight * cr) + 'px'; }
  else { canvas.style.width = wrap.clientWidth + 'px'; canvas.style.height = (wrap.clientWidth / cr) + 'px'; }
}

function connect() {
  // 直接从当前页面地址派生，端口改成 80/443 等默认端口时也不会错误回退到 7980。
  const endpoint = new URL('/ws', window.location.href);
  endpoint.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const url = endpoint.href;
  log('connecting ' + url);
  releasePressedInputs();
  resetFramePipeline(false);
  authed = false;
  setStatus('连接中');
  if (ws) { ws.close(); }
  const socket = new WebSocket(url);
  ws = socket;
  socket.binaryType = 'arraybuffer';
  socket.onopen = function() {
    if (ws === socket) socket.send(JSON.stringify({t:'auth', token: pwEl.value || ''}));
  };
  socket.onmessage = function(ev) {
    if (ws !== socket) return;
    if (typeof ev.data === 'string') {
      let m; try { m = JSON.parse(ev.data); } catch(e) { return; }
      handleText(m);
    } else { handleBinary(new Uint8Array(ev.data)); }
  };
  socket.onclose = function() {
    if (ws !== socket) return;
    resetFramePipeline(false);
    pressedKeys.clear();
    pressedButtons.clear();
    ws = null;
    log('disconnected'); setStatus('未连接'); authed = false;
  };
  socket.onerror = function() { if (ws === socket) log('ws error'); };
}

function handleText(m) {
  if (m.t === 'auth') {
    authed = m.ok;
    setStatus(m.ok ? '已连接' : '鉴权失败');
    log(m.ok ? 'auth ok' : ('auth fail: ' + (m.reason || '')));
    if (m.ok && canvas) canvas.focus();
    return;
  }
  if (m.t === 'cfg') { setupCfg(m); return; }
  if (m.t === 'frame') {
    const id = Number(m.id), streamId = Number(m.stream_id), timestamp = Number(m.ts);
    if (Number.isSafeInteger(id) && id > 0 && Number.isSafeInteger(streamId) && streamId >= 0 &&
        Number.isSafeInteger(timestamp) && timestamp >= 0) {
      nextFrameInfo = { id:id, streamId:streamId, timestamp:timestamp, key:m.key === true };
    } else {
      nextFrameInfo = null;
    }
    return;
  }
  if (m.t === 'monitors') { populateMonitors(m.list); return; }
  if (m.t === 'error') { log('error: ' + (m.msg || '')); return; }
}

function setupCfg(c) {
  const streamId = Number(c.stream_id);
  c.stream_id = Number.isSafeInteger(streamId) && streamId >= 0 ? streamId : 0;
  if (!cfg || cfg.stream_id !== c.stream_id) resetFramePipeline(true);
  cfg = c;
    canvas.width = c.w; canvas.height = c.h;
    fitCanvas();
    if (c.monitors) populateMonitors(c.monitors, c.selected_monitor);
    if (isH264Codec()) setupH264Decoder(c);
    log('cfg ' + c.codec + ' ' + c.w + 'x' + c.h + '@' + c.fps);
}

function populateMonitors(list, selectedMonitor) {
  monSel.innerHTML = '';
  const all = document.createElement('option');
  all.value = '-1'; all.textContent = '全部屏幕';
  monSel.appendChild(all);
  (list || []).forEach(function(m) {
    const o = document.createElement('option');
    o.value = m.index; o.textContent = m.name + ' (' + m.w + 'x' + m.h + ')';
    monSel.appendChild(o);
  });
  monSel.value = String(Number.isInteger(selectedMonitor) ? selectedMonitor : -1);
}

function acknowledgeFrame(id) {
  send({t:'ack', id:id});
}

function acknowledgeH264Frames() {
  h264Frames.forEach(function(infos) {
    infos.forEach(function(info) { if (info.socket === ws) acknowledgeFrame(info.id); });
  });
}

function requestKeyFrame() {
  if (!authed) return;
  const now = performance.now();
  // 防止损坏连接或不兼容浏览器每个增量帧都触发一次控制报文。
  if (now - lastKeyFrameRequestAt < 250) return;
  lastKeyFrameRequestAt = now;
  send({t:'keyframe'});
}

function isH264Codec() {
  return cfg && typeof cfg.codec === 'string' && cfg.codec.toLowerCase().indexOf('avc1.') === 0;
}

function setupH264Decoder(c) {
  if (!window.VideoDecoder || !window.EncodedVideoChunk) {
    setStatus('浏览器不支持 H.264 WebCodecs');
    log('H.264 WebCodecs unavailable');
    return false;
  }
  if (h264Decoder) {
    h264Decoder.close();
    h264Decoder = null;
  }
  const streamId = c.stream_id;
  h264NeedsKeyFrame = true;
  const decoder = new VideoDecoder({
    output: function(videoFrame) {
      const infos = h264Frames.get(videoFrame.timestamp);
      const info = infos && infos.shift();
      if (infos && infos.length === 0) h264Frames.delete(videoFrame.timestamp);
      if (info && h264Decoder === decoder && cfg && cfg.stream_id === streamId && ws === info.socket) {
        ctx.drawImage(videoFrame, 0, 0, canvas.width, canvas.height);
        acknowledgeFrame(info.id);
      }
      videoFrame.close();
    },
    error: function(error) {
      if (h264Decoder !== decoder) return;
      log('H.264 decode error: ' + error.message);
      setStatus('H.264 解码失败');
      acknowledgeH264Frames();
      h264Frames.clear();
      h264NeedsKeyFrame = true;
      h264Decoder = null;
      try { decoder.close(); } catch (_) {}
      if (cfg && isH264Codec()) setupH264Decoder(cfg);
      requestKeyFrame();
    }
  });
  const decoderConfig = {
    codec: c.codec,
    codedWidth: c.w,
    codedHeight: c.h,
    optimizeForLatency: true,
    hardwareAcceleration: 'prefer-hardware'
  };
  if (c.annexb) decoderConfig.avc = { format: 'annexb' };
  try {
    decoder.configure(decoderConfig);
  } catch (error) {
    decoder.close();
    setStatus('H.264 解码器初始化失败');
    log('H.264 decoder config error: ' + error.message);
    return false;
  }
  h264Decoder = decoder;
  requestKeyFrame();
  return true;
}

function resetFramePipeline(ackActive) {
  decodeGeneration++;
  if (ackActive && activeFrame) acknowledgeFrame(activeFrame.id);
  if (ackActive && pendingFrame) acknowledgeFrame(pendingFrame.id);
  if (ackActive) {
    acknowledgeH264Frames();
  }
  h264Frames.clear();
  h264NeedsKeyFrame = true;
  if (h264Decoder) {
    h264Decoder.close();
    h264Decoder = null;
  }
  pendingFrame = null;
  nextFrameInfo = null;
  activeFrame = null;
  imgLoading = false;
}

function handleBinary(data) {
  const info = nextFrameInfo;
  nextFrameInfo = null;
  if (!cfg || !info) return;
  if (info.streamId !== cfg.stream_id) {
    acknowledgeFrame(info.id);
    return;
  }
  if (isH264Codec()) {
    if (!h264Decoder) {
      acknowledgeFrame(info.id);
      return;
    }
    if (h264NeedsKeyFrame && !info.key) {
      acknowledgeFrame(info.id);
      requestKeyFrame();
      return;
    }
    if (h264Decoder.decodeQueueSize > 2) {
      // 丢掉一个 delta 后，后续参考帧不再可靠；直接回到下一张 IDR，保持低延迟。
      acknowledgeFrame(info.id);
      h264NeedsKeyFrame = true;
      requestKeyFrame();
      return;
    }
    const socket = ws;
    const infos = h264Frames.get(info.timestamp) || [];
    const pending = { id:info.id, socket:socket };
    infos.push(pending);
    h264Frames.set(info.timestamp, infos);
    try {
      h264Decoder.decode(new EncodedVideoChunk({
        type: info.key ? 'key' : 'delta',
        timestamp: info.timestamp,
        data: data
      }));
      if (info.key) h264NeedsKeyFrame = false;
    } catch (error) {
      const pendingIndex = infos.lastIndexOf(pending);
      if (pendingIndex >= 0) infos.splice(pendingIndex, 1);
      if (infos.length === 0) h264Frames.delete(info.timestamp);
      acknowledgeFrame(info.id);
      h264NeedsKeyFrame = true;
      requestKeyFrame();
      log('H.264 chunk rejected: ' + error.message);
    }
    return;
  }
  pendingFrame = { data:data, id:info.id, streamId:info.streamId };
  if (!imgLoading) drawNext();
}
function drawNext() {
  if (!pendingFrame) return;
  const frame = pendingFrame;
  pendingFrame = null;
  if (!cfg || frame.streamId !== cfg.stream_id) {
    acknowledgeFrame(frame.id);
    drawNext();
    return;
  }
  imgLoading = true;
  activeFrame = frame;
  const generation = decodeGeneration;
  const socket = ws;
  const blob = new Blob([frame.data], { type: 'image/jpeg' });
  const url = URL.createObjectURL(blob);
  const img = new Image();
  img.onload = function() {
    URL.revokeObjectURL(url);
    if (generation !== decodeGeneration || ws !== socket) return;
    if (cfg && frame.streamId === cfg.stream_id) {
      ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
      // 图片已绘制到 canvas，释放服务端的唯一在途帧配额。
      acknowledgeFrame(frame.id);
    }
    activeFrame = null;
    imgLoading = false;
    drawNext();
  };
  img.onerror = function() {
    URL.revokeObjectURL(url);
    if (generation !== decodeGeneration || ws !== socket) return;
    acknowledgeFrame(frame.id);
    activeFrame = null;
    imgLoading = false;
    drawNext();
  };
  img.src = url;
}

function send(obj) { if (ws && ws.readyState === WebSocket.OPEN && authed) ws.send(JSON.stringify(obj)); }
function btnName(b) { return b === 1 ? 'middle' : b === 2 ? 'right' : 'left'; }

function normalizedWheelDelta(e) {
  // 浏览器的 deltaY 可能是像素、行或页，Windows SendInput 则使用 WHEEL_DELTA
  // (120) 单位。累积精细触控板的零散像素增量，并反向匹配浏览器“向下为正”。
  let scale = 120 / 100;
  if (e.deltaMode === WheelEvent.DOM_DELTA_LINE) scale = 120;
  else if (e.deltaMode === WheelEvent.DOM_DELTA_PAGE) scale = 360;
  wheelRemainder += -e.deltaY * scale;
  const requested = Math.trunc(wheelRemainder);
  if (requested === 0) return 0;
  const delta = Math.max(-1200, Math.min(1200, requested));
  wheelRemainder -= delta;
  return delta;
}

function queueMove(e) {
  pendingMove = normXY(e);
  lastPointer = pendingMove;
  if (moveQueued) return;
  moveQueued = true;
  requestAnimationFrame(function() {
    moveQueued = false;
    if (pendingMove) { send({t:'move', x:pendingMove.x, y:pendingMove.y}); pendingMove = null; }
  });
}

function releasePressedInputs() {
  if (!authed) return;
  pressedKeys.forEach(function(v) {
    send({t:'key', sc:v.sc, ext:v.ext, down:false});
  });
  pressedKeys.clear();
  pressedButtons.forEach(function(button) {
    send({t:'mouse', x:lastPointer.x, y:lastPointer.y, btn:button, down:false});
  });
  pressedButtons.clear();
}

const codeToSc = {
  Escape:0x01, Digit1:0x02, Digit2:0x03, Digit3:0x04, Digit4:0x05, Digit5:0x06, Digit6:0x07,
  Digit7:0x08, Digit8:0x09, Digit9:0x0A, Digit0:0x0B, Minus:0x0C, Equal:0x0D, Backspace:0x0E, Tab:0x0F,
  KeyQ:0x10, KeyW:0x11, KeyE:0x12, KeyR:0x13, KeyT:0x14, KeyY:0x15, KeyU:0x16, KeyI:0x17, KeyO:0x18,
  KeyP:0x19, BracketLeft:0x1A, BracketRight:0x1B, Enter:0x1C, ControlLeft:0x1D, KeyA:0x1E, KeyS:0x1F,
  KeyD:0x20, KeyF:0x21, KeyG:0x22, KeyH:0x23, KeyJ:0x24, KeyK:0x25, KeyL:0x26, Semicolon:0x27,
  Quote:0x28, Backquote:0x29, ShiftLeft:0x2A, Backslash:0x2B, KeyZ:0x2C, KeyX:0x2D, KeyC:0x2E,
  KeyV:0x2F, KeyB:0x30, KeyN:0x31, KeyM:0x32, Comma:0x33, Period:0x34, Slash:0x35, ShiftRight:0x36,
  NumpadMultiply:0x37, AltLeft:0x38, Space:0x39, CapsLock:0x3A, F1:0x3B, F2:0x3C, F3:0x3D, F4:0x3E,
  F5:0x3F, F6:0x40, F7:0x41, F8:0x42, F9:0x43, F10:0x44, NumLock:0x45, ScrollLock:0x46,
  Numpad7:0x47, Numpad8:0x48, Numpad9:0x49, NumpadSubtract:0x4A, Numpad4:0x4B, Numpad5:0x4C,
  Numpad6:0x4D, NumpadAdd:0x4E, Numpad1:0x4F, Numpad2:0x50, Numpad3:0x51, Numpad0:0x52, NumpadDecimal:0x53,
  ArrowLeft:0x4B, ArrowUp:0x48, ArrowRight:0x4D, ArrowDown:0x50,
  F11:0x57, F12:0x58, NumpadEnter:0x1C, NumpadDivide:0x35,
  ControlRight:0x1D, AltRight:0x38, Delete:0x53, Insert:0x52, Home:0x47, End:0x4F, PageUp:0x49, PageDown:0x51,
  MetaLeft:0x5B, MetaRight:0x5C, ContextMenu:0x5D
};
const extendedKeys = new Set([
  'ControlRight', 'AltRight', 'ArrowLeft', 'ArrowUp', 'ArrowRight', 'ArrowDown',
  'Delete', 'Insert', 'Home', 'End', 'PageUp', 'PageDown', 'NumpadEnter', 'NumpadDivide',
  'MetaLeft', 'MetaRight', 'ContextMenu'
]);

function normXY(e) {
  const r = canvas.getBoundingClientRect();
  if (!r.width || !r.height) return lastPointer;
  return {
    x: Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)),
    y: Math.max(0, Math.min(1, (e.clientY - r.top) / r.height))
  };
}

window.addEventListener('DOMContentLoaded', function() {
  canvas = document.getElementById('screen');
  // desynchronized 是 Chromium 的低延迟呈现提示，可减少 VideoFrame/JPEG 绘制
  // 等待合成器的机会；不支持时浏览器会忽略选项，后备上下文保证兼容性。
  ctx = canvas.getContext('2d', { alpha:false, desynchronized:true }) || canvas.getContext('2d');
  logEl = document.getElementById('log');
  statusEl = document.getElementById('status');
  pwEl = document.getElementById('pw');
  monSel = document.getElementById('monitor');
  document.getElementById('connect').addEventListener('click', connect);
  pwEl.addEventListener('keydown', function(e) { if (e.key === 'Enter') connect(); });
  monSel.addEventListener('change', function() { send({t:'monitor', index: parseInt(monSel.value, 10)}); });
  window.addEventListener('resize', fitCanvas);
  window.addEventListener('keydown', function(e) {
    if (!authed) return;
    const sc = codeToSc[e.code];
    if (sc !== undefined) {
      e.preventDefault();
      if (!pressedKeys.has(e.code)) {
        const key = {sc:sc, ext:extendedKeys.has(e.code)};
        pressedKeys.set(e.code, key);
        send({t:'key', sc:key.sc, ext:key.ext, down:true});
      }
    }
  });
  window.addEventListener('keyup', function(e) {
    if (!authed) return;
    const sc = codeToSc[e.code];
    if (sc !== undefined) {
      e.preventDefault();
      const key = pressedKeys.get(e.code);
      if (key) {
        pressedKeys.delete(e.code);
        send({t:'key', sc:key.sc, ext:key.ext, down:false});
      }
    }
  });
  canvas.addEventListener('mousemove', function(e) { if(!authed) return; queueMove(e); });
  canvas.addEventListener('mousedown', function(e) {
    if (!authed) return;
    const p = normXY(e);
    const button = btnName(e.button);
    lastPointer = p;
    pressedButtons.add(button);
    canvas.focus();
    send({t:'mouse', x:p.x, y:p.y, btn:button, down:true});
    e.preventDefault();
  });
  window.addEventListener('mouseup', function(e) {
    if (!authed) return;
    const button = btnName(e.button);
    if (pressedButtons.delete(button)) {
      send({t:'mouse', x:lastPointer.x, y:lastPointer.y, btn:button, down:false});
    }
  });
  canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });
  canvas.addEventListener('wheel', function(e) {
    if (!authed) return;
    const delta = normalizedWheelDelta(e);
    if (delta) send({t:'wheel', delta:delta});
    e.preventDefault();
  }, { passive:false });
  window.addEventListener('blur', releasePressedInputs);
  document.addEventListener('visibilitychange', function() {
    if (document.hidden) releasePressedInputs();
  });
});
