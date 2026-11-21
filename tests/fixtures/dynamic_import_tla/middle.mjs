import { value } from './slow.mjs';
let seen;
try { seen = value; } catch (error) { seen = error.constructor.name; }
globalThis.dynamicImportTlaLog.push(`middle sees ${seen}`);
export const fromMiddle = seen;
export const url = import.meta.url;
