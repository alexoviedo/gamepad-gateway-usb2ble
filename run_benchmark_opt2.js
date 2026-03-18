import fs from 'fs';
import { performance } from 'perf_hooks';

// Setup mock browser environment so we can eval validation.js
const mockWindow = {
  addEventListener: () => {},
  devicePixelRatio: 1,
  document: {
    getElementById: () => ({
      classList: { add: () => {}, remove: () => {} },
      appendChild: () => {},
      addEventListener: () => {},
      style: {},
      getBoundingClientRect: () => ({ width: 800, height: 600 }),
    }),
    createElement: () => ({
      classList: { add: () => {}, remove: () => {} },
      appendChild: () => {},
      addEventListener: () => {},
      style: {},
    }),
  },
  navigator: {},
  ResizeObserver: class { observe() {} },
  requestAnimationFrame: () => {},
  Date: Date,
};

global.document = mockWindow.document;
global.window = mockWindow;
global.ResizeObserver = mockWindow.ResizeObserver;
global.requestAnimationFrame = mockWindow.requestAnimationFrame;

// Load the script code
let code = fs.readFileSync('webapp/validation.js', 'utf8');

// Strip out the THREE.js import which will fail in Node.js easily
code = code.replace(/import \* as THREE.*?;/, `
const THREE = {
  Vector3: class { set() {} },
  WebGLRenderer: class {
    constructor() {
      this.domElement = { style: {} };
    }
    setPixelRatio() {}
    setSize() {}
    render() {}
  },
  Scene: class { add() {} },
  PerspectiveCamera: class {
    constructor() { this.position = { set: () => {} }; }
    lookAt() {}
    updateProjectionMatrix() {}
  },
  Clock: class { getDelta() { return 0.016; } },
  HemisphereLight: class {},
  DirectionalLight: class { constructor() { this.position = { set: () => {} }; } },
  Mesh: class { constructor() { this.position = { set: () => {} }; this.rotation = {}; } },
  Group: class { constructor() { this.position = { set: () => {} }; this.rotation = {}; } add() {} },
  CircleGeometry: class {},
  BoxGeometry: class {},
  CylinderGeometry: class {},
  TorusGeometry: class {},
  MeshBasicMaterial: class {},
  MeshStandardMaterial: class {},
  MathUtils: { lerp: (a,b,t) => a + (b-a)*t },
  SRGBColorSpace: 'srgb',
};
`);

// Apply optimization
code = code.replace(
  /function computeMappedOutputs\(\) \{[\s\S]*?client\.currentConfig = config;[\s\S]*?const next = \{\};[\s\S]*?for \(const output of OUTPUTS\) \{[\s\S]*?const mapping = normalizeAxisConfig\(config\.axes\[output\.key\]\);/,
  `
let cachedConfigInstance = null;
let cachedConfigShape = null;
function computeMappedOutputs() {
  if (client.currentConfig !== cachedConfigInstance) {
    cachedConfigShape = ensureConfigShape(client.currentConfig);
    client.currentConfig = cachedConfigShape;
    cachedConfigInstance = client.currentConfig;
  }
  const config = cachedConfigShape;
  const next = {};

  for (const output of OUTPUTS) {
    const mapping = config.axes[output.key] || normalizeAxisConfig(null);`
);

// We need to make sure that ensureConfigShape already normalized everything.
// ensureConfigShape looks like:
// function ensureConfigShape(config) {
//   const shaped = deepClone(config || { version: 2, axes: {} });
//   shaped.version = 2;
//   shaped.axes = shaped.axes || {};
//   for (const output of OUTPUTS) {
//     if (shaped.axes[output.key]) {
//       shaped.axes[output.key] = normalizeAxisConfig(shaped.axes[output.key]);
//     }
//   }
//   return shaped;
// }
// Wait, ensureConfigShape only normalizes config.axes[output.key] IF IT EXISTS.
// What if it doesn't? The original computeMappedOutputs did:
// normalizeAxisConfig(config.axes[output.key])
// This handles undefined because normalizeAxisConfig(undefined) returns merged with default.

// To make ensureConfigShape ALWAYS normalize it, so we don't have to fallback in computeMappedOutputs:
code = code.replace(
  /function ensureConfigShape\(config\) \{[\s\S]*?return shaped;\n\}/,
  `function ensureConfigShape(config) {
  const shaped = deepClone(config || { version: 2, axes: {} });
  shaped.version = 2;
  shaped.axes = shaped.axes || {};
  for (const output of OUTPUTS) {
    shaped.axes[output.key] = normalizeAxisConfig(shaped.axes[output.key]);
  }
  return shaped;
}`
);


try {
  eval(`
    ${code}
    global.computeMappedOutputs = computeMappedOutputs;
    global.client = client;
    global.scene = scene;
    global.mappedOutputs = mappedOutputs;
    global.ensureConfigShape = ensureConfigShape;
    global.normalizeAxisConfig = normalizeAxisConfig;
  `);
} catch (e) {
  console.error("Error evaluating code:", e);
}

// Ensure the config is set up
client.currentConfig = ensureConfigShape();

// Warmup
for (let i = 0; i < 1000; i++) {
  computeMappedOutputs();
}

// Benchmark
const iterations = 50000;
const start = performance.now();

for (let i = 0; i < iterations; i++) {
  computeMappedOutputs();
}

const end = performance.now();
const ms = end - start;
console.log(`Optimized 2: ${ms.toFixed(2)} ms for ${iterations} iterations`);
console.log(`Average: ${(ms / iterations).toFixed(4)} ms per call`);
