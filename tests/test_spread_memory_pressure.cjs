// a spread object's shape and overflow slots live outside the GC heap but must still prompt
// collections; repeatedly spreading a large array used to grow memory without bound
const assert = require('node:assert');

const big = Array.from({ length: 200000 }, (_, i) => i);
const rss = () => process.memoryUsage().rss / 1048576;

let kept = null;
const start = rss();
for (let round = 0; round < 40; round++) kept = { ...big };

const grown = rss() - start;
assert.strictEqual(Object.keys(kept).length, 200000);
assert.ok(grown < 250, `memory grew ${Math.round(grown)} MB across 40 spreads`);

console.log('spread memory pressure OK');
