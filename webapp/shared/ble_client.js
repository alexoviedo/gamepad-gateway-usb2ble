export const BLE_UUIDS = Object.freeze({
  service: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1',
  cmd: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2',
  evt: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3',
  stream: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4',
  cfg: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a5',
});

export const DEFAULT_COMMAND_TIMEOUTS = Object.freeze({
  default: 10000,
  get_devices: 15000,
  get_descriptor: 30000,
  get_config: 15000,
});

const DEFAULT_DEVICE_FILTERS = [
  { namePrefix: 'HOTAS_CFG' },
  { namePrefix: 'HOTAS_CONFIG' },
  { namePrefix: 'HOTAS' },
];

function encodeUtf8(value) {
  return new TextEncoder().encode(value);
}

function decodeUtf8(bytes) {
  return new TextDecoder().decode(bytes);
}

function stableStringify(value) {
  return JSON.stringify(value ?? {});
}

export function normalizeElementMeta(meta) {
  if (!meta || typeof meta !== 'object') return null;
  const kindCode = meta.kind ?? meta.k ?? 'other';
  const kind = kindCode === 'a'
    ? 'axis'
    : kindCode === 'b'
      ? 'button'
      : kindCode === 'h'
        ? 'hat'
        : kindCode === 'o'
          ? 'other'
          : kindCode;

  return {
    element_id: Number(meta.element_id ?? meta.i ?? 0),
    kind,
    usage_page: Number(meta.usage_page ?? meta.p ?? 0),
    usage: Number(meta.usage ?? meta.u ?? 0),
    usage_name: meta.usage_name ?? meta.n ?? 'unknown',
  };
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

export class BleClient {
  constructor(options = {}) {
    this.uuids = { ...BLE_UUIDS, ...(options.uuids || {}) };
    this.log = options.log || (() => {});
    this.onError = options.onError || (() => {});
    this.onStateChange = options.onStateChange || (() => {});
    this.onJsonPayload = options.onJsonPayload || null;
    this.onDescriptor = options.onDescriptor || null;
    this.onStreamSample = options.onStreamSample || null;
    this.onDisconnect = options.onDisconnect || null;
    this.onReconnectSuccess = options.onReconnectSuccess || null;
    this.normalizeConfig = options.normalizeConfig || ((value) => value);
    this.commandTimeouts = { ...DEFAULT_COMMAND_TIMEOUTS, ...(options.commandTimeouts || {}) };
    this.autoReconnect = Boolean(options.autoReconnect);
    this.maxReconnectAttempts = Number.isFinite(options.maxReconnectAttempts) ? options.maxReconnectAttempts : 4;
    this.connectSettleMs = Number.isFinite(options.connectSettleMs) ? options.connectSettleMs : 650;
    this.deviceFilters = Array.isArray(options.deviceFilters) && options.deviceFilters.length
      ? options.deviceFilters
      : DEFAULT_DEVICE_FILTERS;

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
    this.streamSubscribed = false;
    this.evtSubscribed = false;
    this.reconnectWanted = false;
    this.reconnectInFlight = false;
    this.reconnectAttempts = 0;
    this.currentConfig = this.normalizeConfig({ version: 2, axes: {} });
    this.savedConfigString = stableStringify(this.currentConfig);
    this.pendingChanges = false;

    this._cmdQueue = Promise.resolve();
    this.onDisconnectedBound = this.onDisconnected.bind(this);
    this.handleEvtBound = (event) => {
      try {
        const value = event.target.value;
        const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        this.handleEvtNotification(bytes);
      } catch (error) {
        this._reportError(`EVT handler failed: ${error.message || error}`);
      }
    };
    this.handleStreamBound = (event) => {
      try {
        const value = event.target.value;
        const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
        this.handleStreamNotification(bytes);
      } catch (error) {
        this._reportError(`STREAM handler failed: ${error.message || error}`);
      }
    };
  }

  _reportError(message) {
    try {
      this.onError(message);
    } catch {
      // swallow secondary UI errors
    }
  }

  _emitStateChange() {
    try {
      this.onStateChange(this);
    } catch (error) {
      this._reportError(`State-change handler failed: ${error.message || error}`);
    }
  }

  async _safeAsyncCallback(callback, ...args) {
    if (!callback) return;
    try {
      await callback(...args);
    } catch (error) {
      this._reportError(`Callback failed: ${error.message || error}`);
    }
  }

  delay(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  resetTransientState() {
    for (const [, pending] of this.pendingJson.entries()) {
      pending.reject(new Error('Request cancelled due to reconnect/disconnect.'));
    }
    this.pendingJson.clear();
    this.chunkAssembler = new ChunkAssembler();
    this.pendingDescriptorBinary = null;
    this.streamActive = false;
    this.streamSubscribed = false;
    this.evtSubscribed = false;
  }

  _enqueueCommand(task) {
    const run = this._cmdQueue.then(task, task);
    this._cmdQueue = run.catch(() => {});
    return run;
  }

  resolveCommandTimeout(commandName, overrideMs) {
    if (Number.isFinite(overrideMs) && overrideMs > 0) return overrideMs;
    if (Number.isFinite(this.commandTimeouts[commandName])) return this.commandTimeouts[commandName];
    return this.commandTimeouts.default || 10000;
  }

  async requestDevice() {
    if (!navigator.bluetooth) {
      throw new Error('Web Bluetooth is not available in this browser. Use Chrome or Edge on desktop.');
    }

    this.log('Opening Bluetooth device picker');
    this.device = await navigator.bluetooth.requestDevice({
      filters: this.deviceFilters,
      optionalServices: [this.uuids.service],
    });
    this.device.removeEventListener('gattserverdisconnected', this.onDisconnectedBound);
    this.device.addEventListener('gattserverdisconnected', this.onDisconnectedBound);
    this.reconnectWanted = true;
    this.reconnectAttempts = 0;
    this.log('Device selected', `${this.device.name || '(unnamed)'} [${this.device.id}]`);
    this._emitStateChange();
    return this.device;
  }

  async ensureEvtSubscription() {
    if (!this.characteristics.evt) throw new Error('EVT characteristic is unavailable.');
    if (this.evtSubscribed) return;
    this.characteristics.evt.removeEventListener('characteristicvaluechanged', this.handleEvtBound);
    this.characteristics.evt.addEventListener('characteristicvaluechanged', this.handleEvtBound);
    await this.characteristics.evt.startNotifications();
    this.evtSubscribed = true;
    this.log('EVT notifications subscribed');
    this._emitStateChange();
  }

  async ensureStreamSubscription() {
    if (!this.characteristics.stream) throw new Error('STREAM characteristic is unavailable.');
    if (this.streamSubscribed) return;
    this.characteristics.stream.removeEventListener('characteristicvaluechanged', this.handleStreamBound);
    this.characteristics.stream.addEventListener('characteristicvaluechanged', this.handleStreamBound);
    await this.characteristics.stream.startNotifications();
    this.streamSubscribed = true;
    this.log('STREAM notifications subscribed');
    this._emitStateChange();
  }

  async connect({ reuseDevice = false } = {}) {
    if (!reuseDevice || !this.device) {
      await this.requestDevice();
    }
    if (!this.device) throw new Error('No Bluetooth device selected.');

    this.resetTransientState();
    this.log('Connecting to GATT server', this.device.name || this.device.id);

    try {
      this.server = await this.device.gatt.connect();
      this.service = await this.server.getPrimaryService(this.uuids.service);
      this.characteristics.cmd = await this.service.getCharacteristic(this.uuids.cmd);
      this.characteristics.evt = await this.service.getCharacteristic(this.uuids.evt);
      this.characteristics.stream = await this.service.getCharacteristic(this.uuids.stream);
      try {
        this.characteristics.cfg = await this.service.getCharacteristic(this.uuids.cfg);
      } catch {
        this.characteristics.cfg = null;
      }

      await this.ensureEvtSubscription();
      this.streamSubscribed = false;
      this.streamActive = false;
      await this.delay(this.connectSettleMs);
      this.log('Connected and EVT notifications enabled');
      this._emitStateChange();
      return true;
    } catch (error) {
      this._reportError(`Connect failed: ${error.message || error}`);
      await this.disconnect({ intentional: false });
      throw error;
    }
  }

  async reconnect() {
    if (!this.device) throw new Error('No previously selected Bluetooth device to reconnect to.');
    return this.connect({ reuseDevice: true });
  }

  async disconnect({ intentional = true } = {}) {
    this.streamActive = false;
    if (intentional) this.reconnectWanted = false;
    this.resetTransientState();
    try {
      if (this.characteristics.evt) {
        this.characteristics.evt.removeEventListener('characteristicvaluechanged', this.handleEvtBound);
      }
      if (this.characteristics.stream) {
        this.characteristics.stream.removeEventListener('characteristicvaluechanged', this.handleStreamBound);
      }
      if (this.device?.gatt?.connected) {
        this.device.gatt.disconnect();
      }
    } catch (error) {
      this.log('Disconnect warning', error.message || String(error));
    }
    this.server = null;
    this.service = null;
    this.characteristics = {};
    this._emitStateChange();
  }

  async onDisconnected() {
    this.log('Device disconnected unexpectedly');
    this.server = null;
    this.service = null;
    this.characteristics = {};
    this.resetTransientState();
    this._emitStateChange();
    await this._safeAsyncCallback(this.onDisconnect, this);

    if (!this.autoReconnect || !this.reconnectWanted || this.reconnectInFlight) return;

    this.reconnectInFlight = true;
    while (this.reconnectWanted && this.reconnectAttempts < this.maxReconnectAttempts) {
      this.reconnectAttempts += 1;
      const delayMs = this.reconnectAttempts * 1000;
      this.log(`Reconnect attempt ${this.reconnectAttempts}/${this.maxReconnectAttempts} scheduled`, `${delayMs}ms`);
      await this.delay(delayMs);
      try {
        await this.reconnect();
        await this._safeAsyncCallback(this.onReconnectSuccess, this);
        this.log('Reconnect successful');
        this.reconnectAttempts = 0;
        this.reconnectInFlight = false;
        this._emitStateChange();
        return;
      } catch (error) {
        this.log('Reconnect failed', error.message || String(error));
      }
    }

    this.reconnectInFlight = false;
    this._reportError('Connection dropped and automatic reconnect did not succeed. Press Reconnect to try again.');
  }

  async writeCommandText(json) {
    const cmd = this.characteristics.cmd;
    if (!cmd) throw new Error('CMD characteristic is unavailable.');

    const data = encodeUtf8(json);
    const props = cmd.properties || {};

    if (props.write && typeof cmd.writeValue === 'function') {
      await cmd.writeValue(data);
      return 'with-response';
    }
    if (typeof cmd.writeValue === 'function') {
      await cmd.writeValue(data);
      return 'legacy-write';
    }
    if (props.writeWithoutResponse && typeof cmd.writeValueWithoutResponse === 'function') {
      await cmd.writeValueWithoutResponse(data);
      return 'without-response';
    }
    throw new Error('CMD characteristic is not writable.');
  }

  async sendCommand(command, options = {}) {
    return this._enqueueCommand(async () => {
      if (!this.characteristics.cmd) throw new Error('Not connected to the Config Service.');
      await this.ensureEvtSubscription();

      if (this.pendingJson.size === 0 && !this.pendingDescriptorBinary) {
        this.chunkAssembler = new ChunkAssembler();
      }

      const rid = ++this.requestId;
      const payload = { rid, ...command };
      const json = JSON.stringify(payload);
      const timeoutMs = this.resolveCommandTimeout(payload.cmd, options.timeoutMs);

      const responsePromise = new Promise((resolve, reject) => {
        const timeoutId = setTimeout(() => {
          if (this.pendingJson.has(rid)) {
            this.pendingJson.delete(rid);
            reject(new Error(`Timed out waiting for response to ${payload.cmd}`));
          }
        }, timeoutMs);

        this.pendingJson.set(rid, {
          resolve: (value) => {
            clearTimeout(timeoutId);
            resolve(value);
          },
          reject: (error) => {
            clearTimeout(timeoutId);
            reject(error);
          },
        });
      });

      try {
        const writeMode = await this.writeCommandText(json);
        this.log('CMD →', `${json} [${writeMode}]`);
      } catch (error) {
        const pending = this.pendingJson.get(rid);
        if (pending) {
          this.pendingJson.delete(rid);
          pending.reject(error);
        }
        throw error;
      }

      return responsePromise;
    });
  }

  async getDevices() {
    const response = await this.sendCommand({ cmd: 'get_devices' });
    this.devices = Array.isArray(response.devices) ? response.devices : [];

    const liveIds = new Set(this.devices.map((device) => device.device_id));
    for (const key of this.descriptorCache.keys()) {
      if (!liveIds.has(key)) this.descriptorCache.delete(key);
    }
    for (const key of this.elementMetaByDevice.keys()) {
      if (!liveIds.has(key)) this.elementMetaByDevice.delete(key);
    }

    if (!this.devices.length) {
      this.selectedDeviceId = null;
      this._emitStateChange();
      return this.devices;
    }

    const selectedStillExists = this.devices.some((device) => device.device_id === this.selectedDeviceId);
    if (!selectedStillExists) {
      this.selectedDeviceId = this.devices[0].device_id;
    }
    this._emitStateChange();
    return this.devices;
  }

  async getDescriptor(deviceId) {
    this.pendingDescriptorBinary = { deviceId };
    return this.sendCommand({ cmd: 'get_descriptor', device_id: deviceId });
  }

  async getElements(deviceId) {
    const response = await this.sendCommand({ cmd: 'get_elements', device_id: deviceId });
    if (typeof response?.device_id === 'number' && Array.isArray(response?.elements)) {
      const byId = new Map();
      for (const rawElement of response.elements) {
        const element = normalizeElementMeta(rawElement);
        if (element) {
          byId.set(String(element.element_id), element);
        }
      }
      this.elementMetaByDevice.set(response.device_id, byId);
      this._emitStateChange();
    }
    return response;
  }

  async loadAllElementMetadata() {
    for (const device of this.devices) {
      if (this.elementMetaByDevice.has(device.device_id)) continue;
      await this.getElements(device.device_id);
      await this.delay(50);
    }
  }

  async ensureConfigCharacteristic() {
    if (!this.characteristics.cfg) throw new Error('CFG characteristic is unavailable.');
    return this.characteristics.cfg;
  }

  async readConfigCharacteristic() {
    const cfg = await this.ensureConfigCharacteristic();
    const value = await cfg.readValue();
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    const jsonText = decodeUtf8(bytes);
    this.log('CFG read ←', `${bytes.byteLength} bytes`);
    return JSON.parse(jsonText);
  }

  async getConfig({ markSaved = true, timeoutMs } = {}) {
    let cfgError = null;
    try {
      const cfg = await this.readConfigCharacteristic();
      this.currentConfig = this.normalizeConfig(cfg);
      if (markSaved) {
        this.savedConfigString = stableStringify(this.currentConfig);
        this.pendingChanges = false;
      }
      this._emitStateChange();
      return this.currentConfig;
    } catch (error) {
      cfgError = error;
      this.log('CFG-first read unavailable', error.message || String(error));
    }

    const response = await this.sendCommand({ cmd: 'get_config' }, { timeoutMs });
    if (response?.config && typeof response.config === 'object') {
      this.currentConfig = this.normalizeConfig(response.config);
    } else if (typeof response?.config_json === 'string') {
      this.currentConfig = this.normalizeConfig(JSON.parse(response.config_json));
    } else if (response?.transport === 'cfg_read') {
      this.currentConfig = this.normalizeConfig(await this.readConfigCharacteristic());
    } else if (cfgError) {
      throw new Error(`CFG read failed: ${cfgError.message || cfgError}; inline get_config response did not include config data.`);
    } else {
      throw new Error('get_config response did not include config or config_json.');
    }

    if (markSaved) {
      this.savedConfigString = stableStringify(this.currentConfig);
      this.pendingChanges = false;
    }
    this._emitStateChange();
    return this.currentConfig;
  }

  async startStream() {
    await this.ensureEvtSubscription();
    await this.ensureStreamSubscription();
    const response = await this.sendCommand({ cmd: 'start_stream' });
    this.streamActive = true;
    this._emitStateChange();
    return response;
  }

  async stopStream() {
    const response = await this.sendCommand({ cmd: 'stop_stream' });
    this.streamActive = false;
    this._emitStateChange();
    return response;
  }

  async saveProfile() {
    const response = await this.sendCommand({ cmd: 'save_profile' });
    if (response.ok && this.currentConfig) {
      this.savedConfigString = stableStringify(this.currentConfig);
      this.pendingChanges = false;
      this._emitStateChange();
    }
    return response;
  }

  async applyAxisConfig(axisKey, axisConfig) {
    const normalized = this.normalizeConfig({ version: 2, axes: { [axisKey]: axisConfig } }).axes[axisKey];
    const patch = {
      version: 2,
      axes: {
        [axisKey]: normalized,
      },
    };

    const response = await this.sendCommand({ cmd: 'set_config', config: patch });
    if (response.config && typeof response.config === 'object') {
      this.currentConfig = this.normalizeConfig(response.config);
    } else {
      this.currentConfig = this.normalizeConfig(this.currentConfig || { version: 2, axes: {} });
      this.currentConfig.axes[axisKey] = normalized;
    }

    this.pendingChanges = stableStringify(this.currentConfig) !== this.savedConfigString;
    this._emitStateChange();
    return response;
  }

  async applyAxisPatch(axisKey, candidate) {
    const existing = this.normalizeConfig(this.currentConfig || { version: 2, axes: {} }).axes?.[axisKey] || {};
    const next = {
      ...existing,
      configured: true,
      device_id: candidate.deviceId,
      element_id: candidate.elementId,
    };
    return this.applyAxisConfig(axisKey, next);
  }

  async rebootToRun() {
    return this.sendCommand({ cmd: 'reboot_to_run' });
  }

  async rebootToConfig() {
    return this.sendCommand({ cmd: 'reboot_to_config' });
  }

  getElementMeta(deviceId, elementId) {
    return this.elementMetaByDevice.get(deviceId)?.get(String(elementId)) || null;
  }

  handleEvtNotification(bytes) {
    if (bytes.byteLength >= 8) {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const version = view.getUint8(0);
      const type = view.getUint8(1);
      const msgId = view.getUint16(2, true);
      const offset = view.getUint16(4, true);
      const total = view.getUint16(6, true);
      this.log('EVT frame', `${bytes.byteLength} bytes · v=${version} type=${type} msg=${msgId} off=${offset} total=${total}`);
    }

    let complete;
    try {
      complete = this.chunkAssembler.push(bytes);
    } catch (error) {
      this.chunkAssembler = new ChunkAssembler();
      throw error;
    }
    if (!complete) return;

    if (complete.type === 1) {
      const text = decodeUtf8(complete.bytes);
      this.log('EVT JSON ←', text);
      const payload = JSON.parse(text);

      if (Array.isArray(payload?.devices)) this.devices = payload.devices;
      if (typeof payload?.device_id === 'number' && Array.isArray(payload?.elements)) {
        const byId = new Map();
        for (const rawElement of payload.elements) {
          const element = normalizeElementMeta(rawElement);
          if (element) byId.set(String(element.element_id), element);
        }
        this.elementMetaByDevice.set(payload.device_id, byId);
      }
      if (payload?.config && typeof payload.config === 'object') {
        this.currentConfig = this.normalizeConfig(payload.config);
        this.pendingChanges = stableStringify(this.currentConfig) !== this.savedConfigString;
      }

      const pending = typeof payload?.rid === 'number' ? this.pendingJson.get(payload.rid) : null;
      if (pending) {
        this.pendingJson.delete(payload.rid);
        pending.resolve(payload);
      }

      this._emitStateChange();
      this._safeAsyncCallback(this.onJsonPayload, payload, this);
      return;
    }

    if (complete.type === 2) {
      if (this.pendingDescriptorBinary?.deviceId != null) {
        this.descriptorCache.set(this.pendingDescriptorBinary.deviceId, complete.bytes);
        this.log('Descriptor received', `${complete.bytes.length} bytes for device ${this.pendingDescriptorBinary.deviceId}`);
      } else {
        this.log('Descriptor received', `${complete.bytes.length} bytes`);
      }
      this.pendingDescriptorBinary = null;
      this._emitStateChange();
      this._safeAsyncCallback(this.onDescriptor, complete.bytes, this);
      return;
    }

    this.log('Unhandled EVT frame', `type=${complete.type} len=${complete.bytes.length}`);
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

    this._emitStateChange();
    this._safeAsyncCallback(this.onStreamSample, sample, this);
  }
}
