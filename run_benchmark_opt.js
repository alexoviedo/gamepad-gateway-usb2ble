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
console.log(`Optimized: ${ms.toFixed(2)} ms for ${iterations} iterations`);
console.log(`Average: ${(ms / iterations).toFixed(4)} ms per call`);
