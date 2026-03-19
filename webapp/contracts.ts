export const AXIS_KEYS = [
  'x',
  'y',
  'z',
  'rx',
  'ry',
  'rz',
  'slider1',
  'slider2',
  'hat',
] as const;

export type AxisKey = (typeof AXIS_KEYS)[number];

export interface BezierPoint {
  x: number;
  y: number;
}

export interface AxisCurve {
  type: 'bezier';
  p1: BezierPoint;
  p2: BezierPoint;
}

export interface AxisDeadzone {
  inner: number;
  outer: number;
}

export interface AxisConfig {
  configured: boolean;
  device_id: number;
  element_id: number;
  invert: boolean;
  deadzone: AxisDeadzone;
  smoothing_alpha: number;
  curve: AxisCurve;
}

export interface BridgeConfig {
  version: 2;
  replace_all?: boolean;
  axes: Partial<Record<AxisKey, AxisConfig>>;
}

export interface FirmwareArtifact {
  label?: string;
  offset: string;
  path: string;
  sha256: string;
}

export interface FirmwareManifestMeta {
  version: string;
  channel?: string;
  chipFamily?: string;
  build?: string;
  manifestUrl?: string | null;
  manifestPath?: string | null;
  publishedAt?: string;
  releaseNotes?: string;
  releaseUrl?: string;
  checksums?: {
    bootloader?: string;
    partitionTable?: string;
    app?: string;
  };
  artifacts?: Record<string, FirmwareArtifact>;
}

function assert(condition: unknown, message: string): asserts condition {
  if (!condition) {
    throw new Error(message);
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function expectNumber(value: unknown, label: string): number {
  assert(typeof value === 'number' && Number.isFinite(value), `${label} must be a finite number.`);
  return value;
}

function expectBoolean(value: unknown, label: string): boolean {
  assert(typeof value === 'boolean', `${label} must be a boolean.`);
  return value;
}

function expectString(value: unknown, label: string): string {
  assert(typeof value === 'string' && value.length > 0, `${label} must be a non-empty string.`);
  return value;
}

function expectPoint(value: unknown, label: string): BezierPoint {
  assert(isRecord(value), `${label} must be an object.`);
  return {
    x: expectNumber(value.x, `${label}.x`),
    y: expectNumber(value.y, `${label}.y`),
  };
}

export function validateAxisConfig(value: unknown, label = 'axis'): AxisConfig {
  assert(isRecord(value), `${label} must be an object.`);
  assert(isRecord(value.deadzone), `${label}.deadzone must be an object.`);
  assert(isRecord(value.curve), `${label}.curve must be an object.`);

  const curveType = value.curve.type;
  assert(curveType === 'bezier', `${label}.curve.type must be "bezier".`);

  return {
    configured: expectBoolean(value.configured, `${label}.configured`),
    device_id: expectNumber(value.device_id, `${label}.device_id`),
    element_id: expectNumber(value.element_id, `${label}.element_id`),
    invert: expectBoolean(value.invert, `${label}.invert`),
    deadzone: {
      inner: expectNumber(value.deadzone.inner, `${label}.deadzone.inner`),
      outer: expectNumber(value.deadzone.outer, `${label}.deadzone.outer`),
    },
    smoothing_alpha: expectNumber(value.smoothing_alpha, `${label}.smoothing_alpha`),
    curve: {
      type: 'bezier',
      p1: expectPoint(value.curve.p1, `${label}.curve.p1`),
      p2: expectPoint(value.curve.p2, `${label}.curve.p2`),
    },
  };
}

export function validateBridgeConfig(value: unknown): BridgeConfig {
  assert(isRecord(value), 'config must be an object.');
  assert(value.version === 2, 'config.version must be 2.');
  assert(isRecord(value.axes), 'config.axes must be an object.');

  const axes: BridgeConfig['axes'] = {};
  for (const axisKey of AXIS_KEYS) {
    const axisValue = value.axes[axisKey];
    if (axisValue != null) {
      axes[axisKey] = validateAxisConfig(axisValue, `config.axes.${axisKey}`);
    }
  }

  if (value.replace_all != null) {
    assert(typeof value.replace_all === 'boolean', 'config.replace_all must be a boolean when present.');
  }

  return {
    version: 2,
    replace_all: value.replace_all as boolean | undefined,
    axes,
  };
}

export function validateFirmwareManifestMeta(value: unknown): FirmwareManifestMeta {
  assert(isRecord(value), 'firmware manifest metadata must be an object.');

  const artifacts = value.artifacts;
  if (artifacts != null) {
    assert(isRecord(artifacts), 'artifacts must be an object when present.');
    for (const [name, artifactValue] of Object.entries(artifacts)) {
      assert(isRecord(artifactValue), `artifacts.${name} must be an object.`);
      expectString(artifactValue.offset, `artifacts.${name}.offset`);
      expectString(artifactValue.path, `artifacts.${name}.path`);
      expectString(artifactValue.sha256, `artifacts.${name}.sha256`);
      if (artifactValue.label != null) {
        expectString(artifactValue.label, `artifacts.${name}.label`);
      }
    }
  }

  if (value.checksums != null) {
    assert(isRecord(value.checksums), 'checksums must be an object when present.');
  }

  return {
    version: expectString(value.version, 'version'),
    channel: value.channel == null ? undefined : expectString(value.channel, 'channel'),
    chipFamily: value.chipFamily == null ? undefined : expectString(value.chipFamily, 'chipFamily'),
    build: value.build == null ? undefined : expectString(value.build, 'build'),
    manifestUrl: value.manifestUrl == null ? null : expectString(value.manifestUrl, 'manifestUrl'),
    manifestPath: value.manifestPath == null ? null : expectString(value.manifestPath, 'manifestPath'),
    publishedAt: value.publishedAt == null ? undefined : expectString(value.publishedAt, 'publishedAt'),
    releaseNotes: value.releaseNotes == null ? undefined : expectString(value.releaseNotes, 'releaseNotes'),
    releaseUrl: value.releaseUrl == null ? undefined : expectString(value.releaseUrl, 'releaseUrl'),
    checksums: value.checksums == null
      ? undefined
      : {
          bootloader: value.checksums.bootloader == null ? undefined : expectString(value.checksums.bootloader, 'checksums.bootloader'),
          partitionTable: value.checksums.partitionTable == null ? undefined : expectString(value.checksums.partitionTable, 'checksums.partitionTable'),
          app: value.checksums.app == null ? undefined : expectString(value.checksums.app, 'checksums.app'),
        },
    artifacts: value.artifacts == null ? undefined : (value.artifacts as Record<string, FirmwareArtifact>),
  };
}
