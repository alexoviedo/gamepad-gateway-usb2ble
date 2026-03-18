import { HotasConfigClient, showError } from './app.js';

// Minimal mock for DataView
global.DataView = global.DataView || class {
  constructor(buffer, byteOffset=0, byteLength=undefined) {
      this.buffer = buffer;
      this.byteOffset = byteOffset;
      this.byteLength = byteLength !== undefined ? byteLength : buffer.byteLength;
  }
  getUint8(offset) { return new Uint8Array(this.buffer, this.byteOffset, this.byteLength)[offset]; }
  getUint16(offset, littleEndian) {
    const view = new Uint8Array(this.buffer, this.byteOffset, this.byteLength);
    return littleEndian ? view[offset] | (view[offset+1] << 8) : (view[offset] << 8) | view[offset+1];
  }
  getUint32(offset, littleEndian) {
    const view = new Uint8Array(this.buffer, this.byteOffset, this.byteLength);
    if (littleEndian) {
        return view[offset] | (view[offset+1] << 8) | (view[offset+2] << 16) | (view[offset+3] << 24);
    }
    return (view[offset] << 24) | (view[offset+1] << 16) | (view[offset+2] << 8) | view[offset+3];
  }
  setUint8(offset, val) { new Uint8Array(this.buffer, this.byteOffset, this.byteLength)[offset] = val; }
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

  // Test 1: getConfig returns valid parsed binary structure
  try {
    client.sendCommand = async () => ({
      config: { version: 2, axes: { z: { configured: true, device_id: 1, element_id: 2 } } }
    });
    await client.getConfig();
    assert(client.currentConfig.version === 2, 'getConfig should parse valid object');
  } catch (e) {
    assert(false, 'Test 1 failed: ' + e.message);
  }

  // Test 2: handleEvtNotification parses binary device stream
  try {
    const payload = new Uint8Array([
      1, 0, 0, 0, // device_id
      1, // active
      1, // role
      65, 66, 67, 0 // name "ABC"
    ]);

    client.chunkAssembler.push = () => ({
      type: 1, // GET_DEVICES
      bytes: payload
    });

    client.pendingJson.set(123, {
      resolve: (val) => {
        assert(val.devices && val.devices[0].device_id === 1, 'handleEvtNotification should resolve devices');
      },
      reject: () => {}
    });

    client.handleEvtNotification(new Uint8Array([2, 1, 123, 0, 0, 0, 10, 0, ...payload]));
    assert(client.devices[0].name === "ABC", 'handleEvtNotification should update devices list');
  } catch (e) {
    assert(false, 'Test 2 failed: ' + e.message);
  }

  console.log(`\nTests finished. Passed: ${passed}, Failed: ${failed}`);
  if (failed > 0) process.exit(1);
}

runTests().catch(e => {
  console.error('Test runner crashed:', e);
  process.exit(1);
});
