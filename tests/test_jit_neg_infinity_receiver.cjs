// -Infinity's bits are NANBOX_PREFIX itself, so shifting out its top bits
// gives the object tag (kTypeObject is 0). Compiled property reads guarded
// only on that tag decode -Infinity as an object at cage offset 0; that page
// used to be unmapped (SIGBUS) and now reads as zeros, so the guard finds a
// null shape and takes its slow path.
const assert = require('node:assert');

function getX(o) { return o.x; }
function missing(o) { return o.missing; }
function viaMethod(n) { return n.toFixed(1); }
function self(n) { return n.kind(); }
Number.prototype.kind = function () { return typeof this; };

for (let i = 0; i < 3000; i++) {
  getX({ x: i });
  missing({ y: i });
  viaMethod(i);
  self(i);
}

for (const n of [-Infinity, Infinity, NaN, -0]) {
  assert.strictEqual(getX(n), undefined);
  assert.strictEqual(missing(n), undefined);
  assert.strictEqual(viaMethod(n), n.toFixed(1));
  assert.strictEqual(self(n), 'object');
}
delete Number.prototype.kind;

console.log('PASS compiled property reads treat -Infinity as a number');
