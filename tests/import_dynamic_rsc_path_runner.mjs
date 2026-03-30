function assert(condition, message) {
  if (!condition) throw new Error(message);
}

import { loadRscIndexLater } from './ssr/index.js';

const filename = await loadRscIndexLater();

assert(
  filename.endsWith('/tests/rsc/index.js'),
  `late dynamic import resolved from wrong base: ${filename}`
);

console.log(`import.dynamic.rsc.path:${filename}`);
