const assert = require('node:assert');

function churn(rounds) {
  let kept = null;
  for (let i = 0; i < rounds; i++) kept = { i, pad: [i, i + 1, i + 2] };
  return kept;
}

{
  const closure = '(() => typeof later + ":" + typeof other)', declare = 'var later = 1', plain = '1 + 1';
  function host() {
    eval(plain);
    const read = eval(closure);
    eval(plain);
    eval(declare);
    return read() + ":" + eval(plain);
  }
  for (let i = 0; i < 6; i++) assert.strictEqual(host(), 'number:undefined:2');
}

{
  const declare = 'var made = n * 2', read = 'made + n', fn = 'function hoisted() { return n + 1 }', call = 'hoisted()';
  function host(n) {
    const before = eval('typeof made + typeof hoisted');
    eval(declare);
    eval(fn);
    return [before, eval(read), eval(call), typeof made, eval('delete made'), eval('typeof made')];
  }
  for (let i = 0; i < 6; i++)
    assert.deepStrictEqual(host(i), ['undefinedundefined', i * 3, i + 1, 'number', true, 'undefined']);
  assert.strictEqual(typeof made, 'undefined');
  assert.strictEqual(typeof hoisted, 'undefined');
}

{
  const nested = 'eval(inner)', inner = 'var deep = base + 1; deep', after = 'typeof deep';
  function host(base) { return [eval(after), eval(nested), eval(after)]; }
  for (let i = 0; i < 6; i++) assert.deepStrictEqual(host(i), ['undefined', i + 1, 'number']);
}

{
  const count = 'arguments.length', first = 'arguments[0]', plain = 'a';
  function sloppy(a) { return [eval(plain), eval(count), eval(first), eval(plain)]; }
  function strict(a) { 'use strict'; return [eval(plain), eval(count), eval(first), eval(plain)]; }
  for (let i = 0; i < 6; i++) {
    assert.deepStrictEqual(sloppy(i, 'x'), [i, 2, i, i]);
    assert.deepStrictEqual(strict(i, 'x', 'y'), [i, 3, i, i]);
  }
}

{
  const src = 'depth ? rec(depth - 1, tag + 1) + ":" + tag + ":" + depth : "end"';
  function rec(depth, tag) { return eval(src); }
  for (let i = 0; i < 6; i++) assert.strictEqual(rec(3, i), `end:${i + 2}:1:${i + 1}:2:${i}:3`);
}

{
  const outer = 'inner(a + 1) + a', leaf = 'b * 2';
  function inner(b) { return eval(leaf); }
  function host(a) { const before = eval('a'); const sum = eval(outer); return [before, sum, eval('a')]; }
  for (let i = 0; i < 6; i++) assert.deepStrictEqual(host(i), [i, (i + 1) * 2 + i, i]);
}

{
  const read = 'a + b', busy = 'churn(n).i + a + b';
  function host(a, b, n) {
    const first = eval(read);
    churn(n);
    const second = eval(read);
    const third = eval(busy);
    return [first, second, third, eval(read)];
  }
  for (let round = 0; round < 12; round++) {
    const n = round % 3 === 0 ? 60000 : 2000;
    assert.deepStrictEqual(host(round, 1, n), [round + 1, round + 1, n - 1 + round + 1, round + 1]);
  }
}

{
  const small = 'a', wide = 'v0 + v9 + v19 + v24', bump = 'v24 = v24 + 1; v0 = v0 + 1; v24';
  function few(a) { return eval(small); }
  function many(seed) {
    let v0 = seed, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8, v9 = seed + 9;
    let v10 = 10, v11 = 11, v12 = 12, v13 = 13, v14 = 14, v15 = 15, v16 = 16, v17 = 17, v18 = 18, v19 = seed + 19;
    let v20 = 20, v21 = 21, v22 = 22, v23 = 23, v24 = seed + 24;
    const sum = eval(wide), bumped = eval(bump);
    return [sum, bumped, v0, v24, v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v20 + v21 + v22 + v23];
  }
  for (let i = 0; i < 8; i++) {
    assert.strictEqual(few(i), i);
    assert.deepStrictEqual(many(i), [4 * i + 52, i + 25, i + 1, i + 25, 248]);
    assert.strictEqual(few(i + 1), i + 1);
  }
}

{
  const get = 'a', set = 'a = a + 10', early = 'late', lateSet = 'late = late + 1', fixed = 'frozen = 2';
  function mapped(a) {
    const out = [eval(get), eval(set), arguments[0]];
    arguments[0] = 'indexed';
    out.push(eval(get));
    a = 'direct';
    out.push(eval(get), arguments[0]);
    return out;
  }
  function tdz(when) {
    if (when === 'early') return eval(early);
    let late = 1;
    eval(lateSet);
    return eval(early) + late;
  }
  function constant() {
    const frozen = 1;
    try { eval(fixed); } catch (error) { return [error instanceof TypeError, frozen, eval('frozen')]; }
    return 'stored';
  }
  for (let i = 0; i < 6; i++) {
    assert.deepStrictEqual(mapped(i), [i, i + 10, i + 10, 'indexed', 'direct', 'direct']);
    assert.strictEqual(tdz('late'), 4);
    assert.throws(() => tdz('early'), ReferenceError);
    assert.deepStrictEqual(constant(), [true, 1, 1]);
  }
}

{
  const kinds = 'typeof undefined + ":" + typeof globalThis';
  const plain = () => eval(kinds);
  const param = (undefined, globalThis) => eval(kinds);
  const local = () => { let undefined = 1; { let globalThis = 'g'; return eval(kinds); } };
  const enclosing = (undefined) => (() => function () { return eval(kinds); })()();
  const escaped = () => eval('typeof \\u0075ndefined');
  const escapedParam = (undefined) => eval('typeof \\u0075ndefined');
  for (let i = 0; i < 6; i++) {
    assert.strictEqual(plain(), 'undefined:object');
    assert.strictEqual(param(1, 'g'), 'number:string');
    assert.strictEqual(local(), 'number:string');
    assert.strictEqual(enclosing('s'), 'string:object');
    assert.strictEqual(escaped(), 'undefined');
    assert.strictEqual(escapedParam(1), 'number');
  }
}

{
  const first = '1', declare = 'var w = base + 3', read = 'w + base', sloppyWrite = 'leafGlobal = base';
  function host(base) {
    const a = eval(first);
    eval(declare);
    const b = eval(read);
    eval(sloppyWrite);
    return [a, b, w, eval('typeof w')];
  }
  for (let i = 0; i < 6; i++) {
    assert.deepStrictEqual(host(i), [1, 2 * i + 3, i + 3, 'number']);
    assert.strictEqual(globalThis.leafGlobal, i);
  }
  delete globalThis.leafGlobal;
}

{
  const read = 'ev + own';
  function make(v) { eval('var ev = v'); return function (own) { return eval(read); }; }
  const a = make(1), b = make(20), c = make(300);
  for (let i = 0; i < 6; i++) assert.deepStrictEqual([a(i), b(i), c(i), a(i)], [1 + i, 20 + i, 300 + i, 1 + i]);
}

{
  const src = 'fail(a)', ok = 'a + 1';
  function fail(value) { throw new RangeError('boom' + value); }
  function host(a) {
    let caught = null;
    try { eval(src); } catch (error) { caught = error.message; }
    return [caught, eval(ok)];
  }
  for (let i = 0; i < 6; i++) assert.deepStrictEqual(host(i), ['boom' + i, i + 1]);
}

{
  const src = 'a + q', inBlock = 'q * a';
  function blocks(a) {
    let total = 0;
    { let q = 1; total += eval(src); }
    { let q = 10; total += eval(src); { let q = 100; total += eval(inBlock); } }
    for (let q = 0; q < 2; q++) total += eval(inBlock);
    return total;
  }
  for (let i = 0; i < 6; i++) assert.strictEqual(blocks(i), 2 * i + 11 + 100 * i + i);
}

{
  let body = 'var r = 0;';
  for (let i = 0; i < 260; i++) body += 'var v' + i + ' = ' + i + ';';
  for (let i = 0; i < 260; i++) body += '{ let b = ' + i + '; r += eval(blocked); }';
  body += '{ let q = 1000; r += eval(mixed); { let w = 5; r += eval(mixed) + w; kinds.push(eval(kind)); } } return r;';
  const kinds = [];
  const wide = new Function('blocked', 'mixed', 'kind', 'kinds', 'undefined', body);
  for (let i = 0; i < 4; i++) assert.strictEqual(wide('v1 + b', 'v259 + q', 'typeof undefined', kinds, i), 36453);
  assert.deepStrictEqual(kinds, ['number', 'number', 'number', 'number']);
}

{
  const leaf = 'a + 1', capture = '(() => kept)';
  function reads(a) { return eval(leaf); }
  function makes(kept) { return eval(capture); }
  for (let round = 0; round < 6; round++) {
    const made = [];
    reads(round);
    for (let i = 0; i < 30000; i++) made.push(makes(i));
    let sum = 0;
    for (let i = 0; i < 400; i++) sum += reads(i);
    assert.strictEqual(sum, 80200);
    for (let i = 0; i < made.length; i++) if (made[i]() !== i) assert.fail('closure ' + i + ' reads ' + made[i]());
  }
}

{
  const src = 'seen = a + b; churn(n); a = { n: a }; b = [b]; churn(n); a.n + b[0] + seen';
  function host(a, b, n) { let seen = 0; const out = eval(src); return [out, a.n, b[0], seen]; }
  for (let round = 0; round < 16; round++) {
    const n = round % 4 === 0 ? 50000 : 3000;
    assert.deepStrictEqual(host(round, 5, n), [2 * (round + 5), round, 5, round + 5]);
  }
}

{
  const src = 'a + 1', strictVar = '"use strict"; var z = a; z + 1', reflect = 'new.target === undefined ? "call" : "new"';
  function* gen(a) { yield eval(src); a += 10; yield eval(src); }
  async function later(a) { await null; return eval(src); }
  const arrow = (a) => eval(src);
  const object = { method(a) { return eval(src); }, get value() { const a = 5; return eval(src); } };
  class Base { constructor(a) { this.kind = eval(reflect); this.next = eval(src); } }
  class Derived extends Base { constructor(a) { super(a + 1); this.own = eval(src); } }
  function plain(a) { return eval(reflect); }
  function scoped(a) { const r = eval(strictVar); return [r, typeof z]; }
  const pending = [];
  for (let i = 0; i < 6; i++) {
    assert.deepStrictEqual([...gen(i)], [i + 1, i + 11]);
    pending.push(later(i).then((value) => assert.strictEqual(value, i + 1)));
    assert.strictEqual(arrow(i), i + 1);
    assert.strictEqual(object.method(i), i + 1);
    assert.strictEqual(object.value, 6);
    const derived = new Derived(i);
    assert.deepStrictEqual([derived.kind, derived.next, derived.own], ['new', i + 2, i + 1]);
    assert.deepStrictEqual([plain(i), new plain(i) instanceof plain], ['call', true]);
    assert.deepStrictEqual(scoped(i), [i + 1, 'undefined']);
  }
  Promise.all(pending).then(() => console.log('ok'), (error) => { console.error(error); process.exitCode = 1; });
}

{
  const src = 'holder.value = { n: a }; a', read = 'holder.value.n + a';
  const holder = { value: null };
  function host(a) { eval(src); churn(3000); return eval(read); }
  for (let i = 0; i < 40; i++) assert.strictEqual(host(i), 2 * i);
}

{
  const set = 'kept = make(a)', read = 'kept.n';
  function make(n) { return { n, pad: churn(200) }; }
  function host(a) { let kept = null; eval(set); churn(4000); eval(set); churn(4000); return eval(read); }
  for (let i = 0; i < 40; i++) assert.strictEqual(host(i), i);
}

{
  const src = 'delete a', del = 'delete gone', declare = 'var gone = 1';
  function host(a) { eval(declare); return [eval(src), a, eval(del), typeof gone]; }
  for (let i = 0; i < 6; i++) assert.deepStrictEqual(host(i), [false, i, true, 'undefined']);
}
