const UUIDS = {
  service: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1',
  cmd: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2',
  evt: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3',
  stream: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4',
  cfg: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a5',
};

const OUTPUT_TARGETS = [
  { key: 'x', label: 'Aileron', prompt: 'Move your roll control left and right.' },
  { key: 'y', label: 'Elevator', prompt: 'Move your pitch control forward and back.' },
  { key: 'z', label: 'Rudder', prompt: 'Move your rudder left and right.' },
  { key: 'rx', label: 'Left Brake / Aux 1', prompt: 'Move the left brake or the control you want on Aux 1.' },
  { key: 'ry', label: 'Right Brake / Aux 2', prompt: 'Move the right brake or the control you want on Aux 2.' },
  { key: 'rz', label: 'Twist / Secondary Yaw', prompt: 'Move the twist or rocker control you want for secondary yaw.' },
  { key: 'slider1', label: 'Throttle', prompt: 'Move your throttle through its travel.' },
  { key: 'slider2', label: 'Aux Slider', prompt: 'Move the auxiliary slider or combined brake control.' },
  { key: 'hat', label: 'Hat Switch', prompt: 'Move the POV hat in a few directions.' },
];

const TARGET_KIND = {
  x: 'axis',
  y: 'axis',
  z: 'axis',
  rx: 'axis',
  ry: 'axis',
  rz: 'axis',
  slider1: 'axis',
  slider2: 'axis',
  hat: 'hat',
};

const DEFAULT_AXIS_CONFIG = Object.freeze({
  configured: false,
  device_id: 0,
  element_id: 0,
  invert: false,
  deadzone: { inner: 0, outer: 0 },
  smoothing_alpha: 0,
  curve: {
    type: 'bezier',
    p1: { x: 0.25, y: 0.25 },
    p2: { x: 0.75, y: 0.75 },
  },
});

const tuningState = {
  selectedAxisKey: 'z',
  curveDragPoint: null,
  applyTimer: null,
  preview: null,
  previewMetaKey: '',
};

const elements = {
  connectBtn: document.getElementById('connectBtn'),
  reconnectBtn: document.getElementById('reconnectBtn'),
  disconnectBtn: document.getElementById('disconnectBtn'),
  refreshDevicesBtn: document.getElementById('refreshDevicesBtn'),
  readConfigBtn: document.getElementById('readConfigBtn'),
  saveProfileBtn: document.getElementById('saveProfileBtn'),
  startStreamBtn: document.getElementById('startStreamBtn'),
  stopStreamBtn: document.getElementById('stopStreamBtn'),
  loadDescriptorBtn: document.getElementById('loadDescriptorBtn'),
  copyDescriptorBtn: document.getElementById('copyDescriptorBtn'),
  downloadDescriptorBtn: document.getElementById('downloadDescriptorBtn'),
  clearLogBtn: document.getElementById('clearLogBtn'),
  connBadge: document.getElementById('connBadge'),
  streamBadge: document.getElementById('streamBadge'),
  configBadge: document.getElementById('configBadge'),
  sampleRateBadge: document.getElementById('sampleRateBadge'),
  deviceName: document.getElementById('deviceName'),
  gattState: document.getElementById('gattState'),
  evtState: document.getElementById('evtState'),
  streamState: document.getElementById('streamState'),
  deviceCount: document.getElementById('deviceCount'),
  deviceList: document.getElementById('deviceList'),
  descriptorMeta: document.getElementById('descriptorMeta'),
  descriptorHex: document.getElementById('descriptorHex'),
  telemetryList: document.getElementById('telemetryList'),
  latestSample: document.getElementById('latestSample'),
  logPanel: document.getElementById('logPanel'),
  errorBanner: document.getElementById('errorBanner'),
  serviceUuidText: document.getElementById('serviceUuidText'),
  targetSelect: document.getElementById('targetSelect'),
  wizardStartBtn: document.getElementById('wizardStartBtn'),
  wizardConfirmBtn: document.getElementById('wizardConfirmBtn'),
  wizardRetryBtn: document.getElementById('wizardRetryBtn'),
  wizardProgressFill: document.getElementById('wizardProgressFill'),
  wizardPrompt: document.getElementById('wizardPrompt'),
  wizardStatus: document.getElementById('wizardStatus'),
  wizardDetected: document.getElementById('wizardDetected'),
  wizardPreview: document.getElementById('wizardPreview'),
  mappingList: document.getElementById('mappingList'),
  tuneAxisSelect: document.getElementById('tuneAxisSelect'),
  resetModifiersBtn: document.getElementById('resetModifiersBtn'),
  deadzoneInnerRange: document.getElementById('deadzoneInnerRange'),
  deadzoneInnerInput: document.getElementById('deadzoneInnerInput'),
  outerClampRange: document.getElementById('outerClampRange'),
  outerClampInput: document.getElementById('outerClampInput'),
  smoothingRange: document.getElementById('smoothingRange'),
  smoothingInput: document.getElementById('smoothingInput'),
  curveSvg: document.getElementById('curveSvg'),
  curveStatus: document.getElementById('curveStatus'),
  previewRaw: document.getElementById('previewRaw'),
  previewDeadzoned: document.getElementById('previewDeadzoned'),
  previewCurved: document.getElementById('previewCurved'),
  previewSmoothed: document.getElementById('previewSmoothed'),
};

elements.serviceUuidText.textContent = UUIDS.service;

for (const target of OUTPUT_TARGETS) {
  const option = document.createElement('option');
  option.value = target.key;
  option.textContent = target.label;
  elements.targetSelect.appendChild(option);
  const tuneOption = option.cloneNode(true);
  elements.tuneAxisSelect.appendChild(tuneOption);
}

elements.targetSelect.value = 'z';
elements.tuneAxisSelect.value = 'z';

function nowLabel() {
  return new Date().toLocaleTimeString();
}

function log(message, detail = '') {
  const line = `[${nowLabel()}] ${message}${detail ? ` ${detail}` : ''}`;
  elements.logPanel.textContent += `${line}\n`;
  elements.logPanel.scrollTop = elements.logPanel.scrollHeight;
}

function showError(message) {
  elements.errorBanner.textContent = message;
  elements.errorBanner.classList.remove('hidden');
  log('ERROR', message);
}

function clearError() {
  elements.errorBanner.textContent = '';
  elements.errorBanner.classList.add('hidden');
}

function bytesToHex(bytes) {
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');
}

function formatHexBlock(bytes, width = 16) {
  const lines = [];
  for (let offset = 0; offset < bytes.length; offset += width) {
    const chunk = bytes.slice(offset, offset + width);
    const hex = Array.from(chunk, (value) => value.toString(16).padStart(2, '0')).join(' ');
    const ascii = Array.from(chunk, (value) => (value >= 32 && value <= 126 ? String.fromCharCode(value) : '.')).join('');
    lines.push(`${offset.toString(16).padStart(4, '0')}  ${hex.padEnd(width * 3 - 1, ' ')}  ${ascii}`);
  }
  return lines.join('\n');
}

function decodeUtf8(bytes) {
  return new TextDecoder().decode(bytes);
}

function encodeUtf8(value) {
  return new TextEncoder().encode(value);
}

function formatQ15(value) {
  return (value / 32767).toFixed(4);
}

function deepClone(value) {
  return JSON.parse(JSON.stringify(value));
}

function configToStableString(config) {
  return JSON.stringify(config ?? {});
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function round3(value) {
  return Math.round(value * 1000) / 1000;
}

function deepMerge(target, patch) {
  if (patch == null || typeof patch !== 'object' || Array.isArray(patch)) return patch;
  const out = Array.isArray(target) ? [...target] : { ...(target || {}) };
  for (const [key, value] of Object.entries(patch)) {
    if (value && typeof value === 'object' && !Array.isArray(value)) {
      out[key] = deepMerge(out[key], value);
    } else {
      out[key] = value;
    }
  }
  return out;
}

function normalizeAxisConfig(mapping) {
  const merged = deepMerge(DEFAULT_AXIS_CONFIG, mapping || {});
  if (typeof merged.deadzone === 'number') {
    merged.deadzone = { inner: merged.deadzone, outer: 0 };
  }
  merged.deadzone = {
    inner: clamp(Number(merged.deadzone?.inner || 0), 0, 0.5),
    outer: clamp(Number(merged.deadzone?.outer || 0), 0, 0.5),
  };
  if (merged.deadzone.inner + merged.deadzone.outer > 0.98) {
    merged.deadzone.outer = Math.max(0, 0.98 - merged.deadzone.inner);
  }
  merged.smoothing_alpha = clamp(Number(merged.smoothing_alpha || 0), 0, 1);
  merged.curve = deepMerge(DEFAULT_AXIS_CONFIG.curve, merged.curve || {});
  merged.curve.p1 = {
    x: clamp(Number(merged.curve?.p1?.x ?? 0.25), 0, 1),
    y: clamp(Number(merged.curve?.p1?.y ?? 0.25), 0, 1),
  };
  merged.curve.p2 = {
    x: clamp(Number(merged.curve?.p2?.x ?? 0.75), 0, 1),
    y: clamp(Number(merged.curve?.p2?.y ?? 0.75), 0, 1),
  };
  merged.configured = Boolean(merged.configured && merged.device_id && merged.element_id != null);
  merged.device_id = Number(merged.device_id || 0);
  merged.element_id = Number(merged.element_id || 0);
  merged.invert = Boolean(merged.invert);
  merged.curve.type = 'bezier';
  return merged;
}

function ensureConfigShape(config) {
  const shaped = deepClone(config || { version: 2, axes: {} });
  shaped.version = 2;
  shaped.axes = shaped.axes || {};
  for (const target of OUTPUT_TARGETS) {
    if (shaped.axes[target.key]) {
      shaped.axes[target.key] = normalizeAxisConfig(shaped.axes[target.key]);
    }
  }
  return shaped;
}

function isSliderAxis(axisKey) {
  return axisKey === 'slider1' || axisKey === 'slider2';
}

function cubicBezier(a, b, c, d, t) {
  const mt = 1 - t;
  return mt * mt * mt * a + 3 * mt * mt * t * b + 3 * mt * t * t * c + t * t * t * d;
}

function applyBezier01(x, curve) {
  const p1x = clamp(Number(curve?.p1?.x ?? 0.25), 0, 1);
  const p1y = clamp(Number(curve?.p1?.y ?? 0.25), 0, 1);
  const p2x = clamp(Number(curve?.p2?.x ?? 0.75), 0, 1);
  const p2y = clamp(Number(curve?.p2?.y ?? 0.75), 0, 1);
  x = clamp(x, 0, 1);
  let lo = 0;
  let hi = 1;
  let t = x;
  for (let i = 0; i < 18; i += 1) {
    t = (lo + hi) * 0.5;
    const bx = cubicBezier(0, p1x, p2x, 1, t);
    if (bx < x) lo = t;
    else hi = t;
  }
  return clamp(cubicBezier(0, p1y, p2y, 1, t), 0, 1);
}

function applyDeadzoneBipolar(v, inner, outer) {
  inner = clamp(inner, 0, 0.99);
  outer = clamp(outer, 0, 0.99);
  let limit = 1 - outer;
  if (limit < 0.001) limit = 0.001;
  if (inner >= limit) inner = Math.max(0, limit - 0.001);
  const sign = v < 0 ? -1 : 1;
  const mag = Math.abs(v);
  if (mag <= inner) return 0;
  if (mag >= limit) return sign;
  return sign * ((mag - inner) / (limit - inner));
}

function applyDeadzoneUnipolar(v, inner, outer) {
  inner = clamp(inner, 0, 0.99);
  outer = clamp(outer, 0, 0.99);
  let limit = 1 - outer;
  if (limit < 0.001) limit = 0.001;
  if (inner >= limit) inner = Math.max(0, limit - 0.001);
  v = clamp(v, 0, 1);
  if (v <= inner) return 0;
  if (v >= limit) return 1;
  return (v - inner) / (limit - inner);
}

function computePreviewFromSample(sample, axisKey, mapping, previousSmoothed = null) {
  const config = normalizeAxisConfig(mapping);
  const slider = isSliderAxis(axisKey);
  let raw = slider ? clamp((sample.norm + 1) * 0.5, 0, 1) : clamp(sample.norm, -1, 1);
  if (config.invert) raw = slider ? 1 - raw : -raw;
  const deadzoned = slider
    ? applyDeadzoneUnipolar(raw, config.deadzone.inner, config.deadzone.outer)
    : applyDeadzoneBipolar(raw, config.deadzone.inner, config.deadzone.outer);
  const curved = slider
    ? applyBezier01(deadzoned, config.curve)
    : Math.sign(deadzoned) * applyBezier01(Math.abs(deadzoned), config.curve);
  const smoothed = config.smoothing_alpha <= 0 || previousSmoothed == null
    ? curved
    : previousSmoothed * (1 - config.smoothing_alpha) + curved * config.smoothing_alpha;
  return {
    raw,
    deadzoned,
    curved,
    smoothed,
    markerInput: slider ? deadzoned : Math.abs(deadzoned),
    markerOutput: slider ? curved : Math.abs(curved),
    updatedAt: sample.receivedAt,
  };
}

function humanizeKind(kind) {
  if (!kind) return 'unknown';
  if (kind === 'axis') return 'Axis';
  if (kind === 'hat') return 'Hat';
  if (kind === 'button') return 'Button';
  return kind;
}

function describeElement(meta) {
  if (!meta) return 'Unknown element';
  const usage = meta.usage_name && meta.usage_name !== 'unknown'
    ? meta.usage_name
    : `Usage ${meta.usage_page}:${meta.usage}`;
  return `${humanizeKind(meta.kind)} · ${usage} · element ${meta.element_id}`;
}

class ChunkAssembler {
  constructor() {
    this.messages = new Map();
  }

  push(frameBytes) {
    if (frameBytes.byteLength < 8) return null;
    const view = new DataView(frameBytes.buffer, frameBytes.byteOffset, frameBytes.byteLength);
    const version = view.getUint8(0);
    const type = view.getUint8(1);
    const msgId = view.getUint16(2, true);
    const offset = view.getUint16(4, true);
    const total = view.getUint16(6, true);
    const payload = frameBytes.slice(8);

    if (!this.messages.has(msgId)) {
      this.messages.set(msgId, {
        version,
        type,
        total,
        buffer: new Uint8Array(total),
        received: 0,
        offsets: new Set(),
      });
    }

    const state = this.messages.get(msgId);
    if (!state.offsets.has(offset)) {
      state.buffer.set(payload, offset);
      state.offsets.add(offset);
      state.received += payload.length;
    }

    if (state.received >= state.total) {
      this.messages.delete(msgId);
      return { version: state.version, type: state.type, msgId, bytes: state.buffer };
    }

    return null;
  }
}

class HotasConfigClient {
  constructor() {
    this.device = null;
    this.server = null;
    this.service = null;
    this.characteristics = {};
    this.requestId = 0;
    this.chunkAssembler = new ChunkAssembler();
    this.pendingJson = new Map();
    this.pendingDescriptorBinary = null;
    this.devices = [];
    this.selectedDeviceId = null;
    this.descriptorCache = new Map();
    this.elementMetaByDevice = new Map();
    this.sampleHistory = [];
    this.sampleTimestamps = [];
    this.latestSample = null;
    this.latestSampleByKey = new Map();
    this.streamActive = false;
    this.reconnectWanted = false;
    this.reconnectInFlight = false;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 4;
    this.currentConfig = null;
    this.savedConfigString = '';
    this.pendingChanges = false;
    this.onDisconnectedBound = this.onDisconnected.bind(this);
  }

  async requestDevice() {
    if (!navigator.bluetooth) {
      throw new Error('Web Bluetooth is not available in this browser. Use Chrome or Edge on desktop.');
    }

    log('Opening Bluetooth device picker');
    this.device = await navigator.bluetooth.requestDevice({
      filters: [
        { services: [UUIDS.service] },
        { namePrefix: 'HOTAS' },
      ],
      optionalServices: [UUIDS.service],
    });
    log('Device selected', `${this.device.name || '(unnamed)'} [${this.device.id}]`);
    this.device.removeEventListener('gattserverdisconnected', this.onDisconnectedBound);
    this.device.addEventListener('gattserverdisconnected', this.onDisconnectedBound);
    this.reconnectWanted = true;
    this.reconnectAttempts = 0;
    return this.device;
  }

  async connect({ reuseDevice = false } = {}) {
    clearError();

    if (!reuseDevice || !this.device) {
      await this.requestDevice();
    }
    if (!this.device) throw new Error('No Bluetooth device selected.');

    log('Connecting to GATT server', this.device.name || this.device.id);
    this.server = await this.device.gatt.connect();
    this.service = await this.server.getPrimaryService(UUIDS.service);

    this.characteristics.cmd = await this.service.getCharacteristic(UUIDS.cmd);
    this.characteristics.evt = await this.service.getCharacteristic(UUIDS.evt);
    this.characteristics.stream = await this.service.getCharacteristic(UUIDS.stream);
    this.characteristics.cfg = await this.service.getCharacteristic(UUIDS.cfg);

    await this.characteristics.evt.startNotifications();
    this.characteristics.evt.addEventListener('characteristicvaluechanged', this.handleEvtBound ??= (event) => {
      const value = event.target.value;
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      this.handleEvtNotification(bytes);
    });

    await this.characteristics.stream.startNotifications();
    this.characteristics.stream.addEventListener('characteristicvaluechanged', this.handleStreamBound ??= (event) => {
      const value = event.target.value;
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      this.handleStreamNotification(bytes);
    });

    log('Connected and notifications enabled');
    return true;
  }

  async disconnect({ intentional = true } = {}) {
    this.streamActive = false;
    if (intentional) this.reconnectWanted = false;

    try {
      if (this.device?.gatt?.connected) this.device.gatt.disconnect();
    } catch (error) {
      log('Disconnect warning', error.message || String(error));
    }

    this.server = null;
    this.service = null;
    this.characteristics = {};
  }

  async reconnect() {
    if (!this.device) throw new Error('No previously selected Bluetooth device to reconnect to.');
    return this.connect({ reuseDevice: true });
  }

  async onDisconnected() {
    log('Device disconnected unexpectedly');
    this.server = null;
    this.service = null;
    this.characteristics = {};
    this.streamActive = false;
    render();

    if (!this.reconnectWanted || this.reconnectInFlight) return;

    this.reconnectInFlight = true;
    while (this.reconnectWanted && this.reconnectAttempts < this.maxReconnectAttempts) {
      this.reconnectAttempts += 1;
      const delayMs = this.reconnectAttempts * 1000;
      log(`Reconnect attempt ${this.reconnectAttempts}/${this.maxReconnectAttempts} scheduled`, `${delayMs}ms`);
      await new Promise((resolve) => setTimeout(resolve, delayMs));
      try {
        await this.reconnect();
        await this.getDevices();
        await this.loadAllDescriptors();
        await this.getConfig({ markSaved: false });
        log('Reconnect successful');
        this.reconnectAttempts = 0;
        this.reconnectInFlight = false;
        render();
        return;
      } catch (error) {
        log('Reconnect failed', error.message || String(error));
      }
    }

    this.reconnectInFlight = false;
    showError('Connection dropped and automatic reconnect did not succeed. Press Reconnect to try again.');
  }

  async sendCommand(command) {
    if (!this.characteristics.cmd) throw new Error('Not connected to the Config Service.');
    const rid = ++this.requestId;
    const payload = { rid, ...command };
    const json = JSON.stringify(payload);

    const responsePromise = new Promise((resolve, reject) => {
      this.pendingJson.set(rid, { resolve, reject, payload });
      setTimeout(() => {
        if (this.pendingJson.has(rid)) {
          this.pendingJson.delete(rid);
          reject(new Error(`Timed out waiting for response to ${payload.cmd}`));
        }
      }, 8000);
    });

    await this.characteristics.cmd.writeValue(encodeUtf8(json));
    log('CMD →', json);
    return responsePromise;
  }

  async getDevices() {
    const response = await this.sendCommand({ cmd: 'get_devices' });
    this.devices = Array.isArray(response.devices) ? response.devices : [];
    if (this.selectedDeviceId == null && this.devices.length) this.selectedDeviceId = this.devices[0].device_id;
    return this.devices;
  }

  async getDescriptor(deviceId) {
    this.pendingDescriptorBinary = { deviceId, rid: this.requestId + 1 };
    return this.sendCommand({ cmd: 'get_descriptor', device_id: deviceId });
  }

  async loadAllDescriptors() {
    for (const device of this.devices) {
      if (this.elementMetaByDevice.has(device.device_id)) continue;
      await this.getDescriptor(device.device_id);
    }
  }

  async startStream() {
    const response = await this.sendCommand({ cmd: 'start_stream' });
    this.streamActive = true;
    return response;
  }

  async stopStream() {
    const response = await this.sendCommand({ cmd: 'stop_stream' });
    this.streamActive = false;
    return response;
  }

  async getConfig({ markSaved = true } = {}) {
    const response = await this.sendCommand({ cmd: 'get_config' });
    if (response.config && typeof response.config === 'object') {
      this.currentConfig = ensureConfigShape(response.config);
    } else if (response.config_json) {
      this.currentConfig = ensureConfigShape(JSON.parse(response.config_json));
    }

    if (markSaved && this.currentConfig) {
      this.savedConfigString = configToStableString(this.currentConfig);
      this.pendingChanges = false;
    }
    return this.currentConfig;
  }

  async saveProfile() {
    const response = await this.sendCommand({ cmd: 'save_profile' });
    if (response.ok && this.currentConfig) {
      this.savedConfigString = configToStableString(this.currentConfig);
      this.pendingChanges = false;
    }
    return response;
  }

  async applyAxisConfig(axisKey, axisConfig) {
    const normalized = normalizeAxisConfig(axisConfig);
    const patch = {
      version: 2,
      axes: {
        [axisKey]: normalized,
      },
    };

    const response = await this.sendCommand({ cmd: 'set_config', config: patch });
    if (response.config && typeof response.config === 'object') {
      this.currentConfig = ensureConfigShape(response.config);
    } else {
      this.currentConfig = ensureConfigShape(this.currentConfig || {});
      this.currentConfig.axes[axisKey] = normalized;
    }

    this.pendingChanges = configToStableString(this.currentConfig) !== this.savedConfigString;
    return response;
  }

  async applyAxisPatch(axisKey, candidate) {
    const existing = normalizeAxisConfig(this.currentConfig?.axes?.[axisKey]);
    const next = {
      ...existing,
      configured: true,
      device_id: candidate.deviceId,
      element_id: candidate.elementId,
    };
    return this.applyAxisConfig(axisKey, next);
  }

  getElementMeta(deviceId, elementId) {
    return this.elementMetaByDevice.get(deviceId)?.get(String(elementId)) || null;
  }

  handleEvtNotification(bytes) {
    const complete = this.chunkAssembler.push(bytes);
    if (!complete) return;

    if (complete.type === 1) {
      const text = decodeUtf8(complete.bytes);
      log('EVT JSON ←', text);
      let payload = null;
      try {
        payload = JSON.parse(text);
      } catch (error) {
        showError(`Received malformed JSON EVT payload: ${error.message}`);
        return;
      }

      const rid = payload?.rid;
      if (typeof rid === 'number' && this.pendingJson.has(rid)) {
        const pending = this.pendingJson.get(rid);
        this.pendingJson.delete(rid);
        pending.resolve(payload);
      }

      if (Array.isArray(payload?.devices)) this.devices = payload.devices;
      if (typeof payload?.device_id === 'number' && Array.isArray(payload?.elements)) {
        const byId = new Map();
        for (const element of payload.elements) {
          byId.set(String(element.element_id), element);
        }
        this.elementMetaByDevice.set(payload.device_id, byId);
      }
      if (payload?.config && typeof payload.config === 'object') {
        this.currentConfig = ensureConfigShape(payload.config);
      }

      render();
      return;
    }

    if (complete.type === 2) {
      if (this.pendingDescriptorBinary?.deviceId != null) {
        this.descriptorCache.set(this.pendingDescriptorBinary.deviceId, complete.bytes);
        log('Descriptor received', `${complete.bytes.length} bytes for device ${this.pendingDescriptorBinary.deviceId}`);
      } else {
        log('Descriptor received', `${complete.bytes.length} bytes`);
      }
      this.pendingDescriptorBinary = null;
      render();
      return;
    }

    log('Unhandled EVT frame', `type=${complete.type} len=${complete.bytes.length}`);
  }

  handleStreamNotification(bytes) {
    if (bytes.byteLength < 16) return;

    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const sample = {
      version: view.getUint8(0),
      flags: view.getUint8(1),
      deviceId: view.getUint32(2, true),
      elementId: view.getUint32(6, true),
      raw: view.getInt32(10, true),
      normQ15: view.getInt16(14, true),
      norm: view.getInt16(14, true) / 32767,
      receivedAt: new Date(),
    };

    const meta = this.getElementMeta(sample.deviceId, sample.elementId);
    if (meta) sample.meta = meta;

    this.latestSample = sample;
    this.latestSampleByKey.set(`${sample.deviceId}:${sample.elementId}`, sample);
    this.sampleHistory.unshift(sample);
    this.sampleHistory = this.sampleHistory.slice(0, 64);

    const now = Date.now();
    this.sampleTimestamps.push(now);
    while (this.sampleTimestamps.length && now - this.sampleTimestamps[0] > 1000) {
      this.sampleTimestamps.shift();
    }

    wizard.consumeSample(sample);
    render();
  }
}

class MappingWizard {
  constructor(client) {
    this.client = client;
    this.state = 'idle';
    this.targetKey = 'z';
    this.startedAt = 0;
    this.durationMs = 1600;
    this.intervalId = null;
    this.timeoutId = null;
    this.detector = new Map();
    this.candidate = null;
  }

  reset(state = 'idle') {
    this.state = state;
    this.startedAt = 0;
    this.detector.clear();
    this.candidate = null;
    clearInterval(this.intervalId);
    clearTimeout(this.timeoutId);
    this.intervalId = null;
    this.timeoutId = null;
  }

  async begin(targetKey) {
    this.reset('detecting');
    this.targetKey = targetKey;
    this.startedAt = Date.now();

    if (!this.client.streamActive) await this.client.startStream();
    if (!this.client.devices.length) await this.client.getDevices();
    await this.client.loadAllDescriptors();

    this.intervalId = setInterval(() => render(), 60);
    this.timeoutId = setTimeout(() => this.finish(), this.durationMs);
    render();
  }

  consumeSample(sample) {
    if (this.state !== 'detecting') return;

    const meta = sample.meta || this.client.getElementMeta(sample.deviceId, sample.elementId);
    const kind = meta?.kind || 'other';
    const expectedKind = TARGET_KIND[this.targetKey] || 'axis';
    const key = `${sample.deviceId}:${sample.elementId}`;
    const entry = this.detector.get(key) || {
      key,
      deviceId: sample.deviceId,
      elementId: sample.elementId,
      kind,
      meta,
      score: 0,
      totalDelta: 0,
      maxMagnitude: 0,
      activeCount: 0,
      sampleCount: 0,
      lastNorm: null,
      lastRaw: null,
    };

    const delta = entry.lastNorm == null ? Math.abs(sample.norm) : Math.abs(sample.norm - entry.lastNorm);
    const rawChanged = entry.lastRaw == null ? sample.raw !== 0 : sample.raw !== entry.lastRaw;
    const magnitude = kind === 'button' ? (sample.raw ? 1 : 0) : Math.abs(sample.norm);
    const threshold = kind === 'hat' ? 0.18 : (kind === 'button' ? 0.5 : 0.05);
    const changed = delta >= threshold || rawChanged;

    let weight = 0.08;
    if (kind === expectedKind) weight = 1.0;
    else if (expectedKind === 'axis' && kind === 'hat') weight = 0.35;
    else if (expectedKind === 'hat' && kind === 'axis') weight = 0.2;
    else if (kind === 'button') weight = 0.05;

    if (changed) {
      entry.activeCount += 1;
      entry.score += weight * (delta * 14 + magnitude * 3 + 1.2);
    } else {
      entry.score += weight * magnitude * 0.05;
    }

    entry.totalDelta += delta;
    entry.maxMagnitude = Math.max(entry.maxMagnitude, magnitude);
    entry.sampleCount += 1;
    entry.lastNorm = sample.norm;
    entry.lastRaw = sample.raw;
    entry.meta = meta || entry.meta;
    entry.kind = kind;

    this.detector.set(key, entry);
    this.candidate = this.pickBestCandidate();
  }

  pickBestCandidate() {
    const expectedKind = TARGET_KIND[this.targetKey] || 'axis';
    const candidates = [...this.detector.values()]
      .filter((entry) => entry.kind !== 'other')
      .filter((entry) => {
        if (expectedKind === 'hat') return entry.kind === 'hat' && entry.activeCount >= 1;
        if (entry.kind !== 'axis') return false;
        return entry.activeCount >= 2 && (entry.totalDelta >= 0.22 || entry.maxMagnitude >= 0.55);
      })
      .map((entry) => ({
        ...entry,
        weightedScore: entry.score + entry.activeCount * 0.6 + entry.maxMagnitude * 0.8,
      }))
      .sort((a, b) => b.weightedScore - a.weightedScore);

    return candidates[0] || null;
  }

  finish() {
    if (this.state !== 'detecting') return;
    clearInterval(this.intervalId);
    clearTimeout(this.timeoutId);
    this.intervalId = null;
    this.timeoutId = null;
    this.candidate = this.pickBestCandidate();
    this.state = this.candidate ? 'candidate' : 'no-match';
    render();
  }

  async confirm() {
    if (!this.candidate) throw new Error('No candidate mapping to confirm.');
    await this.client.applyAxisPatch(this.targetKey, this.candidate);
    tuningState.selectedAxisKey = this.targetKey;
    elements.tuneAxisSelect.value = this.targetKey;
    tuningState.preview = null;
    tuningState.previewMetaKey = '';
    this.state = 'applied';
    render();
  }

  get progress() {
    if (this.state !== 'detecting') return this.state === 'applied' ? 1 : 0;
    return Math.max(0, Math.min(1, (Date.now() - this.startedAt) / this.durationMs));
  }
}

function selectedAxisConfig() {
  const config = ensureConfigShape(client.currentConfig || { version: 2, axes: {} });
  client.currentConfig = config;
  return normalizeAxisConfig(config.axes[tuningState.selectedAxisKey]);
}

function getLatestMappedSample(axisKey, mapping = selectedAxisConfig()) {
  if (!mapping?.configured || !mapping.device_id || mapping.element_id == null) return null;
  return client.latestSampleByKey.get(`${mapping.device_id}:${mapping.element_id}`) || null;
}

function refreshPreviewForSelectedAxis() {
  const axisKey = tuningState.selectedAxisKey;
  const mapping = selectedAxisConfig();
  const sample = getLatestMappedSample(axisKey, mapping);
  if (!sample) {
    tuningState.preview = null;
    tuningState.previewMetaKey = '';
    return;
  }
  const metaKey = `${sample.deviceId}:${sample.elementId}:${sample.receivedAt?.getTime?.() || 0}`;
  if (metaKey === tuningState.previewMetaKey && tuningState.preview) return;
  const prev = tuningState.previewMetaKey.startsWith(`${sample.deviceId}:${sample.elementId}:`) ? tuningState.preview?.smoothed : null;
  tuningState.preview = computePreviewFromSample(sample, axisKey, mapping, prev);
  tuningState.previewMetaKey = metaKey;
}

function scheduleSelectedAxisApply() {
  clearTimeout(tuningState.applyTimer);
  tuningState.applyTimer = setTimeout(async () => {
    try {
      await client.applyAxisConfig(tuningState.selectedAxisKey, selectedAxisConfig());
      render();
    } catch (error) {
      showError(`Failed to apply tuning change: ${error.message || error}`);
      render();
    }
  }, 120);
}

function updateSelectedAxisConfig(mutator) {
  client.currentConfig = ensureConfigShape(client.currentConfig || { version: 2, axes: {} });
  const current = normalizeAxisConfig(client.currentConfig.axes[tuningState.selectedAxisKey]);
  const next = deepClone(current);
  mutator(next);
  client.currentConfig.axes[tuningState.selectedAxisKey] = normalizeAxisConfig(next);
  client.pendingChanges = configToStableString(client.currentConfig) !== client.savedConfigString;
  tuningState.preview = null;
  tuningState.previewMetaKey = '';
  refreshPreviewForSelectedAxis();
  scheduleSelectedAxisApply();
  render();
}

function drawCurveEditor(axisKey, mapping) {
  const svg = elements.curveSvg;
  const width = 360;
  const height = 240;
  const pad = 28;
  const plotW = width - pad * 2;
  const plotH = height - pad * 2;
  const toX = (n) => pad + n * plotW;
  const toY = (n) => height - pad - n * plotH;
  const p1 = mapping.curve.p1;
  const p2 = mapping.curve.p2;

  const curvePoints = [];
  for (let i = 0; i <= 40; i += 1) {
    const x = i / 40;
    const y = applyBezier01(x, mapping.curve);
    curvePoints.push(`${toX(x).toFixed(2)},${toY(y).toFixed(2)}`);
  }

  const markerInput = tuningState.preview ? clamp(tuningState.preview.markerInput, 0, 1) : null;
  const markerOutput = tuningState.preview ? clamp(tuningState.preview.markerOutput, 0, 1) : null;
  const marker = markerInput != null ? `
    <circle cx="${toX(markerInput)}" cy="${toY(markerOutput)}" r="6" fill="#49d7c4" stroke="#0c101a" stroke-width="2"></circle>
    <line x1="${toX(markerInput)}" y1="${toY(0)}" x2="${toX(markerInput)}" y2="${toY(markerOutput)}" stroke="rgba(73,215,196,0.35)" stroke-dasharray="4 4"></line>
    <line x1="${toX(0)}" y1="${toY(markerOutput)}" x2="${toX(markerInput)}" y2="${toY(markerOutput)}" stroke="rgba(73,215,196,0.35)" stroke-dasharray="4 4"></line>
  ` : '';

  const grid = [0, 0.25, 0.5, 0.75, 1].map((tick) => `
    <line x1="${toX(tick)}" y1="${toY(0)}" x2="${toX(tick)}" y2="${toY(1)}" stroke="rgba(255,255,255,0.06)"></line>
    <line x1="${toX(0)}" y1="${toY(tick)}" x2="${toX(1)}" y2="${toY(tick)}" stroke="rgba(255,255,255,0.06)"></line>
  `).join('');

  svg.innerHTML = `
    <rect x="0" y="0" width="${width}" height="${height}" rx="18" fill="rgba(8,12,24,0.86)"></rect>
    ${grid}
    <line x1="${toX(0)}" y1="${toY(0)}" x2="${toX(1)}" y2="${toY(1)}" stroke="rgba(124,157,255,0.24)" stroke-dasharray="6 6"></line>
    <path d="M ${toX(0)} ${toY(0)} C ${toX(p1.x)} ${toY(p1.y)}, ${toX(p2.x)} ${toY(p2.y)}, ${toX(1)} ${toY(1)}" fill="none" stroke="#7c9dff" stroke-width="4" stroke-linecap="round"></path>
    <line x1="${toX(0)}" y1="${toY(0)}" x2="${toX(p1.x)}" y2="${toY(p1.y)}" stroke="rgba(255,255,255,0.25)"></line>
    <line x1="${toX(1)}" y1="${toY(1)}" x2="${toX(p2.x)}" y2="${toY(p2.y)}" stroke="rgba(255,255,255,0.25)"></line>
    ${marker}
    <circle class="curve-point" data-point="p1" cx="${toX(p1.x)}" cy="${toY(p1.y)}" r="9" fill="#7c9dff" stroke="#ffffff" stroke-width="2"></circle>
    <circle class="curve-point" data-point="p2" cx="${toX(p2.x)}" cy="${toY(p2.y)}" r="9" fill="#ff9b6b" stroke="#ffffff" stroke-width="2"></circle>
    <text x="${toX(0)}" y="${toY(0) + 18}" class="curve-label">0</text>
    <text x="${toX(1) - 10}" y="${toY(0) + 18}" class="curve-label">1</text>
    <text x="${toX(0) - 12}" y="${toY(1)}" class="curve-label">1</text>
  `;

  elements.curveStatus.textContent = `${targetByKey(axisKey).label} · p1 ${round3(p1.x)},${round3(p1.y)} · p2 ${round3(p2.x)},${round3(p2.y)}`;
}

function handleCurvePointer(event) {
  if (!tuningState.curveDragPoint) return;
  const rect = elements.curveSvg.getBoundingClientRect();
  const width = 360;
  const height = 240;
  const pad = 28;
  const plotW = width - pad * 2;
  const plotH = height - pad * 2;
  const x = clamp((event.clientX - rect.left) / rect.width, 0, 1);
  const y = clamp((event.clientY - rect.top) / rect.height, 0, 1);
  let nx = clamp((x * width - pad) / plotW, 0, 1);
  const ny = clamp((height - y * height - pad) / plotH, 0, 1);

  updateSelectedAxisConfig((next) => {
    next.curve = next.curve || deepClone(DEFAULT_AXIS_CONFIG.curve);
    next.curve.p1 = next.curve.p1 || { x: 0.25, y: 0.25 };
    next.curve.p2 = next.curve.p2 || { x: 0.75, y: 0.75 };
    if (tuningState.curveDragPoint === 'p1') {
      nx = Math.min(nx, next.curve.p2.x);
      next.curve.p1.x = nx;
      next.curve.p1.y = ny;
    } else {
      nx = Math.max(nx, next.curve.p1.x);
      next.curve.p2.x = nx;
      next.curve.p2.y = ny;
    }
  });
}

function bindModifierControls() {
  const pairs = [
    [elements.deadzoneInnerRange, elements.deadzoneInnerInput, 'deadzoneInner'],
    [elements.outerClampRange, elements.outerClampInput, 'outerClamp'],
    [elements.smoothingRange, elements.smoothingInput, 'smoothing'],
  ];
  for (const [rangeEl, inputEl, kind] of pairs) {
    const handler = (value) => {
      const numeric = Number(value);
      if (!Number.isFinite(numeric)) return;
      if (kind === 'deadzoneInner') {
        updateSelectedAxisConfig((next) => { next.deadzone.inner = numeric; });
      } else if (kind === 'outerClamp') {
        updateSelectedAxisConfig((next) => { next.deadzone.outer = numeric; });
      } else {
        updateSelectedAxisConfig((next) => { next.smoothing_alpha = numeric; });
      }
    };
    rangeEl.addEventListener('input', (event) => {
      inputEl.value = event.target.value;
      handler(event.target.value);
    });
    inputEl.addEventListener('input', (event) => {
      rangeEl.value = event.target.value;
      handler(event.target.value);
    });
  }

  elements.curveSvg.addEventListener('pointerdown', (event) => {
    const point = event.target?.dataset?.point;
    if (!point) return;
    tuningState.curveDragPoint = point;
    handleCurvePointer(event);
  });
  window.addEventListener('pointermove', handleCurvePointer);
  window.addEventListener('pointerup', () => { tuningState.curveDragPoint = null; });
}

const client = new HotasConfigClient();
const wizard = new MappingWizard(client);

function selectedDevice() {
  return client.devices.find((device) => device.device_id === client.selectedDeviceId) || null;
}

function targetByKey(key) {
  return OUTPUT_TARGETS.find((target) => target.key === key) || OUTPUT_TARGETS[0];
}

function renderConnectionState() {
  const connected = Boolean(client.device?.gatt?.connected && client.service);
  elements.connBadge.textContent = connected ? 'Connected' : 'Disconnected';
  elements.connBadge.className = `pill ${connected ? 'success' : 'warn'}`;
  elements.streamBadge.textContent = client.streamActive ? 'Stream active' : 'Stream idle';
  elements.streamBadge.className = `pill ${client.streamActive ? 'success' : 'muted'}`;
  elements.configBadge.textContent = client.pendingChanges ? 'Pending changes' : 'Saved state';
  elements.configBadge.className = `pill ${client.pendingChanges ? 'warn' : 'success'}`;
  elements.deviceName.textContent = client.device?.name || client.device?.id || '—';
  elements.gattState.textContent = connected ? 'Connected' : 'Not connected';
  elements.evtState.textContent = connected ? 'Subscribed' : 'Off';
  elements.streamState.textContent = connected ? 'Subscribed' : 'Off';

  elements.connectBtn.disabled = connected;
  elements.reconnectBtn.disabled = !client.device || connected;
  elements.disconnectBtn.disabled = !connected;
  elements.refreshDevicesBtn.disabled = !connected;
  elements.readConfigBtn.disabled = !connected;
  elements.saveProfileBtn.disabled = !connected || !client.pendingChanges;
  elements.startStreamBtn.disabled = !connected || client.streamActive;
  elements.stopStreamBtn.disabled = !connected || !client.streamActive;
  elements.wizardStartBtn.disabled = !connected || wizard.state === 'detecting';
  elements.wizardConfirmBtn.disabled = wizard.state !== 'candidate';
  elements.wizardRetryBtn.disabled = !connected || wizard.state === 'detecting';
}

function renderDevices() {
  elements.deviceCount.textContent = `${client.devices.length} device${client.devices.length === 1 ? '' : 's'}`;
  if (!client.devices.length) {
    elements.deviceList.className = 'device-list empty-state';
    elements.deviceList.textContent = 'Connect and request devices to populate this list.';
    return;
  }

  elements.deviceList.className = 'device-list';
  elements.deviceList.innerHTML = '';
  for (const device of client.devices) {
    const card = document.createElement('article');
    card.className = `device-card${device.device_id === client.selectedDeviceId ? ' active' : ''}`;
    card.innerHTML = `
      <h3>${device.role || 'unknown'} · device ${device.device_id}</h3>
      <p>Address ${device.dev_addr} · ${device.num_elements} elements · descriptor ${device.report_desc_len} bytes</p>
      <div class="device-actions">
        <span class="pill muted">role: ${device.role || 'unknown'}</span>
        <span class="pill muted">id: ${device.device_id}</span>
      </div>
    `;
    card.addEventListener('click', () => {
      client.selectedDeviceId = device.device_id;
      render();
    });
    elements.deviceList.appendChild(card);
  }
}

function renderDescriptor() {
  const device = selectedDevice();
  const bytes = device ? client.descriptorCache.get(device.device_id) : null;
  const metadata = device ? client.elementMetaByDevice.get(device.device_id) : null;

  elements.loadDescriptorBtn.disabled = !device || !client.service;
  elements.copyDescriptorBtn.disabled = !bytes;
  elements.downloadDescriptorBtn.disabled = !bytes;

  if (!device) {
    elements.descriptorMeta.textContent = 'Select a device card to inspect its descriptor.';
    elements.descriptorHex.className = 'code-panel empty-state';
    elements.descriptorHex.textContent = 'No descriptor loaded yet.';
    return;
  }

  const metaCount = metadata ? metadata.size : 0;
  elements.descriptorMeta.innerHTML = `
    <strong>Device ${device.device_id}</strong> · role <strong>${device.role}</strong> ·
    <span>${device.num_elements} elements</span> ·
    <span>metadata ${metaCount}</span> ·
    <span>descriptor ${device.report_desc_len} bytes</span>
  `;

  if (!bytes) {
    elements.descriptorHex.className = 'code-panel empty-state';
    elements.descriptorHex.textContent = 'Click “Load Descriptor” to request the report descriptor for this device.';
    return;
  }

  elements.descriptorHex.className = 'code-panel';
  elements.descriptorHex.textContent = formatHexBlock(bytes);
}

function renderTelemetry() {
  const history = client.sampleHistory;
  const latest = client.latestSample;
  const samplesPerSecond = client.sampleTimestamps.length;
  elements.sampleRateBadge.textContent = samplesPerSecond ? `~${samplesPerSecond} samples/s` : 'No samples yet';

  const latestFields = elements.latestSample.querySelectorAll('.latest-row span:last-child');
  if (!latest) {
    ['—', '—', '—', '—'].forEach((value, index) => { latestFields[index].textContent = value; });
  } else {
    latestFields[0].textContent = latest.meta ? describeElement(latest.meta) : `device ${latest.deviceId} · element ${latest.elementId}`;
    latestFields[1].textContent = String(latest.raw);
    latestFields[2].textContent = `${latest.normQ15} (${formatQ15(latest.normQ15)})`;
    latestFields[3].textContent = latest.receivedAt.toLocaleTimeString();
  }

  if (!history.length) {
    elements.telemetryList.className = 'telemetry-list empty-state';
    elements.telemetryList.textContent = client.streamActive ? 'Waiting for stream samples…' : 'Start stream to see live samples.';
    return;
  }

  elements.telemetryList.className = 'telemetry-list';
  elements.telemetryList.innerHTML = '';
  for (const sample of history.slice(0, 24)) {
    const row = document.createElement('div');
    row.className = 'telemetry-row';
    row.innerHTML = `
      <div>
        <div>${sample.meta ? describeElement(sample.meta) : `device ${sample.deviceId} · element ${sample.elementId}`}</div>
        <div class="muted">device ${sample.deviceId}</div>
      </div>
      <div>
        <div>${sample.raw}</div>
        <div class="muted">raw</div>
      </div>
      <div>
        <div>${formatQ15(sample.normQ15)}</div>
        <div class="muted">normalized</div>
      </div>
      <div>
        <div>${sample.receivedAt.toLocaleTimeString()}</div>
        <div class="muted">updated</div>
      </div>
    `;
    elements.telemetryList.appendChild(row);
  }
}

function renderWizard() {
  const target = targetByKey(elements.targetSelect.value);
  const progress = wizard.progress;
  elements.wizardProgressFill.style.width = `${Math.round(progress * 100)}%`;
  elements.wizardPrompt.textContent = target.prompt;

  if (wizard.state === 'detecting') {
    elements.wizardStatus.textContent = 'Listening for the control with the strongest sustained movement…';
  } else if (wizard.state === 'candidate') {
    elements.wizardStatus.textContent = 'Candidate detected. Confirm to apply this mapping immediately.';
  } else if (wizard.state === 'applied') {
    elements.wizardStatus.textContent = 'Mapping applied in memory. Validate it now, then click Save when you are happy.';
  } else if (wizard.state === 'no-match') {
    elements.wizardStatus.textContent = 'No confident match found. Retry and move only the control you want to map.';
  } else {
    elements.wizardStatus.textContent = 'Choose an output target and start the guided detector.';
  }

  if (!wizard.candidate) {
    elements.wizardDetected.innerHTML = '<strong>No candidate yet.</strong><span class="muted-line">Start detection and move only the intended control.</span>';
    elements.wizardPreview.innerHTML = '<span class="muted-line">Live preview will appear here.</span>';
    return;
  }

  const meta = wizard.candidate.meta;
  elements.wizardDetected.innerHTML = `
    <strong>${describeElement(meta)}</strong>
    <span class="muted-line">Device ${wizard.candidate.deviceId} · score ${wizard.candidate.weightedScore?.toFixed(2) || wizard.candidate.score.toFixed(2)}</span>
  `;
  elements.wizardPreview.innerHTML = `
    <div class="preview-stat"><span>Active samples</span><strong>${wizard.candidate.activeCount}</strong></div>
    <div class="preview-stat"><span>Total delta</span><strong>${wizard.candidate.totalDelta.toFixed(3)}</strong></div>
    <div class="preview-stat"><span>Peak magnitude</span><strong>${wizard.candidate.maxMagnitude.toFixed(3)}</strong></div>
  `;
}

function renderMappings() {
  const config = ensureConfigShape(client.currentConfig || { version: 2, axes: {} });
  const axes = config.axes || {};
  elements.mappingList.innerHTML = '';

  for (const target of OUTPUT_TARGETS) {
    const mapping = normalizeAxisConfig(axes[target.key]);
    const row = document.createElement('div');
    row.className = `mapping-row${tuningState.selectedAxisKey === target.key ? ' active' : ''}`;

    let value = '<span class="muted-line">Unmapped · use the wizard above to assign a source.</span>';
    if (mapping?.configured && mapping.device_id && mapping.element_id != null) {
      const meta = client.getElementMeta(mapping.device_id, mapping.element_id);
      const desc = meta ? describeElement(meta) : `device ${mapping.device_id} · element ${mapping.element_id}`;
      value = `<strong>${desc}</strong><span class="muted-line">inner=${mapping.deadzone.inner.toFixed(3)} · outer=${mapping.deadzone.outer.toFixed(3)} · ema=${mapping.smoothing_alpha.toFixed(2)}</span>`;
    }

    row.innerHTML = `
      <div>
        <div class="mapping-name">${target.label}</div>
        <div class="mapping-key">${target.key}</div>
      </div>
      <div class="mapping-value">${value}</div>
    `;
    row.addEventListener('click', () => {
      tuningState.selectedAxisKey = target.key;
      elements.tuneAxisSelect.value = target.key;
      tuningState.preview = null;
      tuningState.previewMetaKey = '';
      refreshPreviewForSelectedAxis();
      render();
    });
    elements.mappingList.appendChild(row);
  }
}

function renderTuningPanel() {
  const mapping = selectedAxisConfig();
  const axisKey = tuningState.selectedAxisKey;
  const connected = Boolean(client.device?.gatt?.connected && client.service);
  const isHat = axisKey === 'hat';
  const disabled = !connected || !mapping.configured || isHat;

  elements.tuneAxisSelect.value = axisKey;
  elements.resetModifiersBtn.disabled = disabled;
  for (const el of [
    elements.deadzoneInnerRange,
    elements.deadzoneInnerInput,
    elements.outerClampRange,
    elements.outerClampInput,
    elements.smoothingRange,
    elements.smoothingInput,
  ]) {
    el.disabled = disabled;
  }

  elements.deadzoneInnerRange.value = mapping.deadzone.inner;
  elements.deadzoneInnerInput.value = mapping.deadzone.inner.toFixed(3);
  elements.outerClampRange.value = mapping.deadzone.outer;
  elements.outerClampInput.value = mapping.deadzone.outer.toFixed(3);
  elements.smoothingRange.value = mapping.smoothing_alpha;
  elements.smoothingInput.value = mapping.smoothing_alpha.toFixed(2);

  refreshPreviewForSelectedAxis();
  drawCurveEditor(axisKey, mapping);

  if (!mapping.configured) {
    elements.previewRaw.textContent = 'Unmapped';
    elements.previewDeadzoned.textContent = '—';
    elements.previewCurved.textContent = '—';
    elements.previewSmoothed.textContent = '—';
    elements.curveStatus.textContent = `${targetByKey(axisKey).label} · map a source first`;
    return;
  }
  if (isHat) {
    elements.previewRaw.textContent = 'POV';
    elements.previewDeadzoned.textContent = 'N/A';
    elements.previewCurved.textContent = 'N/A';
    elements.previewSmoothed.textContent = 'N/A';
    elements.curveStatus.textContent = `${targetByKey(axisKey).label} · deadzone/curve tuning is axis-only`;
    return;
  }

  const preview = tuningState.preview;
  if (!preview) {
    elements.previewRaw.textContent = 'Waiting';
    elements.previewDeadzoned.textContent = '—';
    elements.previewCurved.textContent = '—';
    elements.previewSmoothed.textContent = '—';
    return;
  }

  elements.previewRaw.textContent = preview.raw.toFixed(3);
  elements.previewDeadzoned.textContent = preview.deadzoned.toFixed(3);
  elements.previewCurved.textContent = preview.curved.toFixed(3);
  elements.previewSmoothed.textContent = preview.smoothed.toFixed(3);
}

function render() {
  renderConnectionState();
  renderDevices();
  renderDescriptor();
  renderTelemetry();
  renderWizard();
  renderMappings();
  renderTuningPanel();
}

async function guarded(action, fallbackMessage) {
  clearError();
  try {
    await action();
    render();
  } catch (error) {
    showError(`${fallbackMessage}: ${error.message || error}`);
    render();
  }
}

elements.connectBtn.addEventListener('click', () => guarded(async () => {
  await client.connect();
  await client.getDevices();
  await client.loadAllDescriptors();
  await client.getConfig();
}, 'Unable to connect'));

elements.reconnectBtn.addEventListener('click', () => guarded(async () => {
  await client.reconnect();
  await client.getDevices();
  await client.loadAllDescriptors();
  await client.getConfig({ markSaved: false });
}, 'Reconnect failed'));

elements.disconnectBtn.addEventListener('click', () => guarded(async () => {
  wizard.reset();
  await client.disconnect();
}, 'Disconnect failed'));

elements.refreshDevicesBtn.addEventListener('click', () => guarded(async () => {
  await client.getDevices();
  await client.loadAllDescriptors();
}, 'Failed to refresh devices'));

elements.readConfigBtn.addEventListener('click', () => guarded(async () => {
  const config = await client.getConfig({ markSaved: false });
  log('CFG ←', JSON.stringify(config));
}, 'Failed to read CFG'));

elements.saveProfileBtn.addEventListener('click', () => guarded(async () => {
  const response = await client.saveProfile();
  log('Save profile', response.note || 'Profile marked saved');
}, 'Failed to save profile'));

elements.startStreamBtn.addEventListener('click', () => guarded(async () => {
  await client.startStream();
}, 'Failed to start stream'));

elements.stopStreamBtn.addEventListener('click', () => guarded(async () => {
  wizard.reset();
  await client.stopStream();
}, 'Failed to stop stream'));

elements.loadDescriptorBtn.addEventListener('click', () => guarded(async () => {
  const device = selectedDevice();
  if (!device) throw new Error('Select a device first.');
  await client.getDescriptor(device.device_id);
}, 'Failed to load descriptor'));

elements.copyDescriptorBtn.addEventListener('click', async () => {
  const device = selectedDevice();
  const bytes = device ? client.descriptorCache.get(device.device_id) : null;
  if (!bytes) return;
  await navigator.clipboard.writeText(bytesToHex(bytes));
  log('Descriptor hex copied to clipboard');
});

elements.downloadDescriptorBtn.addEventListener('click', () => {
  const device = selectedDevice();
  const bytes = device ? client.descriptorCache.get(device.device_id) : null;
  if (!bytes || !device) return;

  const blob = new Blob([bytes], { type: 'application/octet-stream' });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement('a');
  anchor.href = url;
  anchor.download = `hid-descriptor-${device.device_id}.bin`;
  anchor.click();
  URL.revokeObjectURL(url);
  log('Descriptor binary downloaded', anchor.download);
});

elements.targetSelect.addEventListener('change', () => render());
elements.tuneAxisSelect.addEventListener('change', () => {
  tuningState.selectedAxisKey = elements.tuneAxisSelect.value;
  tuningState.preview = null;
  tuningState.previewMetaKey = '';
  refreshPreviewForSelectedAxis();
  render();
});
elements.resetModifiersBtn.addEventListener('click', () => {
  updateSelectedAxisConfig((next) => {
    next.deadzone = { inner: 0, outer: 0 };
    next.smoothing_alpha = 0;
    next.curve = deepClone(DEFAULT_AXIS_CONFIG.curve);
  });
});

bindModifierControls();

elements.wizardStartBtn.addEventListener('click', () => guarded(async () => {
  await wizard.begin(elements.targetSelect.value);
}, 'Failed to start mapping wizard'));

elements.wizardRetryBtn.addEventListener('click', () => guarded(async () => {
  await wizard.begin(elements.targetSelect.value);
}, 'Failed to retry detection'));

elements.wizardConfirmBtn.addEventListener('click', () => guarded(async () => {
  await wizard.confirm();
  log('Mapping applied', `${targetByKey(wizard.targetKey).label} -> device ${wizard.candidate.deviceId} element ${wizard.candidate.elementId}`);
}, 'Failed to apply mapping'));

elements.clearLogBtn.addEventListener('click', () => {
  elements.logPanel.textContent = '';
});

render();
