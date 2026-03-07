const UUIDS = {
  service: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a1',
  cmd: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a2',
  evt: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a3',
  stream: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a4',
  cfg: '6b1a2d90-9c1c-4a3f-b1e0-4fd8f5b2c1a5',
};

const elements = {
  connectBtn: document.getElementById('connectBtn'),
  reconnectBtn: document.getElementById('reconnectBtn'),
  disconnectBtn: document.getElementById('disconnectBtn'),
  refreshDevicesBtn: document.getElementById('refreshDevicesBtn'),
  readConfigBtn: document.getElementById('readConfigBtn'),
  startStreamBtn: document.getElementById('startStreamBtn'),
  stopStreamBtn: document.getElementById('stopStreamBtn'),
  loadDescriptorBtn: document.getElementById('loadDescriptorBtn'),
  copyDescriptorBtn: document.getElementById('copyDescriptorBtn'),
  downloadDescriptorBtn: document.getElementById('downloadDescriptorBtn'),
  clearLogBtn: document.getElementById('clearLogBtn'),
  connBadge: document.getElementById('connBadge'),
  streamBadge: document.getElementById('streamBadge'),
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
};

elements.serviceUuidText.textContent = UUIDS.service;

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
      return {
        version: state.version,
        type: state.type,
        msgId,
        bytes: state.buffer,
      };
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
    this.pendingDescriptor = null;
    this.devices = [];
    this.selectedDeviceId = null;
    this.descriptorCache = new Map();
    this.sampleHistory = [];
    this.sampleTimestamps = [];
    this.latestSample = null;
    this.streamActive = false;
    this.reconnectWanted = false;
    this.reconnectInFlight = false;
    this.reconnectAttempts = 0;
    this.maxReconnectAttempts = 4;
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
    this.characteristics.evt.addEventListener('characteristicvaluechanged', (event) => {
      const value = event.target.value;
      const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
      this.handleEvtNotification(bytes);
    });

    await this.characteristics.stream.startNotifications();
    this.characteristics.stream.addEventListener('characteristicvaluechanged', (event) => {
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
      if (this.device?.gatt?.connected) {
        this.device.gatt.disconnect();
      }
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
      this.pendingJson.set(rid, { resolve, reject, payload, startedAt: Date.now() });
      setTimeout(() => {
        if (this.pendingJson.has(rid)) {
          this.pendingJson.delete(rid);
          reject(new Error(`Timed out waiting for response to ${payload.cmd}`));
        }
      }, 7000);
    });

    await this.characteristics.cmd.writeValue(encodeUtf8(json));
    log('CMD →', json);
    return responsePromise;
  }

  async getDevices() {
    const response = await this.sendCommand({ cmd: 'get_devices' });
    this.devices = Array.isArray(response.devices) ? response.devices : [];
    if (this.selectedDeviceId == null && this.devices.length) {
      this.selectedDeviceId = this.devices[0].device_id;
    }
    return this.devices;
  }

  async getDescriptor(deviceId) {
    this.pendingDescriptor = { deviceId, rid: this.requestId + 1 };
    const response = await this.sendCommand({ cmd: 'get_descriptor', device_id: deviceId });
    return response;
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

  async readCfg() {
    if (!this.characteristics.cfg) throw new Error('CFG characteristic not discovered.');
    const value = await this.characteristics.cfg.readValue();
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    return decodeUtf8(bytes);
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

      if (Array.isArray(payload?.devices)) {
        this.devices = payload.devices;
      }

      render();
      return;
    }

    if (complete.type === 2) {
      if (this.pendingDescriptor?.deviceId != null) {
        this.descriptorCache.set(this.pendingDescriptor.deviceId, complete.bytes);
        log('Descriptor received', `${complete.bytes.length} bytes for device ${this.pendingDescriptor.deviceId}`);
      } else {
        log('Descriptor received', `${complete.bytes.length} bytes`);
      }
      this.pendingDescriptor = null;
      render();
      return;
    }

    log('Unhandled EVT frame', `type=${complete.type} len=${complete.bytes.length}`);
  }

  handleStreamNotification(bytes) {
    if (bytes.byteLength < 16) {
      log('STREAM warning', 'sample shorter than expected');
      return;
    }

    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const sample = {
      version: view.getUint8(0),
      flags: view.getUint8(1),
      deviceId: view.getUint32(2, true),
      elementId: view.getUint32(6, true),
      raw: view.getInt32(10, true),
      normQ15: view.getInt16(14, true),
      receivedAt: new Date(),
    };

    this.latestSample = sample;
    this.sampleHistory.unshift(sample);
    this.sampleHistory = this.sampleHistory.slice(0, 40);

    const now = Date.now();
    this.sampleTimestamps.push(now);
    while (this.sampleTimestamps.length && now - this.sampleTimestamps[0] > 1000) {
      this.sampleTimestamps.shift();
    }

    render();
  }
}

const client = new HotasConfigClient();

function selectedDevice() {
  return client.devices.find((device) => device.device_id === client.selectedDeviceId) || null;
}

function renderConnectionState() {
  const connected = Boolean(client.device?.gatt?.connected && client.service);
  elements.connBadge.textContent = connected ? 'Connected' : 'Disconnected';
  elements.connBadge.className = `pill ${connected ? 'success' : 'warn'}`;
  elements.streamBadge.textContent = client.streamActive ? 'Stream active' : 'Stream idle';
  elements.streamBadge.className = `pill ${client.streamActive ? 'success' : 'muted'}`;
  elements.deviceName.textContent = client.device?.name || client.device?.id || '—';
  elements.gattState.textContent = connected ? 'Connected' : 'Not connected';
  elements.evtState.textContent = connected ? 'Subscribed' : 'Off';
  elements.streamState.textContent = connected ? 'Subscribed' : 'Off';

  elements.connectBtn.disabled = connected;
  elements.reconnectBtn.disabled = !client.device || connected;
  elements.disconnectBtn.disabled = !connected;
  elements.refreshDevicesBtn.disabled = !connected;
  elements.readConfigBtn.disabled = !connected;
  elements.startStreamBtn.disabled = !connected || client.streamActive;
  elements.stopStreamBtn.disabled = !connected || !client.streamActive;
}

function renderDevices() {
  const devices = client.devices;
  elements.deviceCount.textContent = `${devices.length} device${devices.length === 1 ? '' : 's'}`;

  if (!devices.length) {
    elements.deviceList.className = 'device-list empty-state';
    elements.deviceList.textContent = 'No devices returned yet. Click Refresh Devices after connecting.';
    return;
  }

  elements.deviceList.className = 'device-list';
  elements.deviceList.innerHTML = '';
  for (const device of devices) {
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

  elements.loadDescriptorBtn.disabled = !device || !client.service;
  elements.copyDescriptorBtn.disabled = !bytes;
  elements.downloadDescriptorBtn.disabled = !bytes;

  if (!device) {
    elements.descriptorMeta.textContent = 'Select a device card to inspect its descriptor.';
    elements.descriptorHex.className = 'code-panel empty-state';
    elements.descriptorHex.textContent = 'No descriptor loaded yet.';
    return;
  }

  elements.descriptorMeta.innerHTML = `
    <strong>Device ${device.device_id}</strong> · role <strong>${device.role}</strong> ·
    <span>${device.num_elements} elements</span> ·
    <span>descriptor length ${device.report_desc_len} bytes</span>
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
    ['—', '—', '—', '—'].forEach((value, index) => {
      latestFields[index].textContent = value;
    });
  } else {
    latestFields[0].textContent = `device ${latest.deviceId} · element ${latest.elementId}`;
    latestFields[1].textContent = String(latest.raw);
    latestFields[2].textContent = `${latest.normQ15} (${formatQ15(latest.normQ15)})`;
    latestFields[3].textContent = latest.receivedAt.toLocaleTimeString();
  }

  if (!history.length) {
    elements.telemetryList.className = 'telemetry-list empty-state';
    elements.telemetryList.textContent = client.streamActive
      ? 'Waiting for stream samples…'
      : 'Start stream to see live samples.';
    return;
  }

  elements.telemetryList.className = 'telemetry-list';
  elements.telemetryList.innerHTML = '';
  for (const sample of history.slice(0, 24)) {
    const row = document.createElement('div');
    row.className = 'telemetry-row';
    row.innerHTML = `
      <div>
        <div>device ${sample.deviceId}</div>
        <div class="muted">element ${sample.elementId}</div>
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

function render() {
  renderConnectionState();
  renderDevices();
  renderDescriptor();
  renderTelemetry();
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
}, 'Unable to connect'));

elements.reconnectBtn.addEventListener('click', () => guarded(async () => {
  await client.reconnect();
  await client.getDevices();
}, 'Reconnect failed'));

elements.disconnectBtn.addEventListener('click', () => guarded(async () => {
  await client.disconnect();
}, 'Disconnect failed'));

elements.refreshDevicesBtn.addEventListener('click', () => guarded(async () => {
  await client.getDevices();
}, 'Failed to refresh devices'));

elements.readConfigBtn.addEventListener('click', () => guarded(async () => {
  const text = await client.readCfg();
  log('CFG ←', text || '(empty)');
}, 'Failed to read CFG'));

elements.startStreamBtn.addEventListener('click', () => guarded(async () => {
  await client.startStream();
}, 'Failed to start stream'));

elements.stopStreamBtn.addEventListener('click', () => guarded(async () => {
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

elements.clearLogBtn.addEventListener('click', () => {
  elements.logPanel.textContent = '';
});

render();
log('Web app ready');
