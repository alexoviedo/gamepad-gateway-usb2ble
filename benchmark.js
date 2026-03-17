
import { JSDOM } from 'jsdom';

const dom = new JSDOM('<!DOCTYPE html><div id="telemetryList"></div>');
const document = dom.window.document;
const elements = {
  telemetryList: document.getElementById('telemetryList'),
};

const history = Array.from({ length: 64 }, (_, i) => ({
  deviceId: 1,
  elementId: i,
  raw: 123,
  normQ15: 456,
  receivedAt: new Date(),
  meta: {
    kind: 'axis',
    usage_name: 'X',
    element_id: i,
  }
}));

function describeElement(meta) {
  if (!meta) return 'Unknown element';
  const usage = meta.usage_name && meta.usage_name !== 'unknown'
    ? meta.usage_name
    : `Usage ${meta.usage_page}:${meta.usage}`;
  return `${meta.kind} · ${usage} · element ${meta.element_id}`;
}

function formatQ15(value) {
  return (value / 32767).toFixed(4);
}

function originalRender() {
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

function optimizedRender() {
  elements.telemetryList.className = 'telemetry-list';
  let html = '';

  for (const sample of history.slice(0, 24)) {
    html += `
      <div class="telemetry-row">
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
      </div>
    `;
  }
  elements.telemetryList.innerHTML = html;
}

const ITERATIONS = 1000;

console.time('Original');
for (let i = 0; i < ITERATIONS; i++) {
  originalRender();
}
console.timeEnd('Original');

console.time('Optimized');
for (let i = 0; i < ITERATIONS; i++) {
  optimizedRender();
}
console.timeEnd('Optimized');
