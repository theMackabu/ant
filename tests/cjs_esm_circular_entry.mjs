import bridge from "./cjs_esm_circular_bridge.cjs";

export const esmValue = "esm-value";
export const fromCjs = bridge.cjsValue;
