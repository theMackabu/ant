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

const M = 600;

{
  globalThis.probeA = 'global';
  const read = 'probeA', decl = 'var probeA = "local"';
  function plain(shadow) { if (shadow) eval(decl); return eval(read); }
  function busy(shadow) { eval('var other = 1'); if (shadow) eval(decl); return eval(read); }
  function own(shadow) { if (shadow) eval(decl); eval('1'); return probeA; }
  for (let i = 0; i < M; i++) {
    const shadow = i % 3 === 1;
    assert.strictEqual(plain(shadow), shadow ? 'local' : 'global');
    assert.strictEqual(busy(shadow), shadow ? 'local' : 'global');
    assert.strictEqual(own(shadow), shadow ? 'local' : 'global');
  }
  delete globalThis.probeA;
}

{
  globalThis.probeB = 'global';
  const read = 'probeB';
  function churn(step) {
    eval('var gone = 1');
    const before = eval(read);
    eval('delete gone');
    const mid = eval(read);
    if (step & 1) eval('var probeB = "local"');
    const after = eval(read);
    eval('var late = 2');
    return [before, mid, after, eval(read), typeof gone];
  }
  for (let i = 0; i < M; i++) {
    const local = i & 1 ? 'local' : 'global';
    assert.deepStrictEqual(churn(i), ['global', 'global', local, local, 'undefined']);
  }
  delete globalThis.probeB;
}

{
  const read = 'typeof probeC === "undefined" ? "missing" : probeC';
  function peek() { return eval(read); }
  for (let i = 0; i < M; i++) {
    const phase = i % 4;
    if (phase === 0) delete globalThis.probeC;
    if (phase === 1) globalThis.probeC = 'data' + i;
    if (phase === 2) Object.defineProperty(globalThis, 'probeC', { get() { return 'getter' + i; }, configurable: true });
    if (phase === 3) Object.defineProperty(globalThis, 'probeC', { value: 'frozen' + i, writable: false, configurable: true });
    assert.strictEqual(peek(), ['missing', 'data' + i, 'getter' + i, 'frozen' + i][phase]);
  }
  delete globalThis.probeC;
}

{
  const src = 'var total = seed; total += step; total *= 2; total';
  function sum(seed, step) { const r = eval(src); return [r, total]; }
  for (let i = 0; i < M; i++) assert.deepStrictEqual(sum(i, 3), [(i + 3) * 2, (i + 3) * 2]);
}

{
  const decl = 'var held = start', bump = 'held += 1; held';
  function make(start) { eval(decl); return () => eval(bump); }
  const a = make(0), b = make(100);
  for (let i = 1; i <= M; i++) {
    assert.strictEqual(a(), i);
    assert.strictEqual(b(), 100 + i);
  }
}

{
  globalThis.probeD = 1;
  const src = '(function () { return probeD + local; })';
  function outer(local, shadow) { if (shadow) eval('var probeD = 1000'); return eval(src); }
  const fns = [];
  for (let i = 0; i < 40; i++) fns.push(outer(i, i % 2 === 1));
  for (let round = 0; round < 20; round++) {
    globalThis.probeD = round;
    for (let i = 0; i < fns.length; i++) assert.strictEqual(fns[i](), (i % 2 === 1 ? 1000 : round) + i);
  }
  delete globalThis.probeD;
}

{
  const src = 'Math.max(a, b) + parseInt("7") + [a, b].length';
  function builtins(a, b) { return eval(src); }
  function shadowed(a, b) { var Math = { max: () => -1 }; return eval(src); }
  function declared(a, b) { eval('var parseInt = function () { return 100; }'); return eval(src); }
  for (let i = 0; i < M; i++) {
    assert.strictEqual(builtins(i, 2), Math.max(i, 2) + 9);
    assert.strictEqual(shadowed(i, 2), 8);
    assert.strictEqual(declared(i, 2), Math.max(i, 2) + 102);
  }
}

{
  const src = 'eval("inner + outerName")';
  globalThis.outerName = 5;
  function nested(inner) { return eval(src); }
  function nestedShadow(inner) { var outerName = 50; return eval(src); }
  for (let i = 0; i < M; i++) {
    assert.strictEqual(nested(i), i + 5);
    assert.strictEqual(nestedShadow(i), i + 50);
  }
  delete globalThis.outerName;
}

{
  const src = 'tally = tally + 1';
  globalThis.tally = 0;
  function bumpGlobal() { eval(src); }
  function bumpLocal() { var tally = 10; eval(src); return tally; }
  function bumpDeclared() { eval('var tally = 20'); eval(src); return tally; }
  for (let i = 0; i < M; i++) {
    bumpGlobal();
    assert.strictEqual(bumpLocal(), 11);
    assert.strictEqual(bumpDeclared(), 21);
  }
  assert.strictEqual(globalThis.tally, M);
  delete globalThis.tally;
}

{
  const src = 'arguments.length + (typeof arguments)';
  function viaEval(a, b) { eval('1'); return eval(src); }
  function declaredFirst() { eval('var arguments = { length: 40 }'); return eval(src); }
  function own(a) { eval('1'); return arguments.length + eval(src); }
  for (let i = 0; i < M; i++) {
    assert.strictEqual(viaEval(1, 2, 3), '3object');
    assert.strictEqual(declaredFirst(1), '40object');
    assert.strictEqual(own(1, 2), '22object');
  }
}

{
  globalThis.probeE = 'global';
  const src = 'probeE';
  function free() { return eval(src); }
  function local(probeE) { return eval(src); }
  function inner() { var probeE = 'var'; return eval(src); }
  for (let i = 0; i < M; i++) {
    assert.strictEqual(free(), 'global');
    assert.strictEqual(local('param'), 'param');
    assert.strictEqual(inner(), 'var');
  }
  delete globalThis.probeE;
}

let moduleLocal = 'module';
{
  globalThis.moduleLocal = 'global';
  globalThis.probeF = 'global';
  const src = 'moduleLocal + ":" + probeF';
  function viaModule() { return eval(src); }
  function viaOuter() { return (function () { return eval(src); })(); }
  for (let i = 0; i < M; i++) {
    moduleLocal = 'module' + (i & 3);
    assert.strictEqual(viaModule(), 'module' + (i & 3) + ':global');
    assert.strictEqual(viaOuter(), 'module' + (i & 3) + ':global');
  }
  delete globalThis.moduleLocal;
  delete globalThis.probeF;
}

{
  globalThis.probeG = 'global';
  const read = 'probeG';
  function outer(tag) {
    eval('var outerVar = tag');
    return function inner(step) {
      eval('var first = 1');
      const a = eval(read);
      if (step & 1) eval('var probeG = "inner"');
      const b = eval(read);
      eval('var second = 2');
      if (step & 2) eval('var probeG = "late"');
      return [a, b, eval(read), outerVar];
    };
  }
  const one = outer('one'), two = outer('two');
  for (let i = 0; i < M; i++) {
    const fn = i & 4 ? one : two, tag = i & 4 ? 'one' : 'two';
    const mid = i & 1 ? 'inner' : 'global';
    const last = i & 2 ? 'late' : mid;
    assert.deepStrictEqual(fn(i), ['global', mid, last, tag]);
  }
  delete globalThis.probeG;
}

{
  globalThis.probeH = 'global';
  const read = 'probeH';
  function outer(shadow) {
    if (shadow) eval('var probeH = "outer"');
    return () => eval(read);
  }
  const plain = outer(false), shadowed = outer(true);
  for (let i = 0; i < M; i++) {
    assert.strictEqual(plain(), 'global');
    assert.strictEqual(shadowed(), 'outer');
  }
  delete globalThis.probeH;
}

{
  const src = 'var own = base; own += 1; own';
  function strictHost(base) { 'use strict'; return eval(src); }
  function sloppyHost(base) { const r = eval(src); return r + own; }
  for (let i = 0; i < M; i++) {
    assert.strictEqual(strictHost(i), i + 1);
    assert.strictEqual(sloppyHost(i), (i + 1) * 2);
  }
}

{
  globalThis.probeI = 'global';
  const read = 'probeI';
  function pick(mode) {
    if (mode === 1) eval('var unrelated = 1');
    if (mode === 2) eval('var probeI = "local"');
    return eval(read);
  }
  for (let i = 0; i < M; i++) {
    const mode = i % 3;
    assert.strictEqual(pick(mode), mode === 2 ? 'local' : 'global');
  }
  delete globalThis.probeI;
}

{
  globalThis.probeJ = 'global';
  const read = 'probeJ';
  const names = [];
  for (let i = 0; i < 1100; i++) names.push('wide' + i);
  const wide = 'var ' + names.join(', ');
  function crowded(shadow) {
    eval(wide);
    const seen = [];
    for (let pass = 0; pass < 2; pass++) {
      seen.push(eval(read));
      if (shadow && pass === 0) eval('var probeJ = "local"');
    }
    return seen;
  }
  for (let i = 0; i < 12; i++) assert.deepStrictEqual(crowded(i & 1), ['global', i & 1 ? 'local' : 'global']);
  delete globalThis.probeJ;
}

{
  globalThis.probeK = 'global';
  const innerSrc = 'probeK', outerSrc = 'eval(innerSrc)';
  function shadowing(probeK) { return eval(outerSrc); }
  function open() { return eval(outerSrc); }
  for (let i = 0; i < M; i++) {
    assert.strictEqual(shadowing('param' + i), 'param' + i);
    assert.strictEqual(shadowing('again' + i), 'again' + i);
    assert.strictEqual(open(), 'global');
  }
  delete globalThis.probeK;
}

{
  globalThis.probeL = 0;
  const src = 'probeL = probeL + 1';
  function write() { eval(src); }
  function frozen() { 'use strict'; try { eval(src); } catch (e) { return e.constructor.name; } return 'none'; }
  for (let i = 0; i < M; i++) write();
  assert.strictEqual(globalThis.probeL, M);
  Object.defineProperty(globalThis, 'probeL', { value: -1, writable: false, configurable: true });
  for (let i = 0; i < 20; i++) { write(); assert.strictEqual(frozen(), 'TypeError'); }
  assert.strictEqual(globalThis.probeL, -1);
  let seen = 0;
  Object.defineProperty(globalThis, 'probeL', { get() { return 5; }, set(v) { seen = v; }, configurable: true });
  for (let i = 0; i < 20; i++) write();
  assert.strictEqual(seen, 6);
  delete globalThis.probeL;
  for (let i = 0; i < 20; i++) assert.strictEqual(frozen(), 'ReferenceError');
}

{
  const innerSrc = 'layered', outerSrc = 'eval(innerSrc)';
  function layered(tag) {
    eval('var layered = "var"');
    { let layered = 'let' + tag; const seen = []; for (let pass = 0; pass < 2; pass++) seen.push(eval(outerSrc)); return seen; }
  }
  for (let i = 0; i < M; i++) assert.deepStrictEqual(layered(i), ['let' + i, 'let' + i]);
}


{
  const out = [];
  const s1 = '1', s2 = 'arguments', s3 = '(function (v) { a = v; })', s4 = 'typeof arguments', s5 = 'var arguments; typeof arguments';
  const s6 = 'eval("argu" + "ments")', s7 = '(() => arguments)', s8 = 'arguments[0] = 7; a', s9 = 'delete arguments', s10 = 'a = 3';
  function f1(a) { eval(s1); const r = eval(s2); return [r.length, r[0], r === arguments]; }
  out.push(f1(5, 6));
  function f2(a) { const w = eval(s3); const r = eval(s2); w(9); return [r[0], a]; }
  out.push(f2(1));
  function f3(a) { eval(s1); a = 4; return eval(s2)[0]; }
  out.push(f3(1));
  function f5(a) { return eval(s4); }
  out.push(f5(1));
  function f6(a) { return eval(s5); }
  out.push(f6(1));
  function f7(a) { return eval(s6)[0]; }
  out.push(f7(8));
  function f8(a) { eval(s1); return eval(s7)()[0]; }
  out.push(f8(2));
  function f9(a) { return eval(s8); }
  out.push(f9(1));
  function f10(a) { eval(s1); const g = eval(s7); return g; }
  out.push(f10(3, 4)().length);
  function f11(a) { eval(s10); return eval(s2)[0]; }
  out.push(f11(1));
  function f12(a = 1) { eval(s1); a = 4; return eval(s2)[0]; }
  out.push(f12(2));
  function f13(a) { eval(s1); return [arguments.length, eval(s2) === arguments]; }
  out.push(f13(1, 2, 3));
  const f14 = (a) => { try { return eval(s4); } catch (e) { return e.constructor.name; } };
  out.push(f14(1));
  function f15(a) { const g = () => eval(s2); eval(s1); return g()[0]; }
  out.push(f15(6));
  function f16(a) { eval(s1); const r1 = eval(s2); const r2 = eval(s2); return r1 === r2; }
  out.push(f16(1));
  function f17(a) { eval(s10); const r = eval(s2); eval(s10.replace('3', '5')); return [r[0], a]; }
  out.push(f17(1));
  assert.deepStrictEqual(out, [[2, 5, true], [9, 9], 4, 'object', 'object', 8, 2, 7, 2, 3, 2, [3, true], 'object', 6, true, [5, 5]]);
}

{
  const out = [];
  const t = (f) => { try { out.push(f()); } catch (e) { out.push(e.constructor.name); } };
  const s = (x) => x;
  t(() => (function (a) { return eval(s('delete arguments')); })(1));
  t(() => (function (a) { eval(s('delete arguments')); return typeof eval(s('arguments')); })(1));
  t(() => (function (a) { eval(s('arguments = 5')); return eval(s('arguments')); })(1));
  t(() => (function (a) { eval(s('arguments = 5')); return typeof arguments; })(1));
  t(() => (function (a) { eval(s('function arguments() { return 9; }')); return eval(s('typeof arguments + arguments()')); })(1));
  t(() => (function (a) { eval(s('var arguments = 3')); return eval(s('arguments')); })(1));
  t(() => (function (a) { eval(s('var arguments')); return eval(s('typeof arguments')); })(1));
  t(() => (function (a) { 'use strict'; return eval(s('arguments.length + typeof arguments')); })(1, 2));
  t(() => (function (a) { 'use strict'; return eval(s('(() => arguments[1])'))(); })(1, 2));
  t(() => (function (a) { return eval(s('(function () { return arguments.length; })'))(1, 2, 3); })(1));
  t(() => (function (a) { const g = () => eval(s('arguments[0]')); return g(); })(7));
  t(() => (function (a) { return (() => (() => eval(s('arguments.length')))())(); })(7, 8));
  t(() => (function (a) { eval(s('var x = 1')); return Object.keys(eval(s('arguments'))).join(); })(7, 8));
  t(() => (function (arguments) { return eval(s('arguments')); })(4));
  t(() => (function () { let arguments = 6; return eval(s('arguments')); })(4));
  t(() => (function (a) { eval(s('let arguments = 1; arguments')); return eval(s('typeof arguments')); })(4));
  t(() => (function (a) { return eval(s('typeof arguments + (function () { return typeof arguments; })()')); })(4));
  t(() => (function (a) { return eval(s('eval("eval(\\"arguments[0]\\")")')); })(11));
  t(() => (function (a, b) { return eval(s('a = 9; arguments[0] + "," + (arguments[1] = 4, b)')); })(1, 2));
  t(() => (function (a) { const r = eval(s('arguments')); eval(s('a = 2')); return r[0]; })(1));
  t(() => { function f(a) { return eval(s('f.arguments === arguments')); } return f(1); });
  t(() => (function (a) { return eval(s('"use strict"; arguments[0]')); })(5));
  t(() => (function (a) { return new (eval(s('(function C() { this.n = arguments.length; })')))(1, 2).n; })(5));
  t(() => (function* (a) { yield eval(s('arguments[0]')); })(12).next().value);
  assert.deepStrictEqual(out, [
    false, 'object', 5, 'number', 'function9', 3, 'object', '2object', 2, 3, 7, 2, '0,1', 4, 6,
    'object', 'objectobject', 11, '9,4', 2, false, 5, 2, 12,
  ]);
}

{
  globalThis.reuseRead = 'global';
  globalThis.reuseWrite = 'global';
  const names = [];
  for (let i = 0; i < 1024; i++) names.push('reuse' + i);
  const wide = 'var ' + names.join(', ');
  const plain = wide + ', pad', shadowRead = wide + ', reuseRead = "local"', shadowWrite = wide + ', reuseWrite = "local"';
  const back = 'reuseWrite', churnSrc = 'for (let i = 0; i < 35000; i++) kept = { i }';
  function churn() { let kept = null; eval(churnSrc); return kept; }
  function scrub(flag) { eval(flag ? '1' : '2'); return eval('3'); }
  function deep(n) { return n ? [n].map((v) => deep(v - 1) + eval(String(v)))[0] : 0; }
  let settled = 0;
  function settle() {
    const form = settled++ % 3;
    if (form === 1) deep(24);
    scrub(false); scrub(true);
    if (form === 2) deep(12);
    churn();
  }
  function restart() { globalThis.reuseTick = 1; delete globalThis.reuseTick; globalThis.reuseWrite = 'global'; }
  function reads(shadow, src) { eval(shadow ? shadowRead : plain); return eval(src); }
  function writes(shadow, src) { eval(shadow ? shadowWrite : plain); eval(src); return [eval(shadow ? back : 'pad'), globalThis.reuseWrite]; }
  const run = (first, src, fn, expected) => {
    for (let round = 0; round < 8; round++) {
      restart();
      fn(first, src);
      settle();
      globalThis.reuseWrite = 'global';
      assert.deepStrictEqual(fn(!first, src), expected);
      settle();
    }
  };
  run(true, 'reuseRead', reads, 'global');
  run(false, 'reuseRead ', reads, 'local');
  run(true, 'reuseWrite = "set"', writes, [undefined, 'set']);
  run(false, 'reuseWrite = "set" ', writes, ['set', 'global']);
  for (let round = 0; round < 12; round++) {
    globalThis.reuseWrite = 'global';
    assert.strictEqual(reads(false, 'reuseRead'), 'global');
    settle();
    assert.strictEqual(reads(true, 'reuseRead'), 'local');
    settle();
    assert.deepStrictEqual(writes(false, 'reuseWrite = "set"'), [undefined, 'set']);
    settle();
    globalThis.reuseWrite = 'global';
    assert.deepStrictEqual(writes(true, 'reuseWrite = "set"'), ['set', 'global']);
    settle();
  }
  delete globalThis.reuseRead;
  delete globalThis.reuseWrite;
}

{
  globalThis.lexShadow = 'property';
  require('node:vm').runInThisContext('let lexShadow = "lexical"');
  const read = 'lexShadow', write = 'lexShadow = lexShadow + "!"';
  function reads() { return eval(read); }
  function writes() { eval(write); return eval(read); }
  for (let i = 0; i < 20; i++) assert.strictEqual(reads(), 'lexical');
  assert.strictEqual(writes(), 'lexical!');
  assert.strictEqual(writes(), 'lexical!!');
  assert.strictEqual(globalThis.lexShadow, 'property');
  delete globalThis.lexShadow;
}

console.log('ok');
