'use strict';
let pendingFrame = null, nextFrameInfo = null, activeFrame = null, imgLoading = false;
let pendingMove = null, moveQueued = false;
let ws = null, cfg = null, authed = false;
// 重复点击“连接”时，必须先等旧 WebSocket 的关闭握手完成；否则服务端仍持有
// 上一控制端槽位，新连接会被误判为并发控制端而拒绝。
let reconnectAfterClose = false;
let reconnectRetriesRemaining = 0;
let canvas, ctx, cursorCanvas, cursorCtx, logEl, statusEl, pwEl, monSel, qualitySel;
let patchThresholdEl, patchThresholdValueEl;
const pressedKeys = new Map();
const pressedButtons = new Set();
let lastPointer = { x: 0.5, y: 0.5 };
let decodeGeneration = 0;
let h264Decoder = null;
const h264Frames = new Map();
let pendingH264Presentation = null, h264DrawQueued = false, h264DrawRequestId = 0;
let h264NeedsKeyFrame = true;
let lastKeyFrameRequestAt = 0;
let wheelRemainder = 0;
// 键盘和鼠标按键绝不能因网络背压被丢弃；高频 move/wheel 则只保留最新状态，
// 避免浏览器的 WebSocket 发送缓冲区堆满后，用户已经停手但远端鼠标仍持续移动。
const inputBufferHighWaterMark = 16 * 1024;
const controllerCleanupReconnectAttempts = 20;
let pendingWheelDelta = 0, wheelQueued = false;
const clientStats = {
  drawn: 0, dropped: 0, drawMsTotal: 0, drawMsSamples: 0,
  decodeMsTotal: 0, decodeMsSamples: 0,
  decodeErrors: 0, maxDecodeQueue: 0, maxWsBuffered: 0
};
let remoteCursor = { visible:false, x:0, y:0 };
let firstFrameWarningTimer = 0, awaitingFrameSocket = null;
let activePatch = null, nextTileInfo = null, patchDrawing = false;
const qualityStorageKey = 'remote-assist.stream-quality';
const patchThresholdStorageKey = 'remote-assist.patch-threshold';
// 鉴权成功只代表控制信道可用。必须等解码后的帧实际绘制到 canvas，才能确认视频
// 通路正常；否则损坏的 H.264/JPEG 负载会把黑屏误报成“已连接”。
let firstFramePresented = false;
// 远端指针移动非常频繁。记录上一次实际绘制的边界，只清除箭头附近的小区域，
// 避免每个 cursor 消息都 clearRect 整张 2K/4K 覆盖 canvas。
let drawnCursorBounds = null;

function log(msg) { logEl.textContent = new Date().toLocaleTimeString() + ' ' + msg; console.log(msg); }
function setStatus(s) { statusEl.textContent = s; }

function normalizePatchThreshold(value) {
  const number = Number(value);
  if (!Number.isInteger(number) || number < 10 || number > 90 || number % 5 !== 0) return 50;
  return number;
}

function updatePatchThresholdLabel() {
  if (patchThresholdEl && patchThresholdValueEl) {
    patchThresholdValueEl.textContent = normalizePatchThreshold(patchThresholdEl.value) + '%';
  }
}

function loadStreamPreferences() {
  if (!qualitySel || !patchThresholdEl) return;
  try {
    const quality = localStorage.getItem(qualityStorageKey);
    if (quality && Array.prototype.some.call(qualitySel.options, function(option) {
      return option.value === quality;
    })) qualitySel.value = quality;
    patchThresholdEl.value = String(normalizePatchThreshold(localStorage.getItem(patchThresholdStorageKey)));
  } catch (_) {
    patchThresholdEl.value = '50';
  }
  updatePatchThresholdLabel();
}

function sendStreamPreferences() {
  if (!qualitySel || !patchThresholdEl) return;
  const threshold = normalizePatchThreshold(patchThresholdEl.value);
  patchThresholdEl.value = String(threshold);
  updatePatchThresholdLabel();
  send({t:'stream', quality:qualitySel.value || 'auto', patch_threshold:threshold, patches:true});
}

function saveAndSendStreamPreferences() {
  if (!qualitySel || !patchThresholdEl) return;
  const threshold = normalizePatchThreshold(patchThresholdEl.value);
  patchThresholdEl.value = String(threshold);
  try {
    localStorage.setItem(qualityStorageKey, qualitySel.value || 'auto');
    localStorage.setItem(patchThresholdStorageKey, String(threshold));
  } catch (_) {}
  sendStreamPreferences();
}

function clearFirstFrameWarning() {
  if (firstFrameWarningTimer) window.clearTimeout(firstFrameWarningTimer);
  firstFrameWarningTimer = 0;
  awaitingFrameSocket = null;
}

function scheduleFirstFrameWarning(socket) {
  clearFirstFrameWarning();
  firstFramePresented = false;
  awaitingFrameSocket = socket;
  firstFrameWarningTimer = window.setTimeout(function() {
    if (ws === socket && authed && awaitingFrameSocket === socket) {
      setStatus('未收到首帧，请查看被控机 logs/agent.log');
      log('first video frame timeout');
    }
  }, 6000);
}

function markFirstFramePresented(socket) {
  if (firstFramePresented || ws !== socket || !authed) return;
  firstFramePresented = true;
  clearFirstFrameWarning();
  setStatus('已连接');
  log('first video frame presented');
}

function fitCanvas() {
  if (!canvas.width || !canvas.height) return;
  const wrap = document.getElementById('screen-wrap');
  const cr = canvas.width / canvas.height;
  const wr = wrap.clientWidth / wrap.clientHeight;
  if (wr > cr) { canvas.style.height = wrap.clientHeight + 'px'; canvas.style.width = (wrap.clientHeight * cr) + 'px'; }
  else { canvas.style.width = wrap.clientWidth + 'px'; canvas.style.height = (wrap.clientWidth / cr) + 'px'; }
}

function connect() {
  if (ws && ws.readyState !== WebSocket.CLOSED) {
    reconnectAfterClose = true;
    reconnectRetriesRemaining = 5;
    setStatus('正在重连');
    releasePressedInputs();
    try { ws.close(1000, 'reconnecting'); } catch (_) {}
    return;
  }
  // 用户明确发起的新连接不继承上一轮自动重连的重试次数。
  reconnectAfterClose = false;
  reconnectRetriesRemaining = 0;
  openConnection();
}

function openConnection() {
  // 延迟重连计时器与用户再次点击可能交错；已有新会话时不能由旧计时器把它关闭。
  if (ws && ws.readyState !== WebSocket.CLOSED) return;
  // 直接从当前页面地址派生，端口改成 80/443 等默认端口时也不会错误回退到 7980。
  const endpoint = new URL('/ws', window.location.href);
  endpoint.protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const url = endpoint.href;
  log('connecting ' + url);
  releasePressedInputs();
  clearFirstFrameWarning();
  firstFramePresented = false;
  cfg = null;
  resetFramePipeline(false);
  pendingMove = null;
  pendingWheelDelta = 0;
  wheelRemainder = 0;
  authed = false;
  updateKeyboardFocusIndicator();
  setStatus('连接中');
  const socket = new WebSocket(url);
  ws = socket;
  socket.binaryType = 'arraybuffer';
  socket.onopen = function() {
    if (ws !== socket) return;
    try {
      socket.send(JSON.stringify({t:'auth', token: pwEl.value || ''}));
    } catch (error) {
      log('auth send failed: ' + error.message);
      try { socket.close(); } catch (_) {}
    }
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
    const shouldReconnect = reconnectAfterClose || reconnectRetriesRemaining > 0;
    reconnectAfterClose = false;
    resetFramePipeline(false);
    clearFirstFrameWarning();
    firstFramePresented = false;
    cfg = null;
    pressedKeys.clear();
    pressedButtons.clear();
    pendingMove = null;
    pendingWheelDelta = 0;
    wheelRemainder = 0;
    remoteCursor.visible = false;
    drawRemoteCursor();
    ws = null;
    log('disconnected'); setStatus('未连接'); authed = false;
    updateKeyboardFocusIndicator();
    if (shouldReconnect) {
      if (reconnectRetriesRemaining > 0) --reconnectRetriesRemaining;
      // 服务端会先释放遗留按键再让出唯一 controller 槽位；给这段收尾留出一个
      // 短窗口。若仍碰到“controller already connected”，最多再重试五次。
      window.setTimeout(openConnection, 250);
    }
  };
  socket.onerror = function() { if (ws === socket) log('ws error'); };
}

function handleText(m) {
  if (m.t === 'auth') {
    authed = m.ok;
    if (m.ok) reconnectRetriesRemaining = 0;
    if (m.ok) {
      setStatus('已连接，等待远端首帧');
      scheduleFirstFrameWarning(ws);
      // 只有新页面主动声明图块能力才会触发局部协议；旧缓存页面不会发送该消息，
      // 服务端将继续使用兼容的整帧传输。
      sendStreamPreferences();
    } else {
      clearFirstFrameWarning();
      setStatus('鉴权失败');
    }
    log(m.ok ? 'auth ok' : ('auth fail: ' + (m.reason || '')));
    if (m.ok && canvas) {
      canvas.focus();
      updateKeyboardFocusIndicator();
    }
    return;
  }
  if (m.t === 'cfg') { setupCfg(m); return; }
  if (m.t === 'cursor') { updateRemoteCursor(m); return; }
  if (m.t === 'patch') {
    const id = Number(m.id), streamId = Number(m.stream_id), count = Number(m.count);
    if (!Number.isSafeInteger(id) || id <= 0 || !Number.isSafeInteger(streamId) ||
        streamId < 0 || !Number.isSafeInteger(count) || count < 1 || count > 1024 ||
        !cfg || streamId !== cfg.stream_id || activePatch || nextTileInfo) {
      requestKeyFrame();
      return;
    }
    activePatch = { id:id, streamId:streamId, count:count, received:0, parts:[],
      socket:ws, receivedAt:performance.now(), generation:decodeGeneration };
    return;
  }
  if (m.t === 'tile') {
    const id = Number(m.id), index = Number(m.i), x = Number(m.x), y = Number(m.y);
    const width = Number(m.w), height = Number(m.h);
    if (!activePatch || activePatch.socket !== ws || id !== activePatch.id ||
        index !== activePatch.received || nextTileInfo || !Number.isSafeInteger(x) ||
        !Number.isSafeInteger(y) || !Number.isSafeInteger(width) ||
        !Number.isSafeInteger(height) || width < 1 || height < 1 || x < 0 || y < 0 ||
        !cfg || x + width > canvas.width || y + height > canvas.height) {
      failActivePatch('invalid patch tile metadata');
      return;
    }
    nextTileInfo = { x:x, y:y, width:width, height:height, patch:activePatch };
    return;
  }
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
  if (m.t === 'error') {
    log('error: ' + (m.msg || ''));
    // 旧控制端断连时服务端会先在当前桌面释放 keyup/mouseup。这个短窗口不能让
    // 新控制端抢入，否则会误抬起新会话按下的键；浏览器识别专用错误后自动重试。
    if (m.code === 'controller_cleanup' && ws) {
      reconnectAfterClose = true;
      reconnectRetriesRemaining = Math.max(reconnectRetriesRemaining,
                                           controllerCleanupReconnectAttempts);
      setStatus('正在等待上一控制端释放输入');
    }
    return;
  }
}

function setupCfg(c) {
  const streamId = Number(c.stream_id);
  c.stream_id = Number.isSafeInteger(streamId) && streamId >= 0 ? streamId : 0;
  const width = Number(c.w), height = Number(c.h);
  const hasVideoSize = Number.isSafeInteger(width) && Number.isSafeInteger(height) &&
    width >= 2 && height >= 2;
  c.w = hasVideoSize ? width : 0;
  c.h = hasVideoSize ? height : 0;
  if (!hasVideoSize) {
    cfg = c;
    if (c.monitors) populateMonitors(c.monitors, c.selected_monitor);
    log('cfg awaiting first frame');
    return;
  }
  // 显示器列表和选中项也会下发 cfg，但它们不会改变已在解码的 H.264 码流。
  // 仅当真正的视频协商参数变化时才销毁解码器；否则热插拔/布局刷新会无谓地
  // 清空参考帧、请求 IDR，并让控制端出现一次短暂黑屏。
  const videoConfigChanged = !cfg || cfg.stream_id !== c.stream_id ||
    cfg.codec !== c.codec || cfg.annexb !== c.annexb ||
    cfg.w !== c.w || cfg.h !== c.h;
  const sizeChanged = !cfg || cfg.w !== c.w || cfg.h !== c.h;
  if (videoConfigChanged) resetFramePipeline(true);
  cfg = c;
  setStatus('已连接，等待远端首帧');
  if (sizeChanged) {
    canvas.width = c.w;
    canvas.height = c.h;
    cursorCanvas.width = c.w;
    cursorCanvas.height = c.h;
    fitCanvas();
  }
  drawRemoteCursor();
  if (c.monitors) populateMonitors(c.monitors, c.selected_monitor);
  if (videoConfigChanged && isH264Codec()) setupH264Decoder(c);
  log('cfg ' + c.codec + ' ' + c.w + 'x' + c.h + '@' + c.fps +
    (videoConfigChanged ? ' video-reset' : ' topology-only'));
}

function updateRemoteCursor(m) {
  const visible = m.visible === true;
  if (!visible) {
    remoteCursor.visible = false;
    drawRemoteCursor();
    return;
  }
  const x = Number(m.x), y = Number(m.y);
  if (!Number.isFinite(x) || !Number.isFinite(y)) return;
  remoteCursor = {
    visible:true,
    x:Math.max(0, Math.min(1, x)),
    y:Math.max(0, Math.min(1, y))
  };
  drawRemoteCursor();
}

function drawRemoteCursor() {
  if (!cursorCanvas || !cursorCtx) return;
  if (drawnCursorBounds) {
    cursorCtx.clearRect(drawnCursorBounds.x, drawnCursorBounds.y,
      drawnCursorBounds.w, drawnCursorBounds.h);
    drawnCursorBounds = null;
  }
  canvas.style.cursor = remoteCursor.visible ? 'none' : 'default';
  if (!remoteCursor.visible || !cursorCanvas.width || !cursorCanvas.height) return;

  const x = remoteCursor.x * Math.max(0, cursorCanvas.width - 1);
  const y = remoteCursor.y * Math.max(0, cursorCanvas.height - 1);
  const size = Math.max(14, Math.min(30,
    Math.round(Math.min(cursorCanvas.width, cursorCanvas.height) / 36)));
  const lineWidth = Math.max(1, size / 12);
  // 箭头底端会到 size * 1.12，描边还会向外扩展半个 lineWidth。额外留两像素，
  // 保证抗锯齿边缘也被擦除，且坐标裁剪由 canvas 自动完成。
  const padding = Math.ceil(lineWidth / 2) + 2;
  drawnCursorBounds = {
    x:x - padding,
    y:y - padding,
    w:size + padding * 2,
    h:Math.ceil(size * 1.12) + padding * 2
  };
  cursorCtx.save();
  cursorCtx.translate(x, y);
  cursorCtx.lineJoin = 'round';
  cursorCtx.lineCap = 'round';
  cursorCtx.beginPath();
  cursorCtx.moveTo(0, 0);
  cursorCtx.lineTo(0, size);
  cursorCtx.lineTo(size * 0.28, size * 0.72);
  cursorCtx.lineTo(size * 0.52, size * 1.12);
  cursorCtx.lineTo(size * 0.70, size);
  cursorCtx.lineTo(size * 0.45, size * 0.60);
  cursorCtx.lineTo(size, size * 0.60);
  cursorCtx.closePath();
  cursorCtx.fillStyle = '#fff';
  cursorCtx.strokeStyle = '#000';
  cursorCtx.lineWidth = lineWidth;
  cursorCtx.stroke();
  cursorCtx.fill();
  cursorCtx.restore();
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

function acknowledgeFrameForSocket(info) {
  if (info && info.socket === ws) acknowledgeFrame(info.id);
}

function acknowledgeH264Frames() {
  h264Frames.forEach(function(infos) {
    infos.forEach(acknowledgeFrameForSocket);
  });
}

// VideoDecoder 可能在同一次浏览器刷新前连续输出多帧。只展示最新帧，既减少重复
// drawImage，又不会跳过编码参考帧；被覆盖的帧会立即 ACK，释放服务端有限发送窗口。
function queueH264Presentation(videoFrame, info, streamId, decoder) {
  if (pendingH264Presentation) {
    acknowledgeFrameForSocket(pendingH264Presentation.info);
    clientStats.dropped++;
    pendingH264Presentation.videoFrame.close();
  }
  pendingH264Presentation = {
    videoFrame:videoFrame, info:info, streamId:streamId,
    decoder:decoder, generation:decodeGeneration
  };
  if (h264DrawQueued) return;

  h264DrawQueued = true;
  const requestId = ++h264DrawRequestId;
  requestAnimationFrame(function() {
    // resetFramePipeline 或解码错误后旧的 rAF 不得消费新流的待绘制帧。
    if (requestId !== h264DrawRequestId) return;
    h264DrawQueued = false;
    const presentation = pendingH264Presentation;
    pendingH264Presentation = null;
    if (!presentation) return;

    const valid = presentation.generation === decodeGeneration &&
      h264Decoder === presentation.decoder && cfg &&
      cfg.stream_id === presentation.streamId && ws === presentation.info.socket;
    if (valid) {
      try {
        ctx.drawImage(presentation.videoFrame, 0, 0, canvas.width, canvas.height);
        recordPresentedFrame(presentation.info);
        markFirstFramePresented(presentation.info.socket);
      } catch (error) {
        log('H.264 draw error: ' + error.message);
      }
      acknowledgeFrameForSocket(presentation.info);
    }
    presentation.videoFrame.close();
  });
}

function discardPendingH264Presentation(ackActive) {
  if (pendingH264Presentation) {
    if (ackActive) acknowledgeFrameForSocket(pendingH264Presentation.info);
    clientStats.dropped++;
    pendingH264Presentation.videoFrame.close();
    pendingH264Presentation = null;
  }
  // 已经预约的 rAF 会因 request id 不匹配安全退出。
  h264DrawRequestId++;
  h264DrawQueued = false;
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

// 丢掉任意 H.264 delta 后，旧 decoder 内仍可能排队着依赖该 delta 的访问单元。
// 仅设置 h264NeedsKeyFrame 不足以隔离这些旧参考帧：关闭并重建 decoder，同时确认
// 已登记但不会再展示的服务端帧，下一张 IDR 才会成为新的独立解码起点。
function resetH264DecoderForKeyFrame(reason) {
  discardPendingH264Presentation(true);
  acknowledgeH264Frames();
  h264Frames.clear();
  h264NeedsKeyFrame = true;
  if (cfg && isH264Codec()) {
    setupH264Decoder(cfg);
  } else {
    requestKeyFrame();
  }
  if (reason) log(reason);
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
        if (Number.isFinite(info.receivedAt)) {
          clientStats.decodeMsTotal += Math.max(0, performance.now() - info.receivedAt);
          clientStats.decodeMsSamples++;
        }
        queueH264Presentation(videoFrame, info, streamId, decoder);
        return;
      }
      videoFrame.close();
    },
    error: function(error) {
      if (h264Decoder !== decoder) return;
      log('H.264 decode error: ' + error.message);
      clientStats.decodeErrors++;
      setStatus('H.264 解码失败');
      discardPendingH264Presentation(true);
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
  discardPendingH264Presentation(ackActive);
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
  discardActivePatch(ackActive);
  activeFrame = null;
  imgLoading = false;
}

function discardActivePatch(ackActive) {
  const patch = activePatch;
  activePatch = null;
  nextTileInfo = null;
  patchDrawing = false;
  if (patch && ackActive && patch.socket === ws) acknowledgeFrame(patch.id);
}

function failActivePatch(reason) {
  const patch = activePatch;
  if (patch && patch.socket === ws) acknowledgeFrame(patch.id);
  activePatch = null;
  nextTileInfo = null;
  patchDrawing = false;
  clientStats.dropped++;
  if (reason) log(reason);
  requestKeyFrame();
}

function drawActivePatch() {
  const patch = activePatch;
  if (!patch || patchDrawing || patch.parts.length !== patch.count) return;
  patchDrawing = true;
  let index = 0;
  function drawNextTile() {
    if (activePatch !== patch || patch.generation !== decodeGeneration || ws !== patch.socket ||
        !cfg || cfg.stream_id !== patch.streamId) {
      if (activePatch === patch) failActivePatch('patch invalidated before draw');
      return;
    }
    if (index >= patch.parts.length) {
      acknowledgeFrame(patch.id);
      recordPresentedFrame({ id:patch.id, receivedAt:patch.receivedAt });
      activePatch = null;
      patchDrawing = false;
      return;
    }
    const part = patch.parts[index++];
    const url = URL.createObjectURL(new Blob([part.data], { type:'image/jpeg' }));
    const image = new Image();
    image.onload = function() {
      URL.revokeObjectURL(url);
      if (activePatch !== patch || patch.generation !== decodeGeneration || ws !== patch.socket) {
        if (activePatch === patch) failActivePatch('patch draw state changed');
        return;
      }
      try {
        ctx.drawImage(image, part.x, part.y, part.width, part.height);
      } catch (error) {
        log('patch draw error: ' + error.message);
        failActivePatch();
        return;
      }
      drawNextTile();
    };
    image.onerror = function() {
      URL.revokeObjectURL(url);
      failActivePatch('patch JPEG decode failed');
    };
    image.src = url;
  }
  drawNextTile();
}

function handleBinary(data) {
  if (nextTileInfo) {
    const info = nextTileInfo;
    nextTileInfo = null;
    if (!activePatch || info.patch !== activePatch || activePatch.socket !== ws) {
      failActivePatch('patch binary without active batch');
      return;
    }
    activePatch.parts.push({ x:info.x, y:info.y, width:info.width, height:info.height, data:data });
    activePatch.received++;
    if (activePatch.received === activePatch.count) drawActivePatch();
    return;
  }
  if (activePatch) {
    failActivePatch('unexpected binary during patch batch');
    return;
  }
  const info = nextFrameInfo;
  nextFrameInfo = null;
  if (!cfg || !info) return;
  if (info.streamId !== cfg.stream_id) {
    acknowledgeFrame(info.id);
    return;
  }
  // 收到二进制负载并不代表浏览器能成功解码或绘制。首帧诊断会持续到实际
  // drawImage 成功，便于区分“Agent 没有输出视频”和“浏览器无法呈现视频”。
  if (!firstFramePresented) setStatus('已连接，正在解码远端首帧');
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
      clientStats.dropped++;
      resetH264DecoderForKeyFrame('H.264 decode queue backpressure; waiting for IDR');
      return;
    }
    const socket = ws;
    clientStats.maxDecodeQueue = Math.max(clientStats.maxDecodeQueue, h264Decoder.decodeQueueSize);
    const infos = h264Frames.get(info.timestamp) || [];
    const pending = { id:info.id, socket:socket, receivedAt:performance.now() };
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
      clientStats.dropped++;
      resetH264DecoderForKeyFrame('H.264 chunk rejected; waiting for IDR');
      log('H.264 chunk rejected: ' + error.message);
    }
    return;
  }
  if (pendingFrame) {
    // JPEG 没有帧间依赖，前端也始终只绘制最新帧；立刻确认旧帧以释放服务端窗口。
    acknowledgeFrame(pendingFrame.id);
    clientStats.dropped++;
  }
  pendingFrame = { data:data, id:info.id, streamId:info.streamId, receivedAt:performance.now() };
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
      try {
        ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
        recordPresentedFrame(frame);
        markFirstFramePresented(socket);
      } catch (error) {
        clientStats.dropped++;
        log('JPEG draw error: ' + error.message);
      }
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

function canSendInput() {
  return ws && ws.readyState === WebSocket.OPEN && authed;
}

// 仅 canvas 获得焦点时才把新的按键下发到远端。此前 window 级 keydown 会把用户
// 在密码框、显示器下拉框甚至浏览器页面内输入的字符一并注入，既容易误操作也会
// 让本地控件难以使用。已按下的按键仍必须允许 keyup 收尾，避免焦点切换时卡键。
function canSendKeyboardInput() {
  return canSendInput() && document.hasFocus() && document.activeElement === canvas;
}

function updateKeyboardFocusIndicator() {
  if (!canvas) return;
  canvas.classList.toggle('remote-keyboard-active',
    canSendInput() && document.hasFocus() && document.activeElement === canvas);
}

function send(obj) {
  if (!canSendInput()) return false;
  const socket = ws;
  try {
    socket.send(JSON.stringify(obj));
    clientStats.maxWsBuffered = Math.max(clientStats.maxWsBuffered, socket.bufferedAmount);
    return true;
  } catch (error) {
    // readyState 检查与 send 之间仍可能收到 Close 帧。输入失败后交给 onclose
    // 统一清理本地按键状态，不能让事件回调因 DOMException 冒泡中断。
    if (ws === socket) log('ws send failed: ' + error.message);
    return false;
  }
}

function recordPresentedFrame(info) {
  clientStats.drawn++;
  if (info && Number.isFinite(info.receivedAt)) {
    clientStats.drawMsTotal += Math.max(0, performance.now() - info.receivedAt);
    clientStats.drawMsSamples++;
  }
}

function scheduleMoveFlush() {
  if (moveQueued) return;
  moveQueued = true;
  requestAnimationFrame(flushPendingMove);
}

function flushPendingMove() {
  moveQueued = false;
  if (!pendingMove || !canSendInput()) {
    if (!canSendInput()) pendingMove = null;
    return;
  }
  if (ws.bufferedAmount > inputBufferHighWaterMark) {
    // 保留最终位置，缓冲退下去后再发；不把早已过期的轨迹继续发送到远端。
    window.setTimeout(scheduleMoveFlush, 16);
    return;
  }
  const point = pendingMove;
  pendingMove = null;
  send({t:'move', x:point.x, y:point.y});
  if (pendingMove) scheduleMoveFlush();
}

function scheduleWheelFlush() {
  if (wheelQueued) return;
  wheelQueued = true;
  requestAnimationFrame(flushPendingWheel);
}

function flushPendingWheel() {
  wheelQueued = false;
  if (!pendingWheelDelta || !canSendInput()) {
    if (!canSendInput()) pendingWheelDelta = 0;
    return;
  }
  if (ws.bufferedAmount > inputBufferHighWaterMark) {
    window.setTimeout(scheduleWheelFlush, 16);
    return;
  }
  const delta = Math.max(-1200, Math.min(1200, Math.trunc(pendingWheelDelta)));
  pendingWheelDelta -= delta;
  if (delta) send({t:'wheel', delta:delta});
  if (pendingWheelDelta) scheduleWheelFlush();
}

function flushClientStats() {
  if (!canSendInput()) return;
  const payload = {
    t:'client_stats', drawn:clientStats.drawn, dropped:clientStats.dropped,
    draw_ms_total:Math.round(clientStats.drawMsTotal),
    draw_ms_samples:clientStats.drawMsSamples, decode_errors:clientStats.decodeErrors,
    decode_ms_total:Math.round(clientStats.decodeMsTotal),
    decode_ms_samples:clientStats.decodeMsSamples,
    max_decode_queue:clientStats.maxDecodeQueue,
    max_ws_buffered:Math.round(clientStats.maxWsBuffered)
  };
  clientStats.drawn = 0; clientStats.dropped = 0;
  clientStats.drawMsTotal = 0; clientStats.drawMsSamples = 0;
  clientStats.decodeMsTotal = 0; clientStats.decodeMsSamples = 0;
  clientStats.decodeErrors = 0; clientStats.maxDecodeQueue = 0; clientStats.maxWsBuffered = 0;
  send(payload);
}
function btnName(b) {
  return b === 0 ? 'left' : b === 1 ? 'middle' : b === 2 ? 'right' : '';
}

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
  // GDI/锁屏路径不应为每个 mousemove 执行一次完整 BitBlt。先在控制端预测
  // 指针位置，使操作反馈不必等待下一次视频帧；被控端随后发送的 cursor 消息
  // 仍是权威状态，可处理边界限制、跨屏或目标程序改写指针位置的情况。
  remoteCursor = { visible:true, x:pendingMove.x, y:pendingMove.y };
  drawRemoteCursor();
  scheduleMoveFlush();
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
  cursorCanvas = document.getElementById('cursor');
  cursorCtx = cursorCanvas.getContext('2d');
  logEl = document.getElementById('log');
  statusEl = document.getElementById('status');
  pwEl = document.getElementById('pw');
  monSel = document.getElementById('monitor');
  qualitySel = document.getElementById('quality');
  patchThresholdEl = document.getElementById('patch-threshold');
  patchThresholdValueEl = document.getElementById('patch-threshold-value');
  loadStreamPreferences();
  document.getElementById('connect').addEventListener('click', connect);
  pwEl.addEventListener('keydown', function(e) { if (e.key === 'Enter') connect(); });
  monSel.addEventListener('change', function() { send({t:'monitor', index: parseInt(monSel.value, 10)}); });
  qualitySel.addEventListener('change', saveAndSendStreamPreferences);
  patchThresholdEl.addEventListener('input', updatePatchThresholdLabel);
  patchThresholdEl.addEventListener('change', saveAndSendStreamPreferences);
  window.addEventListener('resize', fitCanvas);
  window.addEventListener('keydown', function(e) {
    if (!canSendKeyboardInput()) return;
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
      const key = pressedKeys.get(e.code);
      if (key) {
        e.preventDefault();
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
    if (!button) return;
    lastPointer = p;
    pressedButtons.add(button);
    canvas.focus();
    send({t:'mouse', x:p.x, y:p.y, btn:button, down:true});
    e.preventDefault();
  });
  canvas.addEventListener('focus', updateKeyboardFocusIndicator);
  canvas.addEventListener('blur', function() {
    // 点击密码框、显示器选择框等本地控件时立即释放远端已有按键；否则按住 Ctrl
    // 后再点击 UI，远端会一直认为 Ctrl 处于按下状态。
    releasePressedInputs();
    updateKeyboardFocusIndicator();
  });
  window.addEventListener('mouseup', function(e) {
    if (!authed) return;
    const button = btnName(e.button);
    if (!button) return;
    if (pressedButtons.delete(button)) {
      send({t:'mouse', x:lastPointer.x, y:lastPointer.y, btn:button, down:false});
    }
  });
  canvas.addEventListener('contextmenu', function(e) { e.preventDefault(); });
  canvas.addEventListener('wheel', function(e) {
    if (!authed) return;
    const delta = normalizedWheelDelta(e);
    if (delta) {
      pendingWheelDelta = Math.max(-12000, Math.min(12000, pendingWheelDelta + delta));
      scheduleWheelFlush();
    }
    e.preventDefault();
  }, { passive:false });
  window.addEventListener('blur', function() {
    releasePressedInputs();
    updateKeyboardFocusIndicator();
  });
  window.addEventListener('focus', updateKeyboardFocusIndicator);
  document.addEventListener('visibilitychange', function() {
    if (document.hidden) releasePressedInputs();
  });
  window.setInterval(flushClientStats, 5000);
});
