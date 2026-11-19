const assert = require('node:assert');

const N = 2000;

{
  const src = 'a = a + b; a * 2';
  function run(a, b) { const r = eval(src); return [r, a]; }
  for (let i = 0; i < N; i++) assert.deepStrictEqual(run(i, 3), [(i + 3) * 2, i + 3]);
}

{
  const src = 'x * 10 + y';
  function first(x, y, pad) { return eval(src); }
  function second(pad, y, x) { return eval(src); }
  function third(pad) { var more = pad, y = 2, x = 1; return eval(src); }
  for (let i = 0; i < N; i++) {
    assert.strictEqual(first(1, 2, 9), 12);
    assert.strictEqual(second(9, 4, 3), 34);
    assert.strictEqual(third(9), 12);
  }
}

{
  const src = 'target = target + 1';
  function left(target, other) { eval(src); return [target, other]; }
  function right(other, target) { eval(src); return [target, other]; }
  for (let i = 0; i < N; i++) {
    assert.deepStrictEqual(left(1, 50), [2, 50]);
    assert.deepStrictEqual(right(50, 1), [2, 50]);
  }
}

{
  const src = 'late';
  function probe(early) {
    const read = () => { try { return eval(src); } catch (e) { return e.constructor.name; } };
    if (early) return read();
    let late = 7;
    return read();
  }
  for (let i = 0; i < N; i++) {
    assert.strictEqual(probe(false), 7);
    assert.strictEqual(probe(true), 'ReferenceError');
  }
}

{
  const src = 'fixed = fixed + 1';
  function constant(v) { const fixed = v; try { eval(src); } catch (e) { return e.constructor.name + fixed; } return 'none'; }
  function mutable(v) { let fixed = v; eval(src); return fixed; }
  for (let i = 0; i < N; i++) {
    assert.strictEqual(mutable(i), i + 1);
    assert.strictEqual(constant(i), 'TypeError' + i);
  }
}

{
  const src = 'p = p + 1';
  function mapped(p) { eval(src); return [p, arguments[0]]; }
  function unmapped(p) { 'use strict'; eval(src); return [p, arguments[0]]; }
  function defaulted(p = 0) { eval(src); return [p, arguments[0]]; }
  for (let i = 0; i < N; i++) {
    assert.deepStrictEqual(mapped(i), [i + 1, i + 1]);
    assert.deepStrictEqual(unmapped(i), [i + 1, i]);
    assert.deepStrictEqual(defaulted(i), [i + 1, i]);
  }
}

{
  const src = '(function () { n += step; return n; })';
  function counter(n, step) { return eval(src); }
  const counters = [];
  for (let i = 0; i < 50; i++) counters.push(counter(i * 1000, i + 1));
  for (let round = 1; round <= 20; round++) {
    for (let i = 0; i < counters.length; i++) assert.strictEqual(counters[i](), i * 1000 + round * (i + 1));
  }
}

{
  globalThis.sharedName = 100;
  const src = 'sharedName + 1';
  const indirect = eval;
  function direct(sharedName) { return eval(src); }
  const arrow = sharedName => eval(src);
  for (let i = 0; i < N; i++) {
    assert.strictEqual(direct(i), i + 1);
    assert.strictEqual(indirect(src), 101);
    assert.strictEqual(arrow(i * 2), i * 2 + 1);
  }
  delete globalThis.sharedName;
}

{
  const src = 'var grown = seed; grown = grown + inner; grown';
  function declares(seed, inner) { const r = eval(src); return [r, grown]; }
  function shadowed(seed, inner) { let out; { let seed = 5; out = eval(src); } return [out, seed, grown]; }
  for (let i = 0; i < N; i++) {
    assert.deepStrictEqual(declares(i, 2), [i + 2, i + 2]);
    assert.deepStrictEqual(shadowed(i, 2), [7, i, 7]);
  }
}

{
  const src = 'keep = { v: keep.v + extra }; keep.v';
  function churn(keep, extra) { return eval(src); }
  let total = 0;
  const held = [];
  for (let i = 0; i < 60000; i++) {
    total += churn({ v: i }, 1);
    if (i % 1000 === 0) held.push({ i, pad: new Array(64).fill(i) });
  }
  assert.strictEqual(total, (60000 * 59999) / 2 + 60000);
  assert.strictEqual(held.length, 60);
}

{
  const src = 'depth > 0 ? depth + recurse(depth - 1) : 0';
  function recurse(depth) { return eval(src); }
  for (let i = 0; i < 20; i++) assert.strictEqual(recurse(100), 5050);
}

console.log('ok');
