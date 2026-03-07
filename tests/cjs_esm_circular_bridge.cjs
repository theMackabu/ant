const esm = require("./cjs_esm_circular_entry.mjs");

exports.cjsValue = "cjs-value";
Object.defineProperty(exports, "esmValueSeen", {
  enumerable: true,
  get() {
    return esm.esmValue;
  },
});
