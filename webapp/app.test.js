// @ts-nocheck
import { HotasConfigClient, showError } from './app.js';

// Minimal mock for DataView
global.DataView = global.DataView || class {
  constructor(buffer) { this.buffer = buffer; }
  getUint8(offset) { return new Uint8Array(this.buffer)[offset]; }
  getUint16(offset, littleEndian) {
    const view = new Uint8Array(this.buffer);
    return littleEndian ? view[offset] | (view[offset+1] << 8) : (view[offset] << 8) | view[offset+1];
  }
  getUint32(offset, littleEndian) {
    const view = new Uint8Array(this.buffer);
    if (littleEndian) {
        return view[offset] | (view[offset+1] << 8) | (view[offset+2] << 16) | (view[offset+3] << 24);
    }
    return (view[offset] << 24) | (view[offset+1] << 16) | (view[offset+2] << 8) | view[offset+3];
  }
};

// Mock TextDecoder/Encoder
global.TextDecoder = class {
  decode(bytes) { return String.fromCharCode(...bytes); }
};
global.TextEncoder = class {
  encode(str) { return new Uint8Array([...str].map(c => c.charCodeAt(0))); }
};

// Test Runner state
let passed = 0;
let failed = 0;

function assert(condition, message) {
  if (!condition) {
    console.error('FAIL:', message);
    failed++;
    return false;
  }
  passed++;
  return true;
}

async function runTests() {
  console.log('Starting tests...');

  // Mock for HotasConfigClient dependencies
  const client = new HotasConfigClient();
  client.characteristics.cmd = {
    writeValue: async () => {}
  };

  // --- Test Case 1: getConfig handles valid JSON ---
  try {
    client.sendCommand = async () => ({
      config_json: JSON.stringify({ version: 2, axes: { z: { configured: true, device_id: 1, element_id: 2 } } })
    });
    await client.getConfig();
    assert(client.currentConfig.version === 2, 'getConfig should parse valid JSON');
  } catch (e) {
    assert(false, 'Test 1 failed: ' + e.message);
  }

  // --- Test Case 2: getConfig handles malformed JSON (The New Catch Block) ---
  try {
    client.sendCommand = async () => ({
      config_json: '{"invalid": json'
    });

    await client.getConfig();
    assert(true, 'getConfig should reach end without crashing on malformed JSON');
  } catch (e) {
    assert(false, 'Test 2 crashed on malformed JSON: ' + e.message);
  }

  // --- Test Case 3: handleEvtNotification handles valid JSON ---
  try {
    const validJson = JSON.stringify({ rid: 123, devices: [{ device_id: 1, role: 'stick' }] });
    const bytes = new TextEncoder().encode(validJson);

    client.chunkAssembler.push = () => ({
      type: 1,
      bytes: bytes
    });

    client.pendingJson.set(123, {
      resolve: (payload) => {
        assert(payload.rid === 123, 'handleEvtNotification should resolve pending JSON');
      },
      reject: () => {}
    });

    client.handleEvtNotification(new Uint8Array(10));
    assert(client.devices[0].device_id === 1, 'handleEvtNotification should update devices list');
  } catch (e) {
    assert(false, 'Test 3 failed: ' + e.message);
  }

  // --- Test Case 4: handleEvtNotification handles malformed JSON (The Existing Catch Block) ---
  try {
    const malformedJson = '{"invalid": json';
    const bytes = new TextEncoder().encode(malformedJson);

    client.chunkAssembler.push = () => ({
      type: 1,
      bytes: bytes
    });

    client.handleEvtNotification(new Uint8Array(10));
    assert(true, 'handleEvtNotification should reach end without crashing on malformed JSON');
  } catch (e) {
    assert(false, 'Test 4 failed: ' + e.message);
  }

  console.log(`\nTests finished. Passed: ${passed}, Failed: ${failed}`);
  if (failed > 0) process.exit(1);
}

runTests().catch(e => {
  console.error('Test runner crashed:', e);
  process.exit(1);
});
