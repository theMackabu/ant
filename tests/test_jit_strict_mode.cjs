// Compiled code has no VM frame of its own, so failed stores and deletes must
// take their strictness from the compiled (or inlined) function, not from the
// interpreted frame below it: strict code throws, sloppy code stays silent,
// whichever mode its caller is in.
const assert = require('node:assert');

const N = 3000;
const readOnly = () => { const o = {}; Object.defineProperty(o, 'y', { value: 0, writable: false }); return o; };
const frozen = () => Object.freeze({ y: 1, 0: 1 });
const getterOnly = () => ({ get y() { return 1; } });
const nonExtensible = () => Object.preventExtensions({});

function throws(f, make) {
  let n = 0;
  for (let i = 0; i < N; i++) { try { f(make()); } catch (e) { assert.ok(e instanceof TypeError, e); n++; } }
  return n;
}

const strict = body => new Function('o', "'use strict'; " + body);
const sloppy = body => new Function('o', body);
const cases = [
  ['o.y = 2;', readOnly], ['o.y = 2;', frozen], ['o.y = 2;', getterOnly],
  ['o[0] = 2;', frozen], ['var k = "y"; o[k] = 2;', readOnly],
  ['o.z = 2;', nonExtensible], ['delete o.y;', frozen],
];
for (const [body, make] of cases) {
  assert.strictEqual(throws(strict(body), make), N, 'strict: ' + body);
  assert.strictEqual(throws(sloppy(body), make), 0, 'sloppy: ' + body);
}

// sloppy compiled code called from strict code
(function () {
  'use strict';
  for (const [body, make] of cases) assert.strictEqual(throws(sloppy(body), make), 0, 'sloppy from strict: ' + body);
})();

// small callees that the caller may inline, in the other mode from the caller
function sloppyStore(o) { o.y = 2; }
const strictStore = strict('o.y = 2;');
(function () {
  'use strict';
  let n = 0;
  for (let i = 0; i < N; i++) { try { sloppyStore(readOnly()); } catch (e) { assert.ok(e instanceof TypeError, e); n++; } }
  assert.strictEqual(n, 0);
})();
{
  let n = 0;
  for (let i = 0; i < N; i++) { try { strictStore(readOnly()); } catch (e) { assert.ok(e instanceof TypeError, e); n++; } }
  assert.strictEqual(n, N);
}

// a sloppy setter reached from a strict store keeps its own mode
{
  const target = readOnly();
  const o = { set y(v) { target.y = v; } };
  assert.strictEqual(throws(strict('o.y = 2;'), () => o), 0);
}

console.log('PASS compiled stores and deletes follow their own function\'s strictness');
