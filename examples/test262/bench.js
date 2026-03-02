import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';

const t0 = performance.now();
const raw = readFileSync(new URL('./ant.json', import.meta.url), 'utf-8');
console.log('Load:', (performance.now() - t0).toFixed(2), 'ms');

const t1 = performance.now();
JSON.parse(raw);
console.log('Parse:', (performance.now() - t1).toFixed(2), 'ms');
