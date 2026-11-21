import { fromMiddle } from './middle.mjs';
import { late } from './slower.mjs';
export const combined = fromMiddle + late;
await 0;
export const afterOwnAwait = combined + '?';
