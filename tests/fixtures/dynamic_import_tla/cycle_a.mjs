import { nameB } from './cycle_b.mjs';
import { tick } from './cycle_wait.mjs';
globalThis.dynamicImportTlaLog.push(`cycle a after ${tick}`);
export const name = 'A';
export const fromB = () => nameB;
