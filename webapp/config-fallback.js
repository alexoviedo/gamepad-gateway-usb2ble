import { HotasConfigClient, log } from './app.js';

function stableStringify(value) {
  return JSON.stringify(value ?? {});
}

function ensureDefaultConfig(client, markSaved = false) {
  const next = (client.currentConfig && typeof client.currentConfig === 'object')
    ? client.currentConfig
    : { version: 2, axes: {} };

  if (typeof next.axes !== 'object' || next.axes === null || Array.isArray(next.axes)) {
    next.axes = {};
  }
  next.version = 2;
  client.currentConfig = next;

  if (markSaved) {
    client.savedConfigString = stableStringify(client.currentConfig);
    client.pendingChanges = false;
  }
  return client.currentConfig;
}

if (!HotasConfigClient.prototype.readConfigCharacteristic) {
  HotasConfigClient.prototype.readConfigCharacteristic = async function readConfigCharacteristic() {
    const cfg = this.characteristics.cfg;
    if (!cfg) {
      throw new Error('CFG characteristic is unavailable.');
    }

    const value = await cfg.readValue();
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    const jsonText = new TextDecoder().decode(bytes);
    log('CFG read ←', `${bytes.byteLength} bytes`);

    try {
      return JSON.parse(jsonText);
    } catch (error) {
      const previewStart = Math.max(0, 255 - 80);
      const previewEnd = Math.min(jsonText.length, 255 + 120);
      const preview = jsonText.slice(previewStart, previewEnd);
      log('CFG read parse preview', JSON.stringify(preview));
      throw new Error(`CFG read returned invalid JSON: ${error.message}`);
    }
  };
}

HotasConfigClient.prototype.getConfig = async function getConfigWithCfgFallback({ markSaved = true } = {}) {
  let cmdError = null;

  try {
    const response = await this.sendCommand({ cmd: 'get_config' });

    if (response?.config && typeof response.config === 'object') {
      this.currentConfig = response.config;
    } else if (typeof response?.config_json === 'string') {
      this.currentConfig = JSON.parse(response.config_json);
    } else if (response?.ok && response?.transport === 'cfg_read') {
      log('get_config metadata', `transport=${response.transport} config_len=${response.config_len ?? 'unknown'}; reading CFG characteristic`);
      this.currentConfig = await this.readConfigCharacteristic();
    } else {
      throw new Error('get_config response did not include config or config_json.');
    }

    ensureDefaultConfig(this, markSaved);
    return this.currentConfig;
  } catch (error) {
    cmdError = error;
    log('get_config CMD failed', error.message || String(error));
  }

  try {
    this.currentConfig = await this.readConfigCharacteristic();
    ensureDefaultConfig(this, markSaved);
    return this.currentConfig;
  } catch (cfgError) {
    throw new Error(
      `get_config command failed: ${cmdError?.message || cmdError}; CFG fallback failed: ${cfgError?.message || cfgError}`
    );
  }
};
