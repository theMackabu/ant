const assert = require('node:assert');

{
  const read = 'x + y + i', write = 'acc = acc + i; x';
  function outer(x, y, strict) {
    let acc = 0;
    function sloppyInner(i, src) { return eval(src); }
    function strictInner(i, src) { 'use strict'; return eval(src); }
    const inner = strict ? strictInner : sloppyInner;
    const out = [];
    for (let k = 0; k < 6; k++) out.push(inner(k, read), inner(k, write));
    return [out, acc];
  }
  for (const strict of [false, true]) {
    assert.deepStrictEqual(outer(1, 2, strict), [[3, 1, 4, 1, 5, 1, 6, 1, 7, 1, 8, 1], 15]);
    assert.deepStrictEqual(outer(10, 20, strict), [[30, 10, 31, 10, 32, 10, 33, 10, 34, 10, 35, 10], 15]);
  }
}

{
  const src = 'x', declare = 'var x = "shadow"';
  function outer(x) {
    function inner(shadow) { if (shadow) eval(declare); return eval(src); }
    const out = [];
    for (let k = 0; k < 5; k++) out.push(inner(false));
    out.push(inner(true), inner(false), inner(true));
    return out;
  }
  for (let round = 0; round < 3; round++)
    assert.deepStrictEqual(outer('outer'), ['outer', 'outer', 'outer', 'outer', 'outer', 'shadow', 'outer', 'shadow']);
}

{
  const src = 'x', declare = 'var pad = 1', shadow = 'var x = "shadow"';
  function outer(x) {
    function inner(shadowed) { eval(declare); if (shadowed) eval(shadow); return eval(src); }
    const out = [];
    for (let k = 0; k < 5; k++) out.push(inner(false));
    out.push(inner(true), inner(false));
    return out;
  }
  for (let round = 0; round < 3; round++)
    assert.deepStrictEqual(outer('outer'), ['outer', 'outer', 'outer', 'outer', 'outer', 'shadow', 'outer']);
}

{
  const src = 'x + ":" + y';
  const makers = [
    (x, y) => function () { return eval(src); },
    (y, x) => function () { return eval(src); },
    (pad, x, y) => function () { return eval(src); },
    (x, pad, y) => function () { let more = pad; return eval(src) + (more ? '' : ''); },
    (y, pad, more, x) => function () { return eval(src); },
    (x, y) => function (x) { return eval(src); },
    (x) => function (y) { return eval(src); },
    (x, y) => function () { { let x = 'block'; return eval(src); } },
    (x, y) => function () { 'use strict'; return eval(src); },
    (y, x) => function () { 'use strict'; return eval(src); },
  ];
  const hosts = [
    makers[0]('x0', 'y0'), makers[1]('y1', 'x1'), makers[2](0, 'x2', 'y2'), makers[3]('x3', 0, 'y3'),
    makers[4]('y4', 0, 0, 'x4'), makers[5]('no', 'y5'), makers[6]('x6'), makers[7]('no', 'y7'),
    makers[8]('x8', 'y8'), makers[9]('y9', 'x9'),
  ];
  const args = [[], [], [], [], [], ['x5'], ['y6'], [], [], []];
  const expected = ['x0:y0', 'x1:y1', 'x2:y2', 'x3:y3', 'x4:y4', 'x5:y5', 'x6:y6', 'block:y7', 'x8:y8', 'x9:y9'];
  for (let round = 0; round < 6; round++)
    assert.deepStrictEqual(hosts.map((host, index) => host(...args[index])), expected);
  for (let round = 0; round < 6; round++) {
    assert.strictEqual(hosts[round % 2](), expected[round % 2]);
    assert.strictEqual(hosts[8 + round % 2](), expected[8 + round % 2]);
  }
}

{
  const src = 'late', set = 'late = late + 1';
  function outer(early) {
    function inner(source) { return eval(source); }
    if (early) return inner(src);
    let late = 1;
    const out = [];
    for (let k = 0; k < 4; k++) out.push(inner(src), inner(set));
    return out;
  }
  for (let round = 0; round < 3; round++) {
    assert.deepStrictEqual(outer(false), [1, 2, 2, 3, 3, 4, 4, 5]);
    assert.throws(() => outer(true), ReferenceError);
  }
}

{
  const set = 'fixed = 2', get = 'fixed';
  function outer() {
    const fixed = 1;
    function inner(source) { return eval(source); }
    const out = [];
    for (let k = 0; k < 4; k++) {
      out.push(inner(get));
      try { inner(set); out.push('stored'); } catch (error) { out.push(error instanceof TypeError); }
    }
    return out;
  }
  assert.deepStrictEqual(outer(), [1, true, 1, true, 1, true, 1, true]);
}

{
  const set = 'a = a + 10', get = 'a';
  function outer(a) {
    function inner(source) { return eval(source); }
    const out = [];
    for (let k = 0; k < 4; k++) out.push(inner(set), arguments[0], inner(get));
    a = 'direct';
    out.push(inner(get), arguments[0]);
    arguments[0] = 'indexed';
    out.push(inner(get));
    return out;
  }
  for (let round = 0; round < 3; round++)
    assert.deepStrictEqual(outer(1), [11, 11, 11, 21, 21, 21, 31, 31, 31, 41, 41, 41, 'direct', 'direct', 'indexed']);
}

{
  const src = 'typeof arguments + ":" + arguments.length';
  function outer(arguments) {
    const arrow = () => eval(src);
    function own() { return eval(src); }
    const out = [];
    for (let k = 0; k < 4; k++) out.push(arrow(), own(1, 2));
    return out;
  }
  assert.deepStrictEqual(outer('ab'), ['string:2', 'object:2', 'string:2', 'object:2', 'string:2', 'object:2', 'string:2', 'object:2']);
}

{
  const src = 'counter = counter + 1';
  function outer() {
    let counter = 0;
    function inner() { return eval(src); }
    return [inner, () => counter];
  }
  const [bumpA, readA] = outer(), [bumpB, readB] = outer();
  for (let k = 0; k < 5; k++) { bumpA(); bumpB(); bumpB(); }
  assert.deepStrictEqual([readA(), readB()], [5, 10]);
}

{
  const src = 'value';
  function outer(value) {
    function middle() {
      function inner() { return eval(src); }
      return inner();
    }
    return middle();
  }
  for (let k = 0; k < 6; k++) assert.strictEqual(outer(k), k);
}

{
  const sources = [];
  for (let j = 0; j < 12; j++) sources.push('own + base + ' + j);
  const hosts = [];
  for (let j = 0; j < 12; j++) hosts.push(new Function('base', 'src', 'var own = ' + j * 100 + '; return eval(src);'));
  for (let round = 0; round < 6; round++) {
    for (let h = 0; h < hosts.length; h++) {
      for (let s = 0; s < sources.length; s++) assert.strictEqual(hosts[h](round, sources[s]), h * 100 + round + s);
    }
  }
}

{
  const tagged = '(parts => parts)`a${1}b`';
  function host() { return eval(tagged); }
  const first = host(), second = host(), third = host();
  assert.notStrictEqual(first, second);
  assert.notStrictEqual(second, third);
  assert.deepStrictEqual([...first], ['a', 'b']);
}

{
  const src = 'x';
  function outer(x) {
    function free() { return eval(src); }
    function own(x) { return eval(src); }
    return [free(), free(), free(), own('own'), free(), own('again'), own('third'), free()];
  }
  for (let round = 0; round < 3; round++)
    assert.deepStrictEqual(outer('outer' + round), ['outer' + round, 'outer' + round, 'outer' + round, 'own', 'outer' + round, 'again', 'third', 'outer' + round]);
}

{
  const set = 'd = d + 1', get = 'd';
  function outer(a, b, c, d) {
    function inner(p0, p1, p2, p3, p4, p5, source) { const out = eval(source); return [out, p0, p1, p2, p3, p4, p5]; }
    const out = [];
    for (let k = 0; k < 5; k++) out.push(inner(10, 11, 12, 13, 14, 15, set), inner(20, 21, 22, 23, 24, 25, get));
    return [out, a, b, c, d];
  }
  for (let round = 0; round < 3; round++) {
    const [out, a, b, c, d] = outer('a', 'b', 'c', round);
    assert.deepStrictEqual([a, b, c, d], ['a', 'b', 'c', round + 5]);
    for (let k = 0; k < 5; k++) {
      assert.deepStrictEqual(out[2 * k], [round + k + 1, 10, 11, 12, 13, 14, 15]);
      assert.deepStrictEqual(out[2 * k + 1], [round + k + 1, 20, 21, 22, 23, 24, 25]);
    }
  }
}

{
  const src = 'reuseX', churnSrc = 'for (let i = 0; i < 35000; i++) kept = { i }';
  function churn() { let kept = null; eval(churnSrc); return kept; }
  function settle() { churn(); churn(); }
  function outer(reuseX, declare) {
    function inner() { eval(declare); return eval(src); }
    return [inner(), inner(), inner()];
  }
  for (let round = 0; round < 24; round++) {
    assert.deepStrictEqual(outer('outer' + round, 'var reusePad' + (round % 3) + ' = 1'), ['outer' + round, 'outer' + round, 'outer' + round]);
    settle();
    assert.deepStrictEqual(outer('outer', 'var reuseX = "shadow' + round + '"'), ['shadow' + round, 'shadow' + round, 'shadow' + round]);
    settle();
  }
}

console.log('ok');
