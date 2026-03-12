/**
 * Shared JSDoc typedefs for the client-side web app.
 * This file intentionally exports no runtime values beyond a trivial sentinel.
 */

/**
 * @typedef {Object} FirmwareFeatureSupport
 * @property {boolean} bluetooth
 * @property {boolean} serial
 * @property {boolean} usb
 * @property {boolean} secure
 */

/**
 * @typedef {Object} FirmwareArtifactMeta
 * @property {string=} label
 * @property {number|string=} offset
 * @property {string=} sha256
 * @property {string} path
 */

/**
 * @typedef {Object} FirmwareBuildPart
 * @property {number|string=} offset
 * @property {string} path
 */

/**
 * @typedef {Object} FirmwareBuild
 * @property {FirmwareBuildPart[]} parts
 */

/**
 * @typedef {Object} FirmwareManifest
 * @property {FirmwareBuild[]} builds
 */

/**
 * @typedef {Object} FirmwareChecksums
 * @property {string=} bootloader
 * @property {string=} partitionTable
 * @property {string=} app
 */

/**
 * @typedef {Object.<string, FirmwareArtifactMeta>} FirmwareArtifacts
 */

/**
 * @typedef {Object} FirmwareFeedMeta
 * @property {string=} version
 * @property {string=} channel
 * @property {string=} chipFamily
 * @property {string=} publishedAt
 * @property {string=} build
 * @property {string=} releaseNotes
 * @property {string=} releaseUrl
 * @property {string=} manifestUrl
 * @property {string=} manifestPath
 * @property {FirmwareChecksums=} checksums
 * @property {FirmwareArtifacts=} artifacts
 * @property {string=} feedUrl
 */

export const TYPES_MODULE_LOADED = true;
