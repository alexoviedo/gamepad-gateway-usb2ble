const HOTAS_FIRMWARE_FEEDS = {
  stable: './firmware/stable/latest.json',
  beta: './firmware/beta/latest.json',
};

const HOTAS_FLASHER_SCRIPT = 'https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module';

function firmwareFeatureSupport() {
  return {
    bluetooth: 'bluetooth' in navigator,
    serial: 'serial' in navigator,
    usb: 'usb' in navigator,
    secure: window.isSecureContext,
  };
}

function shortenHash(value, keep = 10) {
  if (!value || typeof value !== 'string') return '—';
  return value.length > keep ? `${value.slice(0, keep)}…` : value;
}

function formatDate(value) {
  if (!value) return '—';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return date.toLocaleString();
}

function absoluteUrl(value, base = window.location.href) {
  try {
    return new URL(value, base).toString();
  } catch {
    return value;
  }
}

async function fetchJson(url) {
  const response = await fetch(url, { cache: 'no-store' });
  if (!response.ok) {
    throw new Error(`Request failed (${response.status}) for ${url}`);
  }
  return response.json();
}

async function ensureEspWebToolsLoaded() {
  if (window.customElements?.get('esp-web-install-button')) return;
  if (window.__hotasEspWebToolsPromise) return window.__hotasEspWebToolsPromise;

  window.__hotasEspWebToolsPromise = new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.type = 'module';
    script.src = HOTAS_FLASHER_SCRIPT;
    script.onload = () => resolve();
    script.onerror = () => reject(new Error('Unable to load ESP Web Tools installer component.'));
    document.head.appendChild(script);
  });

  return window.__hotasEspWebToolsPromise;
}

function renderInstallButton(target, manifestUrl) {
  if (!target) return;
  target.innerHTML = '';
  const install = document.createElement('esp-web-install-button');
  install.setAttribute('manifest', manifestUrl);
  install.className = 'firmware-install-button';
  target.appendChild(install);
}

function setText(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function setHref(id, value, labelFallback = 'Open') {
  const el = document.getElementById(id);
  if (!el) return;
  if (value) {
    el.href = value;
    el.classList.remove('hidden');
    if (!el.textContent.trim()) el.textContent = labelFallback;
  } else {
    el.classList.add('hidden');
  }
}

function setStatusPill(id, text, tone = 'muted') {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  el.className = `pill ${tone}`;
}

function buildDownloadsList(container, meta, assetBaseUrl) {
  if (!container) return;
  const artifacts = meta?.artifacts || {};
  const entries = Object.entries(artifacts);
  if (!entries.length) {
    container.innerHTML = '<div class="empty-state">No published binary artifacts were listed in the manifest.</div>';
    return;
  }

  container.innerHTML = '';
  for (const [key, artifact] of entries) {
    const row = document.createElement('div');
    row.className = 'download-row';
    const left = document.createElement('div');
    left.className = 'download-row-copy';
    left.innerHTML = `
      <strong>${artifact.label || key}</strong>
      <span>Offset ${artifact.offset ?? '—'} · SHA ${shortenHash(artifact.sha256, 12)}</span>
    `;
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

function normalizeManifestMeta(meta, metaUrl) {
  if (!meta || typeof meta !== 'object') return null;
  return {
    ...meta,
    feedUrl: metaUrl,
    manifestUrl: meta.manifestUrl ? absoluteUrl(meta.manifestUrl, metaUrl) : meta.manifestPath ? absoluteUrl(meta.manifestPath, metaUrl) : null,
  };
}

async function loadFirmwareFeed(url) {
  const feedUrl = absoluteUrl(url, window.location.href);
  const raw = await fetchJson(feedUrl);
  return normalizeManifestMeta(raw, feedUrl);
}

async function hydrateFirmwareUi(meta, sourceUrl) {
  const support = firmwareFeatureSupport();
  const compat = buildCompatCopy(support);

  setStatusPill('firmwareCompatPill', support.serial ? 'USB flashing ready' : 'Desktop Chromium recommended', support.serial ? 'success' : 'warn');
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

  const quickStatus = document.getElementById('firmwareQuickStatus');
  if (quickStatus) {
    quickStatus.textContent = meta?.manifestUrl ? `Ready to flash ${meta.version}` : 'No published manifest yet';
  }

  const quickVersion = document.getElementById('firmwareQuickVersion');
  if (quickVersion) {
    quickVersion.textContent = meta?.version || 'Unavailable';
  }

  const quickNotes = document.getElementById('firmwareQuickNotes');
  if (quickNotes) {
    quickNotes.textContent = meta?.releaseNotes || 'Publish a release manifest to enable one-click flashing from the config console.';
  }

  const downloads = document.getElementById('firmwareDownloads');
  buildDownloadsList(downloads, meta, meta?.manifestUrl || sourceUrl);

  const installHost = document.getElementById('firmwareInstallHost');
  const manualManifestInput = document.getElementById('customManifestInput');
  if (meta?.manifestUrl && support.secure) {
    try {
      await ensureEspWebToolsLoaded();
      renderInstallButton(installHost, meta.manifestUrl);
      setStatusPill('firmwareManifestPill', 'Manifest ready', 'success');
      if (manualManifestInput && !manualManifestInput.value) manualManifestInput.value = meta.manifestUrl;
    } catch (error) {
      if (installHost) {
        installHost.innerHTML = `<div class="banner">${error.message}</div>`;
      }
      setStatusPill('firmwareManifestPill', 'Installer unavailable', 'warn');
    }
  } else {
    if (installHost) {
      installHost.innerHTML = '<div class="empty-state">No published manifest is available yet. Publish a firmware release or load a custom manifest URL below.</div>';
    }
    setStatusPill('firmwareManifestPill', 'Awaiting release', 'warn');
  }
}

async function loadFeedIntoUi(url) {
  setStatusPill('firmwareFeedPill', 'Loading feed', 'muted');
  try {
    const meta = await loadFirmwareFeed(url);
    await hydrateFirmwareUi(meta, url);
    setStatusPill('firmwareFeedPill', `Loaded ${meta.channel || 'feed'}`, 'success');
    return meta;
  } catch (error) {
    setStatusPill('firmwareFeedPill', 'Feed unavailable', 'warn');
    const installHost = document.getElementById('firmwareInstallHost');
    if (installHost) {
      installHost.innerHTML = `<div class="empty-state">${error.message}</div>`;
    }
    setText('firmwareReleaseNotes', 'No published release feed could be loaded. You can still paste a custom manifest URL below for testing.');
    throw error;
  }
}

async function loadCustomManifest(manifestUrl) {
  if (!manifestUrl) throw new Error('Enter a manifest URL first.');
  setStatusPill('firmwareManifestPill', 'Loading manifest', 'muted');
  await ensureEspWebToolsLoaded();
  renderInstallButton(document.getElementById('firmwareInstallHost'), manifestUrl);
  setStatusPill('firmwareManifestPill', 'Custom manifest ready', 'success');
  setText('firmwareReleaseNotes', 'Custom manifest loaded for manual validation. Use published feeds for normal users.');
}

function attachFirmwarePageEvents() {
  const channelSelect = document.getElementById('firmwareChannelSelect');
  const refreshBtn = document.getElementById('refreshFirmwareFeedBtn');
  const loadCustomBtn = document.getElementById('loadCustomManifestBtn');
  const customManifestInput = document.getElementById('customManifestInput');

  if (channelSelect) {
    channelSelect.addEventListener('change', async () => {
      const url = HOTAS_FIRMWARE_FEEDS[channelSelect.value] || channelSelect.value;
      try {
        await loadFeedIntoUi(url);
      } catch {}
    });
  }

  if (refreshBtn) {
    refreshBtn.addEventListener('click', async () => {
      const selected = channelSelect?.value || 'stable';
      const url = HOTAS_FIRMWARE_FEEDS[selected] || selected;
      try {
        await loadFeedIntoUi(url);
      } catch {}
    });
  }

  if (loadCustomBtn && customManifestInput) {
    loadCustomBtn.addEventListener('click', async () => {
      try {
        await loadCustomManifest(customManifestInput.value.trim());
      } catch (error) {
        const installHost = document.getElementById('firmwareInstallHost');
        if (installHost) installHost.innerHTML = `<div class="banner">${error.message}</div>`;
        setStatusPill('firmwareManifestPill', 'Custom manifest failed', 'warn');
      }
    });
  }
}

async function initializeFirmwareInstaller() {
  const quickCard = document.getElementById('firmwareQuickCard');
  const flashPage = document.getElementById('flashPage');
  if (!quickCard && !flashPage) return;

  attachFirmwarePageEvents();
  try {
    await loadFeedIntoUi(HOTAS_FIRMWARE_FEEDS.stable);
  } catch {
    const compat = buildCompatCopy(firmwareFeatureSupport());
    setText('firmwareCompatText', compat.text);
  }
}

initializeFirmwareInstaller();
