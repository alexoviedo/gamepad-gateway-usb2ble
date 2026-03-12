/**
 * @param {unknown} error
 * @returns {string}
 */
export function errorMessage(error) {
  if (error instanceof Error && error.message) return error.message;
  if (typeof error === 'string' && error.trim()) return error;
  try {
    return JSON.stringify(error);
  } catch {
    return 'Unknown error';
  }
}

/**
 * @param {string} id
 * @returns {Element | null}
 */
export function byId(id) {
  return document.getElementById(id);
}

/**
 * @template {Element} T
 * @param {string} id
 * @returns {T}
 */
export function requireElement(id) {
  const element = document.getElementById(id);
  if (!(element instanceof Element)) {
    throw new Error(`Required element #${id} was not found.`);
  }
  return /** @type {T} */ (/** @type {unknown} */ (element));
}

/**
 * @param {string} id
 * @param {string} value
 */
export function setText(id, value) {
  const element = byId(id);
  if (element) {
    element.textContent = value;
  }
}

/**
 * @param {string} id
 * @param {string | null | undefined} href
 * @param {string} [labelFallback='Open']
 */
export function setHref(id, href, labelFallback = 'Open') {
  const element = byId(id);
  if (!(element instanceof HTMLAnchorElement)) return;

  if (href) {
    element.href = href;
    element.classList.remove('hidden');
    if (!element.textContent || !element.textContent.trim()) {
      element.textContent = labelFallback;
    }
    return;
  }

  element.removeAttribute('href');
  element.classList.add('hidden');
}

/**
 * @param {string} id
 * @param {string} text
 * @param {'muted' | 'warn' | 'success' | 'accent'} [tone='muted']
 */
export function setStatusPill(id, text, tone = 'muted') {
  const element = byId(id);
  if (!element) return;
  element.textContent = text;
  element.className = `pill ${tone}`;
}
