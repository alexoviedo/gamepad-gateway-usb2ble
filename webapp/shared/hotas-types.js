/**
 * Shared JSDoc typedefs for the HOTAS config web UI.
 */

/** @typedef {'x'|'y'|'z'|'rx'|'ry'|'rz'|'slider1'|'slider2'|'hat'} OutputKey */
/** @typedef {'axis'|'button'|'hat'|'other'} ElementKind */
/** @typedef {'axis'|'hat'} TargetKind */

/**
 * @typedef {Object} OutputTarget
 * @property {OutputKey} key
 * @property {string} label
 * @property {string} prompt
 */

/**
 * @typedef {Object} ValidationOutputMeta
 * @property {OutputKey} key
 * @property {string} label
 * @property {'axis'|'hat'} kind
 * @property {'bipolar'|'slider'|'hat'} visual
 */

/**
 * @typedef {Object} DeadzoneConfig
 * @property {number} inner
 * @property {number} outer
 */

/**
 * @typedef {Object} BezierPoint
 * @property {number} x
 * @property {number} y
 */

/**
 * @typedef {Object} BezierCurve
 * @property {'bezier'} type
 * @property {BezierPoint} p1
 * @property {BezierPoint} p2
 */

/**
 * @typedef {Object} AxisConfig
 * @property {boolean} configured
 * @property {number} device_id
 * @property {number} element_id
 * @property {boolean} invert
 * @property {DeadzoneConfig} deadzone
 * @property {number} smoothing_alpha
 * @property {BezierCurve} curve
 */

/**
 * @typedef {Object} ProfileConfig
 * @property {number} version
 * @property {Partial<Record<OutputKey, AxisConfig>>} axes
 */

/**
 * @typedef {Object} ElementMeta
 * @property {number} element_id
 * @property {ElementKind} kind
 * @property {number} usage_page
 * @property {number} usage
 * @property {string} usage_name
 */

/**
 * @typedef {Object} DeviceSummary
 * @property {number} device_id
 * @property {string} [role]
 * @property {number} [num_elements]
 * @property {number} [report_desc_len]
 * @property {number|string} [dev_addr]
 */

/**
 * @typedef {Object} StreamSample
 * @property {number} version
 * @property {number} flags
 * @property {number} deviceId
 * @property {number} elementId
 * @property {number} raw
 * @property {number} normQ15
 * @property {number} norm
 * @property {Date} receivedAt
 * @property {ElementMeta=} meta
 */

/**
 * @typedef {Object} PreviewState
 * @property {number} raw
 * @property {number} deadzoned
 * @property {number} curved
 * @property {number} smoothed
 * @property {number} [markerInput]
 * @property {number} [markerOutput]
 * @property {Date|null} updatedAt
 */

/**
 * @typedef {PreviewState & {
 *   configured: boolean,
 *   mapping: AxisConfig,
 *   sample: StreamSample|null,
 * }} ValidationMappedOutput
 */

/**
 * @typedef {Object} ChunkAssemblyState
 * @property {number} version
 * @property {number} type
 * @property {number} total
 * @property {Uint8Array} buffer
 * @property {number} received
 * @property {Set<number>} offsets
 */

/**
 * @typedef {Object} ChunkedMessage
 * @property {number} version
 * @property {number} type
 * @property {number} msgId
 * @property {Uint8Array} bytes
 */

/**
 * @typedef {Object} PendingJsonRequest
 * @property {(value: BridgeCommandResponse) => void} resolve
 * @property {(reason?: unknown) => void} reject
 * @property {BridgeCommandRequest=} payload
 */

/**
 * @typedef {Object} PendingDescriptorBinary
 * @property {number} deviceId
 * @property {number} rid
 */

/**
 * @typedef {Object} BridgeCommandRequest
 * @property {number=} rid
 * @property {string} cmd
 * @property {number=} device_id
 * @property {unknown=} config
 */

/**
 * @typedef {Object} BridgeCommandResponse
 * @property {number=} rid
 * @property {boolean=} ok
 * @property {string=} note
 * @property {DeviceSummary[]=} devices
 * @property {number=} device_id
 * @property {unknown[]=} elements
 * @property {unknown=} config
 * @property {string=} config_json
 * @property {string=} message
 * @property {string=} error
 * @property {Record<string, unknown>=} extra
 */

/**
 * @typedef {Object} CharacteristicMap
 * @property {BluetoothRemoteGATTCharacteristic=} cmd
 * @property {BluetoothRemoteGATTCharacteristic=} evt
 * @property {BluetoothRemoteGATTCharacteristic=} stream
 * @property {BluetoothRemoteGATTCharacteristic=} cfg
 */

/**
 * @typedef {Object} WizardDetectorEntry
 * @property {string} key
 * @property {number} deviceId
 * @property {number} elementId
 * @property {ElementKind} kind
 * @property {ElementMeta|null|undefined} meta
 * @property {number} score
 * @property {number} totalDelta
 * @property {number} maxMagnitude
 * @property {number} activeCount
 * @property {number} sampleCount
 * @property {number|null} lastNorm
 * @property {number|null} lastRaw
 */

/**
 * @typedef {WizardDetectorEntry & { weightedScore?: number }} WizardCandidate
 */


/**
 * @typedef {Partial<Record<OutputKey, ValidationMappedOutput>>} ValidationMappedOutputMap
 */

export const HOTAS_TYPES_READY = true;
