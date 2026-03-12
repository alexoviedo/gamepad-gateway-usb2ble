import * as THREE from 'https://unpkg.com/three@0.161.0/build/three.module.js';
import { errorMessage, requireElement } from './shared/dom.js';

/** @import { AxisConfig, BezierCurve, BridgeCommandRequest, BridgeCommandResponse, CharacteristicMap, ChunkAssemblyState, ChunkedMessage, OutputKey, PendingJsonRequest, ProfileConfig, StreamSample, ValidationMappedOutput, ValidationMappedOutputMap, ValidationOutputMeta } from './shared/hotas-types.js' */

const THREE_NS = /** @type {any} */ (THREE);

/** @typedef {'x'|'y'|'z'|'rx'|'ry'|'rz'|'slider1'|'slider2'} SceneAxisKey */
/** @typedef {Record<SceneAxisKey, number>} SceneAxisState */

/** @param {unknown} value
 * @returns {Record<string, unknown>}
 */
function asRecord(value) {
  return value && typeof value === 'object' && !Array.isArray(value)
    ? /** @type {Record<string, unknown>} */ (value)
    : {};
}

/** @returns {ValidationMappedOutput} */
function createEmptyMappedOutput() {
  return {
    raw: 0,
    deadzoned: 0,
    curved: 0,
    smoothed: 0,
    updatedAt: null,
    configured: false,
    mapping: normalizeAxisConfig(),
    sample: null,
  };
}

const UUIDS = {
  service: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1',
  cmd: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2',
  evt: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3',
  stream: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4',
};

/** @type {ValidationOutputMeta[]} */
const OUTPUTS = [
  { key: 'x', label: 'Aileron', kind: 'axis', visual: 'bipolar' },
  { key: 'y', label: 'Elevator', kind: 'axis', visual: 'bipolar' },
  { key: 'z', label: 'Rudder', kind: 'axis', visual: 'bipolar' },
  { key: 'rx', label: 'Left Brake / Aux 1', kind: 'axis', visual: 'bipolar' },
  { key: 'ry', label: 'Right Brake / Aux 2', kind: 'axis', visual: 'bipolar' },
  { key: 'rz', label: 'Twist / Secondary Yaw', kind: 'axis', visual: 'bipolar' },
  { key: 'slider1', label: 'Throttle', kind: 'axis', visual: 'slider' },
  { key: 'slider2', label: 'Aux Slider', kind: 'axis', visual: 'slider' },
  { key: 'hat', label: 'Hat Switch', kind: 'hat', visual: 'hat' },
];

/** @type {AxisConfig} */
const DEFAULT_AXIS_CONFIG = Object.freeze({
  configured: false,
  device_id: 0,
  element_id: 0,
  invert: false,
  deadzone: { inner: 0, outer: 0 },
  smoothing_alpha: 0,
  curve: {
    type: /** @type {'bezier'} */ ('bezier'),
    p1: { x: 0.25, y: 0.25 },
    p2: { x: 0.75, y: 0.75 },
  },
});

const elements = {
  connectBtn: /** @type {HTMLButtonElement} */ (requireElement('connectBtn')),
  reconnectBtn: /** @type {HTMLButtonElement} */ (requireElement('reconnectBtn')),
  disconnectBtn: /** @type {HTMLButtonElement} */ (requireElement('disconnectBtn')),
  refreshConfigBtn: /** @type {HTMLButtonElement} */ (requireElement('refreshConfigBtn')),
  startStreamBtn: /** @type {HTMLButtonElement} */ (requireElement('startStreamBtn')),
  saveBtn: /** @type {HTMLButtonElement} */ (requireElement('saveBtn')),
  rebootRunBtn: /** @type {HTMLButtonElement} */ (requireElement('rebootRunBtn')),
  connBadge: /** @type {HTMLElement} */ (requireElement('connBadge')),
  streamBadge: /** @type {HTMLElement} */ (requireElement('streamBadge')),
  saveBadge: /** @type {HTMLElement} */ (requireElement('saveBadge')),
  modeBadge: /** @type {HTMLElement} */ (requireElement('modeBadge')),
  deviceName: /** @type {HTMLElement} */ (requireElement('deviceName')),
  gattState: /** @type {HTMLElement} */ (requireElement('gattState')),
  configState: /** @type {HTMLElement} */ (requireElement('configState')),
  sampleRate: /** @type {HTMLElement} */ (requireElement('sampleRate')),
  telemetryGrid: /** @type {HTMLElement} */ (requireElement('telemetryGrid')),
  primaryInstrumentGrid: /** @type {HTMLElement} */ (requireElement('primaryInstrumentGrid')),
  secondaryInstrumentGrid: /** @type {HTMLElement} */ (requireElement('secondaryInstrumentGrid')),
  sceneCanvas: /** @type {HTMLElement} */ (requireElement('sceneCanvas')),
  logPanel: /** @type {HTMLElement} */ (requireElement('logPanel')),
  clearLogBtn: /** @type {HTMLButtonElement} */ (requireElement('clearLogBtn')),
  errorBanner: /** @type {HTMLElement} */ (requireElement('errorBanner')),
};

function nowLabel() {
  return new Date().toLocaleTimeString();
}

/** @param {string} message
 * @param {string} [detail='']
 */
function log(message, detail = '') {
  const line = `[${nowLabel()}] ${message}${detail ? ` ${detail}` : ''}`;
  elements.logPanel.textContent += `${line}\n`;
  elements.logPanel.scrollTop = elements.logPanel.scrollHeight;
}

/** @param {string} message */
function showError(message) {
  elements.errorBanner.textContent = message;
  elements.errorBanner.classList.remove('hidden');
  log('ERROR', message);
}

function clearError() {
  elements.errorBanner.textContent = '';
  elements.errorBanner.classList.add('hidden');
}

/** @param {string} value */
function encodeUtf8(value) {
  return new TextEncoder().encode(value);
}

/** @param {Uint8Array} bytes */
function decodeUtf8(bytes) {
  return new TextDecoder().decode(bytes);
}

/** @param {number} value
 * @param {number} min
 * @param {number} max
 * @returns {number}
 */
function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

/** @param {number} value */
function round3(value) {
  return Math.round(value * 1000) / 1000;
}

/** @template T
 * @param {T} value
 * @returns {T}
 */
function deepClone(value) {
  return JSON.parse(JSON.stringify(value));
}

/**
 * @param {unknown} target
 * @param {unknown} patch
 * @returns {unknown}
 */
function deepMerge(target, patch) {
  if (patch == null || typeof patch !== 'object' || Array.isArray(patch)) return patch;
  const out = /** @type {Record<string, unknown>} */ ({ ...asRecord(target) });
  for (const [key, value] of Object.entries(asRecord(patch))) {
    out[key] = value && typeof value === 'object' && !Array.isArray(value)
      ? deepMerge(out[key], value)
      : value;
  }
  return out;
}

/** @param {unknown} config */
function configToStableString(config) {
  return JSON.stringify(config ?? {});
}

/** @param {OutputKey} axisKey */
function isSliderAxis(axisKey) {
  return axisKey === 'slider1' || axisKey === 'slider2';
}

/** @param {Partial<AxisConfig> | null | undefined} [mapping]
 * @returns {AxisConfig}
 */
function normalizeAxisConfig(mapping) {
  const merged = /** @type {AxisConfig & { deadzone?: number | { inner?: number, outer?: number }, curve?: Partial<BezierCurve> }} */ (deepMerge(DEFAULT_AXIS_CONFIG, mapping || {}));
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
  merged.curve = /** @type {BezierCurve} */ (deepMerge(DEFAULT_AXIS_CONFIG.curve, merged.curve || {}));
  merged.curve.p1 = {
    x: clamp(Number(merged.curve?.p1?.x ?? 0.25), 0, 1),
    y: clamp(Number(merged.curve?.p1?.y ?? 0.25), 0, 1),
  };
  merged.curve.p2 = {
    x: clamp(Number(merged.curve?.p2?.x ?? 0.75), 0, 1),
    y: clamp(Number(merged.curve?.p2?.y ?? 0.75), 0, 1),
  };
  merged.curve.type = 'bezier';
  merged.device_id = Number(merged.device_id || 0);
  merged.element_id = Number(merged.element_id || 0);
  merged.invert = Boolean(merged.invert);
  merged.configured = Boolean(merged.configured && merged.device_id && merged.element_id != null);
  return merged;
}

/** @param {unknown} [config]
 * @returns {ProfileConfig}
 */
function ensureConfigShape(config) {
  const shaped = /** @type {ProfileConfig} */ (deepClone(config || { version: 2, axes: {} }));
  shaped.version = 2;
  shaped.axes = shaped.axes || {};
  for (const output of OUTPUTS) {
    if (shaped.axes[output.key]) {
      shaped.axes[output.key] = normalizeAxisConfig(shaped.axes[output.key]);
    }
  }
  return shaped;
}

/** @param {number} a
 * @param {number} b
 * @param {number} c
 * @param {number} d
 * @param {number} t
 * @returns {number}
 */
function cubicBezier(a, b, c, d, t) {
  const mt = 1 - t;
  return mt * mt * mt * a + 3 * mt * mt * t * b + 3 * mt * t * t * c + t * t * t * d;
}

/** @param {number} x
 * @param {BezierCurve | null | undefined} curve
 * @returns {number}
 */
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

/** @param {number} v
 * @param {number} inner
 * @param {number} outer
 * @returns {number}
 */
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

/** @param {number} v
 * @param {number} inner
 * @param {number} outer
 * @returns {number}
 */
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

/** @param {StreamSample | null} sample
 * @param {OutputKey} axisKey
 * @param {AxisConfig} mapping
 * @param {number | null} [previousSmoothed=null]
 */
function computeMappedOutput(sample, axisKey, mapping, previousSmoothed = null) {
  if (!sample || !mapping?.configured) {
    return {
      raw: isSliderAxis(axisKey) ? 0 : 0,
      deadzoned: 0,
      curved: 0,
      smoothed: 0,
      updatedAt: null,
    };
  }

  const slider = isSliderAxis(axisKey);
  let raw = slider ? clamp((sample.norm + 1) * 0.5, 0, 1) : clamp(sample.norm, -1, 1);
  if (mapping.invert) raw = slider ? 1 - raw : -raw;

  const deadzoned = slider
    ? applyDeadzoneUnipolar(raw, mapping.deadzone.inner, mapping.deadzone.outer)
    : applyDeadzoneBipolar(raw, mapping.deadzone.inner, mapping.deadzone.outer);

  const curved = slider
    ? applyBezier01(deadzoned, mapping.curve)
    : Math.sign(deadzoned) * applyBezier01(Math.abs(deadzoned), mapping.curve);

  const smoothed = mapping.smoothing_alpha <= 0 || previousSmoothed == null
    ? curved
    : previousSmoothed * (1 - mapping.smoothing_alpha) + curved * mapping.smoothing_alpha;

  return {
    raw,
    deadzoned,
    curved,
    smoothed,
    updatedAt: sample.receivedAt,
  };
}

class ChunkAssembler {
  constructor() {
    /** @type {Map<number, ChunkAssemblyState>} */
    this.messages = new Map();
  }

  /** @param {Uint8Array} frameBytes
   * @returns {ChunkedMessage | null}
   */
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
    if (!state) return null;
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

class BridgeClient {
  constructor() {
    /** @type {BluetoothDevice | null} */
    this.device = null;
    /** @type {BluetoothRemoteGATTServer | null} */
    this.server = null;
    /** @type {BluetoothRemoteGATTService | null} */
    this.service = null;
    /** @type {CharacteristicMap} */
    this.characteristics = {};
    /** @type {ChunkAssembler} */
    this.chunkAssembler = new ChunkAssembler();
    /** @type {Map<number, PendingJsonRequest>} */
    this.pendingJson = new Map();
    /** @type {number} */
    this.requestId = 0;
    /** @type {boolean} */
    this.streamActive = false;
    /** @type {boolean} */
    this.reconnectWanted = false;
    /** @type {ProfileConfig} */
    this.currentConfig = ensureConfigShape();
    /** @type {string} */
    this.savedConfigString = configToStableString(this.currentConfig);
    /** @type {boolean} */
    this.pendingChanges = false;
    /** @type {Map<string, StreamSample>} */
    this.latestSampleByKey = new Map();
    /** @type {StreamSample | null} */
    this.latestSample = null;
    /** @type {number[]} */
    this.sampleTimestamps = [];
    /** @type {() => Promise<void>} */
    this.onDisconnectedBound = this.onDisconnected.bind(this);
    /** @type {(event: Event) => void} */
    this.handleEvtBound = (event) => {
      const target = /** @type {BluetoothRemoteGATTCharacteristic} */ (event.target);
      const value = target.value;
      if (!value) return;
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      this.handleEvtNotification(bytes);
    };
    /** @type {(event: Event) => void} */
    this.handleStreamBound = (event) => {
      const target = /** @type {BluetoothRemoteGATTCharacteristic} */ (event.target);
      const value = target.value;
      if (!value) return;
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      this.handleStreamNotification(bytes);
    };
  }

  async requestDevice() {
    if (!navigator.bluetooth) {
      throw new Error('Web Bluetooth is not available in this browser. Use Chrome or Edge on desktop.');
    }
    this.device = await navigator.bluetooth.requestDevice({
      filters: [
        { services: [UUIDS.service] },
        { namePrefix: 'HOTAS' },
      ],
      optionalServices: [UUIDS.service],
    });
    this.device.removeEventListener('gattserverdisconnected', this.onDisconnectedBound);
    this.device.addEventListener('gattserverdisconnected', this.onDisconnectedBound);
    this.reconnectWanted = true;
    log('Device selected', this.device.name || this.device.id);
  }

  async connect({ reuseDevice = false } = {}) {
    clearError();
    if (!reuseDevice || !this.device) {
      await this.requestDevice();
    }
    if (!this.device) throw new Error('No Bluetooth device selected.');

    if (!this.device.gatt) throw new Error('Selected Bluetooth device does not expose GATT.');
    this.server = await this.device.gatt.connect();
    this.service = await this.server.getPrimaryService(UUIDS.service);
    this.characteristics.cmd = await this.service.getCharacteristic(UUIDS.cmd);
    this.characteristics.evt = await this.service.getCharacteristic(UUIDS.evt);
    this.characteristics.stream = await this.service.getCharacteristic(UUIDS.stream);

    await this.characteristics.evt.startNotifications();
    this.characteristics.evt.addEventListener('characteristicvaluechanged', this.handleEvtBound);

    await this.characteristics.stream.startNotifications();
    this.characteristics.stream.addEventListener('characteristicvaluechanged', this.handleStreamBound);

    log('Connected and notifications enabled');
  }

  async reconnect() {
    if (!this.device) throw new Error('No device to reconnect to.');
    return this.connect({ reuseDevice: true });
  }

  async disconnect({ intentional = true } = {}) {
    this.streamActive = false;
    if (intentional) this.reconnectWanted = false;
    try {
      if (this.device?.gatt?.connected) this.device.gatt.disconnect();
    } catch (error) {
      log('Disconnect warning', errorMessage(error));
    }
    this.server = null;
    this.service = null;
    this.characteristics = {};
  }

  async onDisconnected() {
    log('Device disconnected');
    this.server = null;
    this.service = null;
    this.characteristics = {};
    this.streamActive = false;
    render();
  }

/** @param {BridgeCommandRequest} command
   * @returns {Promise<BridgeCommandResponse>}
   */
  async sendCommand(command) {
    if (!this.characteristics.cmd) throw new Error('Not connected to the Config Service.');
    const rid = ++this.requestId;
    const payload = { rid, ...command };
    const json = JSON.stringify(payload);
    const responsePromise = /** @type {Promise<BridgeCommandResponse>} */ (new Promise((resolve, reject) => {
      this.pendingJson.set(rid, { resolve, reject });
      setTimeout(() => {
        if (this.pendingJson.has(rid)) {
          this.pendingJson.delete(rid);
          reject(new Error(`Timed out waiting for response to ${payload.cmd}`));
        }
      }, 8000);
    }));
    await this.characteristics.cmd.writeValue(encodeUtf8(json));
    log('CMD →', json);
    return responsePromise;
  }

  async getConfig({ markSaved = true } = {}) {
    const response = await this.sendCommand({ cmd: 'get_config' });
    if (response.config && typeof response.config === 'object') {
      this.currentConfig = ensureConfigShape(response.config);
    } else if (response.config_json) {
      this.currentConfig = ensureConfigShape(JSON.parse(response.config_json));
    }
    if (markSaved) {
      this.savedConfigString = configToStableString(this.currentConfig);
      this.pendingChanges = false;
    }
    return response;
  }

  async startStream() {
    const response = await this.sendCommand({ cmd: 'start_stream' });
    this.streamActive = true;
    return response;
  }

  async saveProfile() {
    const response = await this.sendCommand({ cmd: 'save_profile' });
    if (response.ok) {
      this.savedConfigString = configToStableString(this.currentConfig);
      this.pendingChanges = false;
    }
    return response;
  }

  async rebootToRun() {
    return this.sendCommand({ cmd: 'reboot_to_run' });
  }

/** @param {Uint8Array} bytes */
  handleEvtNotification(bytes) {
    const complete = this.chunkAssembler.push(bytes);
    if (!complete) return;
    if (complete.type !== 1) return;

    const text = decodeUtf8(complete.bytes);
    log('EVT JSON ←', text);
    /** @type {BridgeCommandResponse} */
    let payload;
    try {
      payload = JSON.parse(text);
    } catch (error) {
      showError(`Malformed JSON EVT payload: ${errorMessage(error)}`);
      return;
    }

    if (typeof payload?.rid === 'number' && this.pendingJson.has(payload.rid)) {
      const pending = this.pendingJson.get(payload.rid);
      this.pendingJson.delete(payload.rid);
      if (pending) pending.resolve(payload);
    }

    if (payload?.config && typeof payload.config === 'object') {
      this.currentConfig = ensureConfigShape(payload.config);
      this.pendingChanges = configToStableString(this.currentConfig) !== this.savedConfigString;
    }

    render();
  }

/** @param {Uint8Array} bytes */
  handleStreamNotification(bytes) {
    if (bytes.byteLength < 16) return;
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const sample = /** @type {StreamSample} */ ({
      version: view.getUint8(0),
      flags: view.getUint8(1),
      deviceId: view.getUint32(2, true),
      elementId: view.getUint32(6, true),
      raw: view.getInt32(10, true),
      normQ15: view.getInt16(14, true),
      norm: view.getInt16(14, true) / 32767,
      receivedAt: new Date(),
    });
    this.latestSample = sample;
    this.latestSampleByKey.set(`${sample.deviceId}:${sample.elementId}`, sample);
    const now = Date.now();
    this.sampleTimestamps.push(now);
    while (this.sampleTimestamps.length && now - this.sampleTimestamps[0] > 1000) {
      this.sampleTimestamps.shift();
    }
  }
}


class ValidationScene {
  /** @param {HTMLElement} container */
  constructor(container) {
    /** @type {HTMLElement} */
    this.container = container;
    this.focusPoint = new THREE_NS.Vector3(0, 1.0, 0.05);

    const { width, height } = this.getContainerSize();
    this.renderer = new THREE_NS.WebGLRenderer({ antialias: true, alpha: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.setSize(width, height, false);
    this.renderer.outputColorSpace = THREE_NS.SRGBColorSpace;
    this.renderer.domElement.style.width = '100%';
    this.renderer.domElement.style.height = '100%';
    container.appendChild(this.renderer.domElement);

    this.scene = new THREE_NS.Scene();
    this.camera = new THREE_NS.PerspectiveCamera(29, width / height, 0.1, 60);
    this.camera.position.set(0, 4.9, 10.2);
    this.camera.lookAt(this.focusPoint);

    this.clock = new THREE_NS.Clock();
    /** @type {SceneAxisState} */
    this.target = { x: 0, y: 0, z: 0, rx: 0, ry: 0, rz: 0, slider1: 0, slider2: 0 };
    /** @type {SceneAxisState} */
    this.state = { x: 0, y: 0, z: 0, rx: 0, ry: 0, rz: 0, slider1: 0, slider2: 0 };

    this.buildScene();
    this.resizeObserver = new ResizeObserver(() => this.onResize());
    this.resizeObserver.observe(container);
    window.addEventListener('resize', () => this.onResize());
    this.onResize();
    this.animate();
  }

  getContainerSize() {
    const rect = this.container.getBoundingClientRect();
    return {
      width: Math.max(360, Math.round(rect.width || this.container.clientWidth || 960)),
      height: Math.max(460, Math.round(rect.height || this.container.clientHeight || 640)),
    };
  }

  buildScene() {
    const hemi = new THREE_NS.HemisphereLight(0x91a9ff, 0x09111f, 1.0);
    this.scene.add(hemi);

    const key = new THREE_NS.DirectionalLight(0xe5edff, 1.3);
    key.position.set(4.5, 8.0, 6.2);
    this.scene.add(key);

    const fill = new THREE_NS.DirectionalLight(0x39c2c9, 0.36);
    fill.position.set(-5.4, 4.2, 2.4);
    this.scene.add(fill);

    const ambientGlow = new THREE_NS.Mesh(
      new THREE_NS.CircleGeometry(9.5, 64),
      new THREE_NS.MeshBasicMaterial({ color: 0x12213f, transparent: true, opacity: 0.34 })
    );
    ambientGlow.rotation.x = -Math.PI / 2;
    ambientGlow.position.y = -0.06;
    this.scene.add(ambientGlow);

    const floor = new THREE_NS.Mesh(
      new THREE_NS.CircleGeometry(9.1, 72),
      new THREE_NS.MeshStandardMaterial({ color: 0x0a1221, metalness: 0.08, roughness: 0.95 })
    );
    floor.rotation.x = -Math.PI / 2;
    floor.position.y = -0.02;
    this.scene.add(floor);

    const stageMat = new THREE_NS.MeshStandardMaterial({ color: 0x121c2e, metalness: 0.1, roughness: 0.9 });
    const surfaceMat = new THREE_NS.MeshStandardMaterial({ color: 0x5f78da, metalness: 0.28, roughness: 0.34 });
    const accentMat = new THREE_NS.MeshStandardMaterial({ color: 0x35b7be, metalness: 0.12, roughness: 0.42 });
    const trimMat = new THREE_NS.MeshStandardMaterial({ color: 0x1d335f, metalness: 0.14, roughness: 0.62 });

    const mainDeck = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(8.6, 0.28, 4.8), stageMat);
    mainDeck.position.set(0, 0.14, 0.08);
    this.scene.add(mainDeck);

    const deckInset = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(7.5, 0.02, 3.72), trimMat);
    deckInset.position.set(0, 0.29, 0.05);
    this.scene.add(deckInset);

    const guideRail = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(5.8, 0.03, 0.1), trimMat);
    guideRail.position.set(0, 0.3, -0.7);
    this.scene.add(guideRail);

    const stickPlate = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(1.95, 0.2, 1.95), stageMat);
    stickPlate.position.set(-2.75, 0.32, 0.95);
    this.scene.add(stickPlate);
    const stickTop = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(1.24, 0.06, 1.04), surfaceMat);
    stickTop.position.set(-2.9, 0.45, 1.02);
    stickTop.rotation.z = -0.04;
    this.scene.add(stickTop);

    this.stickGimbal = new THREE_NS.Group();
    this.stickGimbal.position.set(-2.58, 0.44, 1.0);
    this.scene.add(this.stickGimbal);

    const stickBoot = new THREE_NS.Mesh(new THREE_NS.CylinderGeometry(0.16, 0.2, 0.22, 18), trimMat);
    stickBoot.position.y = 0.1;
    this.stickGimbal.add(stickBoot);

    const stickShaft = new THREE_NS.Mesh(new THREE_NS.CylinderGeometry(0.11, 0.13, 1.3, 18), surfaceMat);
    stickShaft.position.y = 0.8;
    this.stickGimbal.add(stickShaft);

    const stickGrip = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.4, 0.7, 0.34), accentMat);
    stickGrip.position.set(0.08, 1.56, 0.02);
    stickGrip.rotation.z = -0.22;
    this.stickGimbal.add(stickGrip);

    const throttlePlate = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(2.15, 0.2, 1.95), stageMat);
    throttlePlate.position.set(2.75, 0.32, 0.95);
    this.scene.add(throttlePlate);

    const throttleTop = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(1.85, 0.06, 0.92), trimMat);
    throttleTop.position.set(2.75, 0.45, 1.0);
    this.scene.add(throttleTop);

    const throttleTrack = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(1.55, 0.08, 0.18), surfaceMat);
    throttleTrack.position.set(2.68, 0.48, 0.92);
    this.scene.add(throttleTrack);

    this.throttleCarriage = new THREE_NS.Group();
    this.throttleCarriage.position.set(2.12, 0.49, 0.92);
    this.scene.add(this.throttleCarriage);

    const carriage = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.2, 0.18, 0.34), surfaceMat);
    carriage.position.set(0, 0.08, 0);
    this.throttleCarriage.add(carriage);

    const throttleStem = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.16, 0.72, 0.16), surfaceMat);
    throttleStem.position.set(0, 0.46, 0);
    this.throttleCarriage.add(throttleStem);

    const throttleGrip = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.34, 0.42, 0.38), accentMat);
    throttleGrip.position.set(0.08, 0.9, 0.02);
    throttleGrip.rotation.z = -0.18;
    this.throttleCarriage.add(throttleGrip);

    const pedalPlate = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(3.95, 0.2, 1.7), stageMat);
    pedalPlate.position.set(0, 0.32, -1.12);
    this.scene.add(pedalPlate);

    this.pedalYaw = new THREE_NS.Group();
    this.pedalYaw.position.set(0, 0.44, -1.0);
    this.scene.add(this.pedalYaw);

    const pedalCrossbar = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(2.65, 0.12, 0.14), surfaceMat);
    pedalCrossbar.position.set(0, 0.06, 0.02);
    this.pedalYaw.add(pedalCrossbar);

    this.leftPedal = new THREE_NS.Group();
    this.leftPedal.position.set(-1.02, 0, 0.02);
    this.pedalYaw.add(this.leftPedal);

    this.rightPedal = new THREE_NS.Group();
    this.rightPedal.position.set(1.02, 0, 0.02);
    this.pedalYaw.add(this.rightPedal);

    /** @param {any} group
     * @param {number} [mirror=1]
     */
    const buildPedal = (group, mirror = 1) => {
      const arm = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.18, 0.14, 0.78), surfaceMat);
      arm.position.set(0, 0.02, 0.24);
      group.add(arm);

      const pedalFace = new THREE_NS.Group();
      pedalFace.position.set(0, 0.13, 0.54);
      group.add(pedalFace);

      const face = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.84, 0.92, 0.12), accentMat);
      face.position.set(0, 0.4, 0);
      face.rotation.x = -0.2;
      pedalFace.add(face);

      const toePivot = new THREE_NS.Group();
      toePivot.position.set(0, 0.82, 0.06);
      pedalFace.add(toePivot);

      const toe = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.84, 0.22, 0.12), accentMat);
      toe.position.set(0, 0.08, 0.04);
      toePivot.add(toe);

      const heel = new THREE_NS.Mesh(new THREE_NS.BoxGeometry(0.32, 0.08, 0.22), trimMat);
      heel.position.set(0, -0.02, -0.24 * mirror);
      group.add(heel);

      return toePivot;
    };

    this.leftToePivot = buildPedal(this.leftPedal, -1);
    this.rightToePivot = buildPedal(this.rightPedal, 1);

    const backArc = new THREE_NS.Mesh(
      new THREE_NS.TorusGeometry(6.1, 0.08, 16, 72, Math.PI),
      new THREE_NS.MeshBasicMaterial({ color: 0x24385b, transparent: true, opacity: 0.4 })
    );
    backArc.rotation.x = Math.PI / 2;
    backArc.rotation.z = Math.PI;
    backArc.position.set(0, 0.34, -0.2);
    this.scene.add(backArc);
  }

/** @param {ValidationMappedOutputMap} mapped */
  setTargets(mapped) {
    for (const key of /** @type {SceneAxisKey[]} */ (Object.keys(this.target))) {
      this.target[key] = mapped[key]?.smoothed ?? 0;
    }
  }

  onResize() {
    const { width, height } = this.getContainerSize();
    this.camera.aspect = width / height;
    this.camera.updateProjectionMatrix();
    this.camera.lookAt(this.focusPoint);
    this.renderer.setSize(width, height, false);
  }

  animate() {
    requestAnimationFrame(() => this.animate());
    const dt = Math.min(this.clock.getDelta(), 0.05);
    const damp = 1 - Math.exp(-8 * dt);
    for (const key of /** @type {SceneAxisKey[]} */ (Object.keys(this.state))) {
      this.state[key] += (this.target[key] - this.state[key]) * damp;
    }

    this.stickGimbal.rotation.z = -this.state.x * 0.42;
    this.stickGimbal.rotation.x = this.state.y * 0.34;

    this.throttleCarriage.position.x = THREE_NS.MathUtils.lerp(2.12, 3.18, clamp((this.state.slider1 + 1) * 0.5, 0, 1));

    this.pedalYaw.rotation.y = this.state.z * 0.28;
    this.leftToePivot.rotation.x = clamp((this.state.rx + 1) * 0.5, 0, 1) * 0.45;
    this.rightToePivot.rotation.x = clamp((this.state.ry + 1) * 0.5, 0, 1) * 0.45;

    this.camera.lookAt(this.focusPoint);
    this.renderer.render(this.scene, this.camera);
  }
}

const client = new BridgeClient();
const scene = new ValidationScene(elements.sceneCanvas);
/** @type {ValidationMappedOutputMap} */
let mappedOutputs = {};

/** @param {OutputKey} output
 * @returns {ValidationOutputMeta}
 */
function outputMeta(output) {
  return OUTPUTS.find((item) => item.key === output) || OUTPUTS[0];
}

function computeMappedOutputs() {
  const config = ensureConfigShape(client.currentConfig);
  client.currentConfig = config;
  /** @type {ValidationMappedOutputMap} */
  const next = {};

  for (const output of OUTPUTS) {
    const mapping = normalizeAxisConfig(config.axes[output.key]);
    const sample = mapping.configured
      ? client.latestSampleByKey.get(`${mapping.device_id}:${mapping.element_id}`) || null
      : null;
    const previousSmoothed = mappedOutputs[output.key]?.smoothed ?? null;
    next[output.key] = {
      ...computeMappedOutput(sample, output.key, mapping, previousSmoothed),
      configured: mapping.configured,
      mapping,
      sample,
    };
  }

  mappedOutputs = next;
  scene.setTargets(mappedOutputs);
}

/** @param {StreamSample | null | undefined} sample */
function sampleSourceLabel(sample) {
  return sample ? `${sample.deviceId}:${sample.elementId}` : '—';
}

/** @param {ValidationMappedOutput | null | undefined} state */
function isRecentUpdate(state) {
  if (!state?.updatedAt) return false;
  const ts = state.updatedAt instanceof Date ? state.updatedAt.getTime() : new Date(state.updatedAt).getTime();
  if (!Number.isFinite(ts)) return false;
  return Date.now() - ts < 550;
}


function renderTelemetry() {
  elements.telemetryGrid.innerHTML = '';

  for (const output of OUTPUTS) {
    const state = mappedOutputs[output.key] || createEmptyMappedOutput();
    const slider = output.visual === 'slider';
    const progress = slider ? clamp(state.smoothed, 0, 1) : clamp(Math.abs(state.smoothed), 0, 1);
    const direction = slider ? 1 : (state.smoothed >= 0 ? 1 : -1);
    const thumb = slider
      ? `${clamp((state.smoothed ?? 0), 0, 1) * 100}%`
      : `${(clamp(state.smoothed ?? 0, -1, 1) + 1) * 50}%`;
    const fillTransform = slider
      ? `scaleX(${clamp(progress, 0, 1)})`
      : `scaleX(${direction * clamp(progress, 0, 1)})`;

    const card = document.createElement('article');
    card.className = `summary-card-item ${isRecentUpdate(state) ? 'active' : ''}`;
    card.innerHTML = `
      <div class="summary-card-top">
        <div class="summary-card-title">
          <strong>${output.label}</strong>
          <span>${output.key.toUpperCase()} output</span>
        </div>
        <span class="summary-state">${state.configured ? 'Mapped' : 'Unmapped'}</span>
      </div>
      <div class="summary-meter ${slider ? 'slider' : 'bipolar'}">
        <div class="summary-fill ${slider ? 'slider' : 'bipolar'}" style="transform:${fillTransform};"></div>
        <div class="summary-thumb" style="left:${thumb};"></div>
      </div>
      <div class="summary-meta">
        <span>Output<strong>${round3(state.smoothed ?? 0)}</strong></span>
        <span>Curve<strong>${round3(state.curved ?? 0)}</strong></span>
        <span>Source<em>${sampleSourceLabel(state.sample)}</em></span>
      </div>
    `;
    elements.telemetryGrid.appendChild(card);
  }
}

/** @param {{ title: string, subtitle: string, stateLabel: string, active: boolean, primary?: boolean }} options */
function createInstrumentCard({ title, subtitle, stateLabel, active, primary = false }) {
  const card = document.createElement('article');
  card.className = `instrument-card ${primary ? 'primary' : ''} ${active ? 'active' : ''}`.trim();
  card.innerHTML = `
    <div class="instrument-header">
      <div class="instrument-title">
        <strong>${title}</strong>
        <span>${subtitle}</span>
      </div>
      <span class="instrument-state">${stateLabel}</span>
    </div>
  `;
  return card;
}

/** @param {ValidationMappedOutput} state
 * @param {{ slider?: boolean }} [options={}]
 */
function buildAxisTrackMarkup(state, { slider = false } = {}) {
  const fillTransform = slider
    ? `scaleX(${clamp(state.smoothed ?? 0, 0, 1)})`
    : `scaleX(${(state.smoothed ?? 0) >= 0 ? 1 : -1} * ${clamp(Math.abs(state.smoothed ?? 0), 0, 1)})`;
  const thumbLeft = slider
    ? `${clamp(state.smoothed ?? 0, 0, 1) * 100}%`
    : `${(clamp(state.smoothed ?? 0, -1, 1) + 1) * 50}%`;
  return `
    <div class="meter-track ${slider ? 'unipolar' : 'bipolar'}">
      <div class="meter-fill ${slider ? 'unipolar' : 'bipolar'}" style="transform:${fillTransform};"></div>
      <div class="meter-thumb" style="left:${thumbLeft};"></div>
    </div>
    <div class="meter-scale">
      ${slider ? '<span>Min</span><span>Mid</span><span>Max</span>' : '<span>-1</span><span>0</span><span>+1</span>'}
    </div>
  `;
}

function createStickInstrument() {
  const xState = mappedOutputs.x || createEmptyMappedOutput();
  const yState = mappedOutputs.y || createEmptyMappedOutput();
  const active = isRecentUpdate(xState) || isRecentUpdate(yState);
  const mapped = xState.configured || yState.configured;

  const card = createInstrumentCard({
    title: 'Stick XY',
    subtitle: 'Aileron + elevator preview',
    stateLabel: mapped ? (active ? 'Live' : 'Mapped') : 'Unmapped',
    active,
    primary: true,
  });

  const body = document.createElement('div');
  body.className = 'instrument-body';

  const pad = document.createElement('div');
  pad.className = 'xy-pad';
  pad.innerHTML = '<div class="xy-rings"></div>';
  const dot = document.createElement('div');
  dot.className = 'xy-dot';
  dot.style.left = `${(clamp(xState.smoothed ?? 0, -1, 1) + 1) * 50}%`;
  dot.style.top = `${(1 - (clamp(yState.smoothed ?? 0, -1, 1) + 1) * 0.5) * 100}%`;
  pad.appendChild(dot);
  body.appendChild(pad);

  const values = document.createElement('div');
  values.className = 'xy-values';
  values.innerHTML = `
    <div class="instrument-stats two-col">
      <span>Aileron<strong>${round3(xState.smoothed ?? 0)}</strong></span>
      <span>Source<em>${sampleSourceLabel(xState.sample)}</em></span>
    </div>
    <div class="instrument-stats two-col">
      <span>Elevator<strong>${round3(yState.smoothed ?? 0)}</strong></span>
      <span>Source<em>${sampleSourceLabel(yState.sample)}</em></span>
    </div>
  `;
  body.appendChild(values);
  card.appendChild(body);
  return card;
}

function createPedalsInstrument() {
  const rudder = mappedOutputs.z || createEmptyMappedOutput();
  const leftBrake = mappedOutputs.rx || createEmptyMappedOutput();
  const rightBrake = mappedOutputs.ry || createEmptyMappedOutput();
  const active = isRecentUpdate(rudder) || isRecentUpdate(leftBrake) || isRecentUpdate(rightBrake);
  const mapped = rudder.configured || leftBrake.configured || rightBrake.configured;

  const card = createInstrumentCard({
    title: 'Pedals',
    subtitle: 'Rudder yaw + toe brakes',
    stateLabel: mapped ? (active ? 'Live' : 'Mapped') : 'Unmapped',
    active,
    primary: true,
  });

  const body = document.createElement('div');
  body.className = 'instrument-body pedals-layout';

  const rudderGauge = document.createElement('div');
  rudderGauge.className = 'meter-card';
  rudderGauge.innerHTML = `
    ${buildAxisTrackMarkup(rudder)}
    <div class="instrument-stats three-col">
      <span>Raw<strong>${round3(rudder.raw ?? 0)}</strong></span>
      <span>Curved<strong>${round3(rudder.curved ?? 0)}</strong></span>
      <span>Smoothed<strong>${round3(rudder.smoothed ?? 0)}</strong></span>
    </div>
  `;
  body.appendChild(rudderGauge);

  const brakes = document.createElement('div');
  brakes.className = 'brake-row';
  for (const item of [
    { label: 'Left Toe Brake', state: leftBrake },
    { label: 'Right Toe Brake', state: rightBrake },
  ]) {
    const normalized = clamp(((item.state.smoothed ?? 0) + 1) * 0.5, 0, 1);
    const brakeCard = document.createElement('div');
    brakeCard.className = 'brake-card';
    brakeCard.innerHTML = `
      <label>${item.label}</label>
      <div class="brake-track">
        <div class="brake-fill" style="transform:scaleX(${normalized});"></div>
      </div>
      <div class="brake-scale">
        <span>0</span><span>${round3(item.state.smoothed ?? 0)}</span><span>1</span>
      </div>
    `;
    brakes.appendChild(brakeCard);
  }
  body.appendChild(brakes);

  const meta = document.createElement('div');
  meta.className = 'instrument-meta two-col';
  meta.innerHTML = `
    <span>Rudder Source<em>${sampleSourceLabel(rudder.sample)}</em></span>
    <span>Brake Sources<em>${sampleSourceLabel(leftBrake.sample)} / ${sampleSourceLabel(rightBrake.sample)}</em></span>
  `;
  body.appendChild(meta);

  card.appendChild(body);
  return card;
}

function createThrottleInstrument() {
  const state = mappedOutputs.slider1 || createEmptyMappedOutput();
  const active = isRecentUpdate(state);
  const card = createInstrumentCard({
    title: 'Throttle',
    subtitle: 'Primary power axis',
    stateLabel: state.configured ? (active ? 'Live' : 'Mapped') : 'Unmapped',
    active,
    primary: true,
  });

  const body = document.createElement('div');
  body.className = 'instrument-body';
  body.innerHTML = `
    <div class="meter-card">
      ${buildAxisTrackMarkup(state, { slider: true })}
      <div class="instrument-stats three-col">
        <span>Raw<strong>${round3(state.raw ?? 0)}</strong></span>
        <span>Curved<strong>${round3(state.curved ?? 0)}</strong></span>
        <span>Smoothed<strong>${round3(state.smoothed ?? 0)}</strong></span>
      </div>
    </div>
    <div class="instrument-meta two-col">
      <span>Output<strong>${round3(state.smoothed ?? 0)}</strong></span>
      <span>Source<em>${sampleSourceLabel(state.sample)}</em></span>
    </div>
  `;
  card.appendChild(body);
  return card;
}

/** @param {OutputKey} outputKey */
function createCompactAxisInstrument(outputKey) {
  const meta = outputMeta(outputKey);
  const state = mappedOutputs[outputKey] || createEmptyMappedOutput();
  const active = isRecentUpdate(state);
  const slider = meta.visual === 'slider';
  const card = createInstrumentCard({
    title: meta.label,
    subtitle: `${meta.key.toUpperCase()} output`,
    stateLabel: state.configured ? (active ? 'Live' : 'Mapped') : 'Unmapped',
    active,
  });

  const body = document.createElement('div');
  body.className = 'instrument-body';
  body.innerHTML = `
    <div class="meter-card">
      ${buildAxisTrackMarkup(state, { slider })}
      <div class="instrument-stats two-col">
        <span>Output<strong>${round3(state.smoothed ?? 0)}</strong></span>
        <span>Source<em>${sampleSourceLabel(state.sample)}</em></span>
      </div>
    </div>
  `;
  card.appendChild(body);
  return card;
}

function createHatInstrument() {
  const state = mappedOutputs.hat || createEmptyMappedOutput();
  const active = isRecentUpdate(state);
  const card = createInstrumentCard({
    title: 'Hat Switch',
    subtitle: 'Directional preview',
    stateLabel: state.configured ? (active ? 'Live' : 'Mapped') : 'Unmapped',
    active,
  });

  const body = document.createElement('div');
  body.className = 'instrument-body';
  const grid = document.createElement('div');
  grid.className = 'hat-grid';
  /** @type {Record<number, number>} */
  const hatMap = { 1: 1, 2: 2, 3: 5, 4: 8, 5: 7, 6: 6, 7: 3, 8: 0 };
  const activeIndex = hatMap[Math.round(state.smoothed || 0)] ?? -1;
  for (let index = 0; index < 9; index += 1) {
    const cell = document.createElement('div');
    cell.className = `hat-cell ${index === activeIndex ? 'active' : ''}`;
    if (index === 4) cell.style.opacity = '0.35';
    grid.appendChild(cell);
  }
  body.appendChild(grid);

  const stats = document.createElement('div');
  stats.className = 'instrument-stats two-col';
  stats.innerHTML = `
    <span>State<strong>${Math.round(state.smoothed || 0)}</strong></span>
    <span>Source<em>${sampleSourceLabel(state.sample)}</em></span>
  `;
  body.appendChild(stats);
  card.appendChild(body);
  return card;
}

function renderInstrumentDeck() {
  elements.primaryInstrumentGrid.innerHTML = '';
  elements.secondaryInstrumentGrid.innerHTML = '';

  elements.primaryInstrumentGrid.appendChild(createStickInstrument());
  elements.primaryInstrumentGrid.appendChild(createPedalsInstrument());
  elements.primaryInstrumentGrid.appendChild(createThrottleInstrument());

  elements.secondaryInstrumentGrid.appendChild(createCompactAxisInstrument('rz'));
  elements.secondaryInstrumentGrid.appendChild(createCompactAxisInstrument('slider2'));
  elements.secondaryInstrumentGrid.appendChild(createHatInstrument());
}

function renderConnectionState() {
  const connected = Boolean(client.device?.gatt?.connected && client.service);
  elements.connBadge.textContent = connected ? 'Connected' : 'Disconnected';
  elements.connBadge.className = `pill ${connected ? 'success' : 'warn'}`;
  elements.streamBadge.textContent = client.streamActive ? 'Stream active' : 'Stream idle';
  elements.streamBadge.className = `pill ${client.streamActive ? 'success' : 'muted'}`;
  elements.saveBadge.textContent = client.pendingChanges ? 'Pending changes' : 'Saved state';
  elements.saveBadge.className = `pill ${client.pendingChanges ? 'warn' : 'success'}`;
  elements.modeBadge.textContent = connected ? 'CONFIG mode session' : 'Awaiting connection';
  elements.modeBadge.className = `pill ${connected ? 'accent' : 'muted'}`;
  elements.deviceName.textContent = client.device?.name || client.device?.id || '—';
  elements.gattState.textContent = connected ? 'Connected' : 'Not connected';
  elements.configState.textContent = client.currentConfig ? 'Loaded' : 'Not loaded';
  elements.sampleRate.textContent = client.sampleTimestamps.length ? `~${client.sampleTimestamps.length} samples/s` : 'No samples yet';

  elements.connectBtn.disabled = connected;
  elements.reconnectBtn.disabled = !client.device || connected;
  elements.disconnectBtn.disabled = !connected;
  elements.refreshConfigBtn.disabled = !connected;
  elements.startStreamBtn.disabled = !connected || client.streamActive;
  elements.saveBtn.disabled = !connected || !client.pendingChanges;
  elements.rebootRunBtn.disabled = !connected;
}

function render() {
  computeMappedOutputs();
  renderConnectionState();
  renderTelemetry();
  renderInstrumentDeck();
}

/** @param {() => Promise<void>} fn */
async function guarded(fn) {
  clearError();
  try {
    await fn();
    render();
  } catch (error) {
    showError(errorMessage(error));
    render();
  }
}

elements.connectBtn.addEventListener('click', () => guarded(async () => {
  await client.connect();
  await client.getConfig();
  await client.startStream();
  log('Validation page ready');
}));

elements.reconnectBtn.addEventListener('click', () => guarded(async () => {
  await client.reconnect();
  await client.getConfig();
  if (!client.streamActive) await client.startStream();
}));

elements.disconnectBtn.addEventListener('click', () => guarded(async () => {
  await client.disconnect();
}));

elements.refreshConfigBtn.addEventListener('click', () => guarded(async () => {
  await client.getConfig({ markSaved: false });
  client.pendingChanges = configToStableString(client.currentConfig) !== client.savedConfigString;
  log('Config refreshed');
}));

elements.startStreamBtn.addEventListener('click', () => guarded(async () => {
  await client.startStream();
}));

elements.saveBtn.addEventListener('click', () => guarded(async () => {
  await client.saveProfile();
  log('Profile saved to NVS');
}));

elements.rebootRunBtn.addEventListener('click', () => guarded(async () => {
  await client.rebootToRun();
  log('Device rebooting to RUN mode');
}));

elements.clearLogBtn.addEventListener('click', () => {
  elements.logPanel.textContent = '';
});

render();
