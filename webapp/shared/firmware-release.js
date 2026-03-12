/** @import { FirmwareFeedMeta, FirmwareFeatureSupport, FirmwareManifest } from './types.js' */

export const HOTAS_FIRMWARE_FEEDS = Object.freeze({
  stable: './firmware/stable/latest.json',
  beta: './firmware/beta/latest.json',
});

export const HOTAS_FLASHER_SCRIPT = 'https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module';

/**
 * @returns {FirmwareFeatureSupport}
 */
export function firmwareFeatureSupport() {
  const nav = /** @type {Navigator & { bluetooth?: unknown, serial?: unknown, usb?: unknown }} */ (navigator);
  return {
    bluetooth: 'bluetooth' in nav,
    serial: 'serial' in nav,
    usb: 'usb' in nav,
    secure: window.isSecureContext,
  };
}

/**
 * @param {string | null | undefined} value
 * @param {number} [keep=10]
 * @returns {string}
 */
export function shortenHash(value, keep = 10) {
  if (!value || typeof value !== 'string') return '—';
  return value.length > keep ? `${value.slice(0, keep)}…` : value;
}

/**
 * @param {string | null | undefined} value
 * @returns {string}
 */
export function formatDate(value) {
  if (!value) return '—';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return date.toLocaleString();
}

/**
 * @param {string} value
 * @param {string} [base=window.location.href]
 * @returns {string}
 */
export function absoluteUrl(value, base = window.location.href) {
  try {
    return new URL(value, base).toString();
  } catch {
    return value;
  }
}

/**
 * @template T
 * @param {string} url
 * @returns {Promise<T>}
 */
export async function fetchJson(url) {
  const response = await fetch(url, { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`Request failed (${response.status}) for ${url}`);
  }
  return /** @type {Promise<T>} */ (response.json());
}

/**
 * @param {string} manifestUrl
 * @returns {Promise<FirmwareManifest>}
 */
export async function validateManifestUrl(manifestUrl) {
  const manifest = await fetchJson(manifestUrl);
  if (!manifest || typeof manifest !== 'object') {
    throw new Error('Manifest response was not valid JSON.');
  }

  const typedManifest = /** @type {FirmwareManifest} */ (manifest);
  if (!Array.isArray(typedManifest.builds) || typedManifest.builds.length === 0) {
    throw new Error('Manifest did not contain any builds.');
  }

  const firstBuild = typedManifest.builds[0];
  if (!Array.isArray(firstBuild?.parts) || firstBuild.parts.length === 0) {
    throw new Error('Manifest did not contain any flash parts.');
  }

  return typedManifest;
}

/**
 * @param {unknown} meta
 * @param {string} metaUrl
 * @returns {FirmwareFeedMeta | null}
 */
export function normalizeManifestMeta(meta, metaUrl) {
  if (!meta || typeof meta !== 'object' || Array.isArray(meta)) return null;

  const typedMeta = /** @type {FirmwareFeedMeta} */ (meta);
  return {
    ...typedMeta,
    feedUrl: metaUrl,
    manifestUrl: typedMeta.manifestUrl
      ? absoluteUrl(typedMeta.manifestUrl, metaUrl)
      : typedMeta.manifestPath
        ? absoluteUrl(typedMeta.manifestPath, metaUrl)
        : undefined,
  };
}

/**
 * @param {string} url
 * @returns {Promise<FirmwareFeedMeta | null>}
 */
export async function loadFirmwareFeed(url) {
  const feedUrl = absoluteUrl(url, window.location.href);
  const raw = await fetchJson(feedUrl);
  return normalizeManifestMeta(raw, feedUrl);
}
