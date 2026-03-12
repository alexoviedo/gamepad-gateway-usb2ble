/** @import { FirmwareArtifactMeta, FirmwareFeedMeta, FirmwareFeatureSupport } from './shared/types.js' */

import { byId, errorMessage, requireElement, setHref, setStatusPill, setText } from './shared/dom.js';
import {
  HOTAS_FIRMWARE_FEEDS,
  HOTAS_FLASHER_SCRIPT,
  absoluteUrl,
  firmwareFeatureSupport,
  formatDate,
  loadFirmwareFeed,
  shortenHash,
  validateManifestUrl,
} from './shared/firmware-release.js';

/** @type {Promise<void> | null} */
let espWebToolsLoadPromise = null;

/**
 * @returns {Promise<void>}
 */
async function ensureEspWebToolsLoaded() {
  if (window.customElements?.get('esp-web-install-button')) return;
  if (espWebToolsLoadPromise) return espWebToolsLoadPromise;

  espWebToolsLoadPromise = new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.type = 'module';
    script.src = HOTAS_FLASHER_SCRIPT;
    script.onload = () => resolve();
    script.onerror = () => reject(new Error('Unable to load ESP Web Tools installer component.'));
    document.head.appendChild(script);
  });

  return espWebToolsLoadPromise;
}

/**
 * @param {Element | null} target
 * @param {string} manifestUrl
 */
function renderInstallButton(target, manifestUrl) {
  if (!(target instanceof HTMLElement)) return;
  target.replaceChildren();

  const install = document.createElement('esp-web-install-button');
  install.setAttribute('manifest', manifestUrl);
  install.className = 'firmware-install-button';
  target.appendChild(install);
}

/**
 * @param {Element | null} container
 * @param {FirmwareFeedMeta | null} meta
 * @param {string | null | undefined} assetBaseUrl
 */
function buildDownloadsList(container, meta, assetBaseUrl) {
  if (!(container instanceof HTMLElement)) return;

  const artifacts = meta?.artifacts ?? {};
  const entries = Object.entries(artifacts);
  if (entries.length === 0) {
    container.innerHTML = '<div class="empty-state">No published binary artifacts were listed in the manifest.</div>';
    return;
  }

  container.replaceChildren();
  for (const [key, artifactValue] of entries) {
    const artifact = /** @type {FirmwareArtifactMeta} */ (artifactValue);
    const row = document.createElement('div');
    row.className = 'download-row';

    const left = document.createElement('div');
    left.className = 'download-row-copy';

    const title = document.createElement('strong');
    title.textContent = artifact.label || key;

    const metaLine = document.createElement('span');
    const offsetText = artifact.offset ?? '—';
    metaLine.textContent = `Offset ${offsetText} · SHA ${shortenHash(artifact.sha256, 12)}`;
    left.append(title, metaLine);

    const link = document.createElement('a');
    link.className = 'btn';
    link.target = '_blank';
    link.rel = 'noreferrer';
    link.href = absoluteUrl(artifact.path, assetBaseUrl || window.location.href);
    link.textContent = 'Download';

    row.append(left, link);
    container.appendChild(row);
  }
}

/**
 * @param {FirmwareFeatureSupport} support
 * @returns {{ text: string, tone: 'success' | 'warn' }}
 */
function buildCompatCopy(support) {
  if (!support.secure) {
    return {
      text: 'Open this page from https:// or http://localhost before attempting browser-based flashing.',
      tone: 'warn',
    };
  }

  if (support.serial) {
    return {
      text: 'Direct USB flashing is available in this browser. Use a data-capable USB cable and close other serial tools first.',
      tone: 'success',
    };
  }

  return {
    text: 'This browser does not expose the required serial device APIs here. Desktop Chrome or Edge is the recommended path for one-click flashing.',
    tone: 'warn',
  };
}

/**
 * @param {FirmwareFeedMeta | null} meta
 * @param {string} sourceUrl
 * @returns {Promise<void>}
 */
async function hydrateFirmwareUi(meta, sourceUrl) {
  const support = firmwareFeatureSupport();
  const compat = buildCompatCopy(support);

  setStatusPill(
    'firmwareCompatPill',
    support.serial ? 'USB flashing ready' : 'Desktop Chromium recommended',
    compat.tone,
  );
  setText('firmwareCompatText', compat.text);
  setText('firmwareVersion', meta?.version || 'No published firmware');
  setText('firmwareChannel', meta?.channel || '—');
  setText('firmwareChip', meta?.chipFamily || 'ESP32-S3');
  setText('firmwarePublishedAt', formatDate(meta?.publishedAt));
  setText('firmwareSourceUrl', sourceUrl || '—');
  setText('firmwareBuild', meta?.build || '—');
  setText('firmwareBootSha', shortenHash(meta?.checksums?.bootloader));
  setText('firmwarePartSha', shortenHash(meta?.checksums?.partitionTable));
  setText('firmwareAppSha', shortenHash(meta?.checksums?.app));
  setText('firmwareReleaseNotes', meta?.releaseNotes || 'No release notes were published with this manifest.');
  setHref('firmwareReleaseLink', meta?.releaseUrl, 'Open release notes');

  const quickStatus = byId('firmwareQuickStatus');
  if (quickStatus) {
    quickStatus.textContent = meta?.manifestUrl ? `Ready to flash ${meta?.version || 'published build'}` : 'No published manifest yet';
  }

  const quickVersion = byId('firmwareQuickVersion');
  if (quickVersion) {
    quickVersion.textContent = meta?.version || 'Unavailable';
  }

  const quickNotes = byId('firmwareQuickNotes');
  if (quickNotes) {
    quickNotes.textContent = meta?.releaseNotes || 'Publish a release manifest to enable one-click flashing from the config console.';
  }

  buildDownloadsList(byId('firmwareDownloads'), meta, meta?.manifestUrl || sourceUrl);

  const installHost = byId('firmwareInstallHost');
  const manualManifestInput = byId('customManifestInput');
  const shouldRenderInstaller = Boolean(byId('flashPage'));

  if (meta?.manifestUrl && support.secure) {
    try {
      await validateManifestUrl(meta.manifestUrl);
      if (shouldRenderInstaller && installHost) {
        await ensureEspWebToolsLoaded();
        renderInstallButton(installHost, meta.manifestUrl);
      } else if (installHost) {
        installHost.innerHTML = '<div class="empty-state">Open the full firmware flasher page to launch the browser installer.</div>';
      }

      setStatusPill('firmwareManifestPill', 'Manifest ready', 'success');
      if (manualManifestInput instanceof HTMLInputElement && !manualManifestInput.value) {
        manualManifestInput.value = meta.manifestUrl;
      }
      return;
    } catch (error) {
      if (installHost) {
        installHost.innerHTML = `<div class="banner">${errorMessage(error)}</div>`;
      }
      setStatusPill('firmwareManifestPill', 'Installer unavailable', 'warn');
      return;
    }
  }

  if (installHost) {
    installHost.innerHTML = '<div class="empty-state">No published manifest is available yet. Publish a firmware release or load a custom manifest URL below.</div>';
  }
  setStatusPill('firmwareManifestPill', 'Awaiting release', 'warn');
}

/**
 * @param {string} url
 * @returns {Promise<FirmwareFeedMeta | null>}
 */
async function loadFeedIntoUi(url) {
  setStatusPill('firmwareFeedPill', 'Loading feed', 'muted');
  try {
    const meta = await loadFirmwareFeed(url);
    await hydrateFirmwareUi(meta, url);
    setStatusPill('firmwareFeedPill', `Loaded ${meta?.channel || 'feed'}`, 'success');
    return meta;
  } catch (error) {
    setStatusPill('firmwareFeedPill', 'Feed unavailable', 'warn');
    const installHost = byId('firmwareInstallHost');
    if (installHost) {
      installHost.innerHTML = `<div class="empty-state">${errorMessage(error)}</div>`;
    }
    setText('firmwareReleaseNotes', 'No published release feed could be loaded. You can still paste a custom manifest URL below for testing.');
    throw error;
  }
}

/**
 * @param {string} manifestUrl
 * @returns {Promise<void>}
 */
async function loadCustomManifest(manifestUrl) {
  if (!manifestUrl) {
    throw new Error('Enter a manifest URL first.');
  }

  setStatusPill('firmwareManifestPill', 'Loading manifest', 'muted');
  const absoluteManifestUrl = absoluteUrl(manifestUrl, window.location.href);
  await validateManifestUrl(absoluteManifestUrl);
  await ensureEspWebToolsLoaded();
  renderInstallButton(byId('firmwareInstallHost'), absoluteManifestUrl);
  setStatusPill('firmwareManifestPill', 'Custom manifest ready', 'success');
  setText('firmwareReleaseNotes', 'Custom manifest loaded for manual validation. Use published feeds for normal users.');
}

function attachFirmwarePageEvents() {
  const channelSelect = byId('firmwareChannelSelect');
  const refreshBtn = byId('refreshFirmwareFeedBtn');
  const loadCustomBtn = byId('loadCustomManifestBtn');
  const customManifestInput = byId('customManifestInput');

  if (channelSelect instanceof HTMLSelectElement) {
    channelSelect.addEventListener('change', async () => {
      const selectedValue = channelSelect.value;
      const url = selectedValue in HOTAS_FIRMWARE_FEEDS
        ? HOTAS_FIRMWARE_FEEDS[/** @type {'stable' | 'beta'} */ (selectedValue)]
        : selectedValue;
      try {
        await loadFeedIntoUi(url);
      } catch {
        // Status UI was already updated by loadFeedIntoUi.
      }
    });
  }

  if (refreshBtn instanceof HTMLButtonElement) {
    refreshBtn.addEventListener('click', async () => {
      const selected = channelSelect instanceof HTMLSelectElement ? channelSelect.value : 'stable';
      const url = selected in HOTAS_FIRMWARE_FEEDS
        ? HOTAS_FIRMWARE_FEEDS[/** @type {'stable' | 'beta'} */ (selected)]
        : selected;
      try {
        await loadFeedIntoUi(url);
      } catch {
        // Status UI was already updated by loadFeedIntoUi.
      }
    });
  }

  if (loadCustomBtn instanceof HTMLButtonElement && customManifestInput instanceof HTMLInputElement) {
    loadCustomBtn.addEventListener('click', async () => {
      try {
        await loadCustomManifest(customManifestInput.value.trim());
      } catch (error) {
        const installHost = byId('firmwareInstallHost');
        if (installHost) {
          installHost.innerHTML = `<div class="banner">${errorMessage(error)}</div>`;
        }
        setStatusPill('firmwareManifestPill', 'Custom manifest failed', 'warn');
      }
    });
  }
}

async function initializeFirmwareInstaller() {
  const quickCard = byId('firmwareQuickCard');
  const flashPage = byId('flashPage');
  if (!quickCard && !flashPage) return;

  attachFirmwarePageEvents();
  try {
    await loadFeedIntoUi(HOTAS_FIRMWARE_FEEDS.stable);
  } catch {
    const compat = buildCompatCopy(firmwareFeatureSupport());
    setText('firmwareCompatText', compat.text);
  }
}

void initializeFirmwareInstaller();
