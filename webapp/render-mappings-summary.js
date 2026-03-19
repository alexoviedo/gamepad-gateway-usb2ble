globalThis.renderMappingsSummary = function () {
  // `guarded()` in app.js always calls `render()` after successful actions.
  // This compatibility shim preserves the existing call sites during the
  // incremental web-console cleanup without surfacing a false user-facing error.
};
