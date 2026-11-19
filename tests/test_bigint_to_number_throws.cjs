const assert = require('node:assert');

for (const op of [
  () => 'abc'.charAt(1n), () => 'abc'.charCodeAt(1n), () => 'abc'.codePointAt(1n), () => 'abc'.substr(1n), () => 'abc'.startsWith('b', 1n),
  () => [1, 2].includes(2, 1n), () => String.fromCharCode(65n), () => new Date(1n),
  () => isNaN(1n), () => Math.max(1n), () => +1n,
  () => 'abc'.charAt({ valueOf() { return 1n; } }), () => +{ valueOf() { return 1n; } },
]) assert.throws(op, TypeError, String(op));
assert.strictEqual(Number(5n), 5);
assert.strictEqual(new Number(5n).valueOf(), 5);
assert.strictEqual(Number({ valueOf() { return 7n; } }), 7);
assert.strictEqual(5n == 5, true);
assert.strictEqual(5n > 1, true);
assert.strictEqual(new Intl.NumberFormat('en').format(1234n), '1,234');

console.log('ok');
