const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

if (process.argv[2] === 'count') {
  function w(o, s) { with (o) { return eval(s); } }
  let total = 0;
  for (let i = 0; i < 60; i++) total += w({ a: i }, 'a + 1');
  class P { #x = 3; m(s) { return eval(s); } }
  const p = new P();
  for (let i = 0; i < 60; i++) total += p.m('this.#x');
  console.log(total);
  return;
}

function one(o, s) { with (o) { return eval(s); } }
function two(o, p, s) { with (o) { with (p) { return eval(s); } } }
function sites(o, s, second) {
  if (second) { with ({ v: 'second' }) { return eval(s); } }
  with (o) { return eval(s); }
}
function shadowed(s, declare) {
  with ({ x: 'outer with' }) {
    return (function () {
      if (declare) eval('var x = "eval var"');
      return eval(s);
    })();
  }
}
function plain(v, s) { return eval(s); }
function nested(o, s) { with (o) { return (() => eval(s))(); } }
function writes(o, s) { with (o) { eval(s); } return o; }
function declares(o, s) { with (o) { eval(s); } return typeof fresh; }

var v = 'global';
for (let round = 0; round < 6; round++) {
  assert.strictEqual(one({ v: round }, 'v'), round);
  assert.strictEqual(one({}, 'v'), 'global');
  assert.strictEqual(plain('param', 'v'), 'param');
  assert.strictEqual(two({ v: 'a' }, { v: 'b' }, 'v'), 'b');
  assert.strictEqual(two({ v: 'a' }, {}, 'v'), 'a');
  assert.strictEqual(sites({ v: 'first' }, 'v', false), 'first');
  assert.strictEqual(sites({ v: 'first' }, 'v', true), 'second');
  assert.strictEqual(shadowed('x', false), 'outer with');
  assert.strictEqual(shadowed('x', true), 'eval var');
  assert.strictEqual(shadowed('x', false), 'outer with');
  assert.strictEqual(nested({ v: 'arrow' }, 'v'), 'arrow');
  assert.strictEqual(nested({}, 'typeof v'), 'string');
  assert.strictEqual(writes({ v: 0 }, 'v = 7').v, 7);
  assert.strictEqual(declares({}, 'var fresh = 1'), 'number');
  assert.strictEqual(one({ eval: (s) => s + '!' }, 'v'), 'v!');
  assert.strictEqual(one({ [Symbol.unscopables]: { v: true }, v: 'hidden' }, 'v'), 'global');
}

class A { #x = 'a'; get(s) { return eval(s); } }
function classUnderWith(o) { with (o) { return class { #x = 'w'; get(s) { return eval(s); } }; } }
class B { #x() { return 'b'; } get(s) { return eval(s); } }
for (let round = 0; round < 6; round++) {
  assert.strictEqual(new A().get('this.#x'), 'a');
  assert.strictEqual(new B().get('this.#x()'), 'b');
  assert.strictEqual(new B().get('typeof this.#x'), 'function');
  assert.strictEqual(new A().get('typeof this.#x'), 'string');
  assert.strictEqual(new (classUnderWith({ k: round }))().get('this.#x + k'), 'w' + round);
  assert.throws(() => plain(0, 'this.#x'), SyntaxError);
}

{
  const sources = [];
  for (let i = 0; i < 40; i++) sources.push(`with ({ w${i}: ${i} }) { return eval(s) }`);
  let churn = null;
  for (let round = 0; round < 4; round++) {
    for (let i = 0; i < sources.length; i++) {
      const f = new Function('s', sources[i]);
      assert.strictEqual(f(`w${i}`), i);
      assert.strictEqual(f(`w${i}`), i);
      assert.strictEqual(f(`typeof w${(i + 1) % 40}`), 'undefined');
      for (let k = 0; k < 2000; k++) churn = { k, list: [k, round, i], text: 'x' + k };
    }
  }
  assert.ok(churn);
}

if (typeof Ant !== 'undefined') {
  const child = spawnSync(process.execPath, [__filename, 'count'], { encoding: 'utf8', env: { ...process.env, ANT_DEBUG: 'dump/compile:trace' }, maxBuffer: 16 * 1024 * 1024 });
  assert.strictEqual(child.status, 0, child.stderr);
  assert.strictEqual(child.stdout.trim(), String(60 * 61 / 2 + 60 * 3));
  const compiles = child.stderr.split('\n').filter((line) => /\[compile\] start kind=program mode=[14]/.test(line)).length;
  assert.ok(compiles <= 4, `direct evals under with and with private names compiled ${compiles} times`);
}

console.log('ok');
