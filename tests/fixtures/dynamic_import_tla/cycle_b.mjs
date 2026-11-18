import { name } from './cycle_a.mjs';
import { tick } from './cycle_wait.mjs';
globalThis.dynamicImportTlaLog.push(`cycle b after ${tick}`);
export const nameB = 'B';
export const fromA = () => name;
