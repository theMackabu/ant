const assert = require('node:assert');

function rep(n, fn) {
  const out = [];
  for (let i = 0; i < n; i++) out.push(fn(i));
  return out;
}

assert.deepStrictEqual(rep(5, i => eval('i * 2')), [0, 2, 4, 6, 8]);

{
  const objs = rep(4, () => eval('({ a: [1, 2], r: /x/g, f: function () {}, c: class {} })'));
  for (let i = 1; i < objs.length; i++) {
    assert.notStrictEqual(objs[i], objs[0]);
    assert.notStrictEqual(objs[i].a, objs[0].a);
    assert.notStrictEqual(objs[i].r, objs[0].r);
    assert.notStrictEqual(objs[i].f, objs[0].f);
    assert.notStrictEqual(objs[i].c, objs[0].c);
  }
  objs[0].a.push(3);
  assert.deepStrictEqual(objs[1].a, [1, 2]);
  objs[0].r.lastIndex = 5;
  assert.strictEqual(objs[1].r.lastIndex, 0);
}

{
  const counters = rep(4, i => eval('var n = i * 10; (function () { return n++ })'));
  assert.deepStrictEqual(counters.map(c => c()), [0, 10, 20, 30]);
  assert.deepStrictEqual(counters.map(c => c()), [1, 11, 21, 31]);
}

{
  const src = 'typeof undefined + ":" + typeof globalThis';
  const plain = () => eval(src);
  const shadowed = (undefined, globalThis) => eval(src);
  for (let i = 0; i < 4; i++) {
    assert.strictEqual(plain(), 'undefined:object');
    assert.strictEqual(shadowed(1, 'g'), 'number:string');
  }
}

{
  const src = 'var declared = 1; typeof declared';
  const sloppy = () => { const r = eval(src); return r + typeof declared };
  const strict = () => { 'use strict'; const r = eval(src); return r + typeof declared };
  for (let i = 0; i < 4; i++) {
    assert.strictEqual(sloppy(), 'numbernumber');
    assert.strictEqual(strict(), 'numberundefined');
  }
}

{
  const src = 'new.target';
  function inFn() { return eval(src) }
  for (let i = 0; i < 4; i++) {
    assert.strictEqual(inFn(), undefined);
    assert.strictEqual(new inFn(), inFn);
    assert.throws(() => (0, eval)(src), SyntaxError);
  }
}

{
  const src = 'let dup = 1; var dup2 = 2; dup + dup2';
  const clash = () => { let dup2 = 0; return eval(src) };
  for (let i = 0; i < 4; i++) {
    assert.strictEqual(eval(src), 3);
    assert.throws(clash, SyntaxError);
  }
}

for (let i = 0; i < 4; i++) {
  assert.throws(() => eval('1 +'), SyntaxError);
  assert.throws(() => eval('missingName'), ReferenceError);
  assert.throws(() => eval('throw new RangeError("r" + i)'), { name: 'RangeError', message: 'r' + i });
}

{
  const tag = s => s;
  const sites = rep(4, () => eval('tag`a${1}b`'));
  for (let i = 1; i < sites.length; i++) assert.notStrictEqual(sites[i], sites[0]);
}

{
  const src = 'this';
  const o = { m() { return eval(src) } };
  for (let i = 0; i < 4; i++) {
    assert.strictEqual(o.m(), o);
    assert.strictEqual((0, eval)(src), globalThis);
  }
}

{
  const rec = n => (n ? eval('rec(n - 1) + 1') : 0);
  assert.strictEqual(rec(50), 50);
}

{
  const src = 'var acc = (typeof acc === "number" ? acc : 0) + 1; acc';
  const run = () => [eval(src), eval(src), eval(src)];
  assert.deepStrictEqual(run(), [1, 2, 3]);
  assert.deepStrictEqual(run(), [1, 2, 3]);
}

{
  let total = 0;
  const src = 'x * x + y';
  for (let x = 0; x < 20000; x++) { const y = x & 3; total += eval(src); }
  let want = 0;
  for (let x = 0; x < 20000; x++) want += x * x + (x & 3);
  assert.strictEqual(total, want);
}

{
  const made = [];
  for (let i = 0; i < 300; i++) made.push(eval('(function (a) { return a + i })'));
  const junk = [];
  for (let i = 0; i < 200000; i++) junk.push({ i });
  assert.strictEqual(made[7](1), 8);
  assert.strictEqual(made[299](1), 300);
}

for (let i = 0; i < 400; i++) assert.strictEqual(eval('i + ' + i), i * 2);

console.log('ok');
