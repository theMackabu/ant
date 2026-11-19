const assert = require('node:assert');

const run = (source) => new Function(source)();

for (const source of [
  'undefined = 1', 'NaN = 1', 'Infinity = 1', 'undefined += 1', 'NaN++', '--Infinity', '[undefined] = [1]',
  '({ a: NaN } = { a: 1 })', 'undefined ??= 1', 'undefined ||= 1', 'for (undefined of [1]);', 'for (NaN in { a: 1 });',
  'globalThis.undefined = 1', 'NaN ||= 1',
]) assert.throws(() => run(`"use strict"; ${source}`), TypeError, source);
assert.strictEqual(run('"use strict"; NaN &&= 1; undefined &&= 1; return 2'), 2);
assert.strictEqual(run('undefined = 1; return undefined'), undefined);
assert.strictEqual(run('undefined += 1; undefined ??= 2; undefined++; return undefined'), undefined);
assert.ok(Number.isNaN(run('NaN = 1; return NaN')));
assert.ok(Number.isNaN(run('return (undefined += 1)')));
assert.strictEqual(run('return (undefined ??= 4)'), 4);
assert.strictEqual(run('var undefined = 5; return undefined'), 5);
assert.strictEqual(run('"use strict"; { let undefined = 5; return undefined }'), 5);
assert.strictEqual(run('"use strict"; return (function (undefined) { undefined = 3; return undefined })()'), 3);
assert.strictEqual(run('"use strict"; return ((NaN) => { NaN = 3; return NaN })()'), 3);
assert.strictEqual(run('"use strict"; return (function (undefined) { undefined += 3; undefined ||= 9; return undefined })(1)'), 4);

console.log('ok');
