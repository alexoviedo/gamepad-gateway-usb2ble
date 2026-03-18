// @ts-nocheck
import { performance } from 'perf_hooks';

// Simulate HotasConfigClient structures
class HotasConfigClient {
  constructor() {
    this.devices = [];
    this.descriptorCache = new Map();
    this.elementMetaByDevice = new Map();
  }

  // Current implementation
  currentGetDevices(liveIds) {
    for (const key of [...this.descriptorCache.keys()]) {
      if (!liveIds.has(key)) this.descriptorCache.delete(key);
    }
    for (const key of [...this.elementMetaByDevice.keys()]) {
      if (!liveIds.has(key)) this.elementMetaByDevice.delete(key);
    }
  }

  // Optimized implementation
  optimizedGetDevices(liveIds) {
    for (const key of this.descriptorCache.keys()) {
      if (!liveIds.has(key)) this.descriptorCache.delete(key);
    }
    for (const key of this.elementMetaByDevice.keys()) {
      if (!liveIds.has(key)) this.elementMetaByDevice.delete(key);
    }
  }
}

function runBenchmark(numItems, deletePercent) {
  console.log(`\nBenchmarking with ${numItems} items, deleting ${deletePercent * 100}%...`);

  const iterations = 10000;

  // Setup data
  const liveIds = new Set();
  const allIds = [];
  for (let i = 0; i < numItems; i++) {
    allIds.push(i);
    if (Math.random() >= deletePercent) {
      liveIds.add(i);
    }
  }

  // Warmup and measure current
  let currentTotalTime = 0;
  for (let iter = 0; iter < iterations; iter++) {
    const client = new HotasConfigClient();
    for (const id of allIds) {
      client.descriptorCache.set(id, 'data');
      client.elementMetaByDevice.set(id, 'meta');
    }

    const start = performance.now();
    client.currentGetDevices(liveIds);
    const end = performance.now();
    currentTotalTime += (end - start);
  }

  // Warmup and measure optimized
  let optimizedTotalTime = 0;
  for (let iter = 0; iter < iterations; iter++) {
    const client = new HotasConfigClient();
    for (const id of allIds) {
      client.descriptorCache.set(id, 'data');
      client.elementMetaByDevice.set(id, 'meta');
    }

    const start = performance.now();
    client.optimizedGetDevices(liveIds);
    const end = performance.now();
    optimizedTotalTime += (end - start);
  }

  console.log(`Current:   ${currentTotalTime.toFixed(2)} ms`);
  console.log(`Optimized: ${optimizedTotalTime.toFixed(2)} ms`);
  const improvement = ((currentTotalTime - optimizedTotalTime) / currentTotalTime) * 100;
  console.log(`Improvement: ${improvement.toFixed(2)}%`);
}

// Run benchmarks
runBenchmark(10, 0.5);
runBenchmark(100, 0.5);
runBenchmark(1000, 0.5);
