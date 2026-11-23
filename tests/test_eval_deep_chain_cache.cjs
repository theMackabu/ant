const assert = require('node:assert');
const vm = require('node:vm');

const G = globalThis;
const declOuter = 'var o1 = 1', declInner = 'var x1 = 1', declLeaf = 'var y1 = 1';
const declTwo = 'var gone = 1, kept = 2', delGone = 'delete gone';

function* outer(body, steps) {
  eval(declOuter);
  const f = (function inner() { eval(declInner); return eval(body); })();
  yield f;
  for (const s of steps) yield eval(s);
}

function drive(body, steps, probe) {
  const it = outer(body, steps);
  const f = it.next().value;
  const r = [probe(f), probe(f), probe(f)];
  for (let i = 0; i < steps.length; i++) { it.next(); r.push(probe(f), probe(f), probe(f)); }
  return r;
}

const call = (f) => f();
const triple = (v) => [v, v, v];
const reader = '(function () { return typeof chainName === "undefined" ? "missing" : typeof chainName === "function" ? "fn" : chainName; })';

{
  G.chainName = 'global';
  assert.deepStrictEqual(
    drive(reader, ['var chainName = "outer"', 'delete chainName', 'var chainName = "again"', 'var unrelated = 1'], call),
    [...triple('global'), ...triple('outer'), ...triple('global'), ...triple('again'), ...triple('again')]);
  assert.deepStrictEqual(
    drive(reader, ['var unrelated = 1', 'var chainName = "late"', 'var other = 2', 'delete chainName'], call),
    [...triple('global'), ...triple('global'), ...triple('late'), ...triple('late'), ...triple('global')]);
  assert.deepStrictEqual(
    drive(reader, ['function chainName() {}', 'delete chainName', 'var chainName', 'chainName = 5'], call),
    [...triple('global'), ...triple('fn'), ...triple('global'), ...triple('missing'), ...triple(5)]);
}

{
  const a = outer(reader, ['var chainName = "A"', 'delete chainName']);
  const b = outer(reader, ['var other = 1', 'var chainName = "B"']);
  const fa = a.next().value, fb = b.next().value;
  const both = () => [fa(), fa(), fa(), fb(), fb(), fb(), fa(), fb(), fa(), fa(), fb(), fb()];
  const row = (x, y) => [x, x, x, y, y, y, x, y, x, x, y, y];
  const r = [both()];
  a.next(); r.push(both());
  b.next(); r.push(both());
  b.next(); r.push(both());
  a.next(); r.push(both());
  assert.deepStrictEqual(r, [
    row('global', 'global'), row('A', 'global'), row('A', 'global'), row('A', 'B'), row('global', 'B'),
  ]);
}

{
  const fs = [];
  for (let i = 0; i < 6; i++) {
    const it = outer(reader, ['var chainName = "t' + i + '"']);
    const f = it.next().value;
    if (i % 2) it.next();
    fs.push(f);
  }
  const r = [];
  for (let k = 0; k < 3; k++) for (const f of fs) r.push(f());
  const row = ['global', 't1', 'global', 't3', 'global', 't5'];
  assert.deepStrictEqual(r, [...row, ...row, ...row]);
}

{
  const it = outer(reader, []);
  const f = it.next().value;
  const r = [f(), f(), f()];
  G.chainName = 'g2'; r.push(f(), f());
  delete G.chainName; r.push(f(), f(), f());
  let reads = 0;
  Object.defineProperty(G, 'chainName', { get() { reads++; return 'acc'; }, configurable: true });
  r.push(f(), f(), f());
  Object.defineProperty(G, 'chainName', { value: 'data', writable: true, configurable: true });
  r.push(f(), f(), f());
  G.chainExtra = 1; r.push(f(), f());
  delete G.chainExtra; r.push(f(), f());
  Object.prototype.chainName = 'inherited';
  delete G.chainName; r.push(f(), f(), f());
  G.chainName = 'own'; r.push(f(), f());
  delete Object.prototype.chainName;
  G.chainName = 'global'; r.push(f(), f());
  assert.deepStrictEqual(r, [
    'global', 'global', 'global', 'g2', 'g2', 'missing', 'missing', 'missing',
    'acc', 'acc', 'acc', 'data', 'data', 'data', 'data', 'data', 'data', 'data',
    'inherited', 'inherited', 'inherited', 'own', 'own', 'global', 'global',
  ]);
  assert.strictEqual(reads, 9);
}

{
  G.chainCount = 0;
  const writer = '(function () { chainCount = chainCount + 1; return chainCount; })';
  assert.deepStrictEqual(drive(writer, ['var chainCount = 100', 'delete chainCount', 'var pad = 1'], call),
    [1, 2, 3, 101, 102, 103, 4, 5, 6, 7, 8, 9]);
  assert.strictEqual(G.chainCount, 9);

  G.chainCount = 0;
  const it = outer(writer, []);
  const f = it.next().value;
  const r = [f(), f(), f()];
  Object.defineProperty(G, 'chainCount', { value: 50, writable: false, configurable: true });
  r.push(f(), f(), f());
  Object.defineProperty(G, 'chainCount', { value: 60, writable: true, configurable: true });
  r.push(f(), f(), f());
  let stored = 70;
  Object.defineProperty(G, 'chainCount', { get() { return stored; }, set(v) { stored = v + 10; }, configurable: true });
  r.push(f(), f(), f());
  delete G.chainCount;
  assert.deepStrictEqual(r, [1, 2, 3, 50, 50, 50, 61, 62, 63, 81, 92, 103]);
  assert.strictEqual(stored, 103);
}

{
  G.chainStrict = 1;
  const writer = '(function () { "use strict"; try { chainStrict = chainStrict + 1; return chainStrict; } catch (e) { return e.constructor.name; } })';
  const it = outer(writer, ['var chainStrict = 10']);
  const f = it.next().value;
  const r = [f(), f(), f()];
  Object.defineProperty(G, 'chainStrict', { value: 9, writable: false, configurable: true });
  r.push(f(), f(), f());
  delete G.chainStrict;
  r.push(f(), f(), f());
  it.next();
  r.push(f(), f(), f());
  assert.deepStrictEqual(r, [2, 3, 4, 'TypeError', 'TypeError', 'TypeError', 'ReferenceError', 'ReferenceError', 'ReferenceError', 11, 12, 13]);
  assert.strictEqual('chainStrict' in G, false);
}

{
  G.arguments = 'G';
  function* arrows(a) {
    eval(declOuter);
    const f = (() => { eval(declInner); return eval('(() => typeof arguments + ":" + (typeof arguments === "object" ? arguments[0] : arguments))'); })();
    yield f;
    yield eval('arguments = "assigned"');
    yield eval('var arguments = "declared"');
  }
  const it = arrows(1);
  const f = it.next().value;
  const r = [f(), f(), f()];
  it.next(); r.push(f(), f(), f());
  it.next(); r.push(f(), f(), f());
  delete G.arguments;
  assert.deepStrictEqual(r, [...triple('object:1'), ...triple('string:assigned'), ...triple('string:declared')]);

  function plain(a) {
    eval(declOuter);
    return (function () {
      eval(declInner);
      const r = [];
      for (let k = 0; k < 4; k++) r.push(eval('(function () { return arguments.length; })')(k, k), arguments.length, eval('arguments[0]'));
      return r;
    })(7, 8, 9);
  }
  assert.deepStrictEqual(plain(1), [2, 3, 7, 2, 3, 7, 2, 3, 7, 2, 3, 7]);
}

{
  G.chainLexical = 'property';
  const it = outer('(function (v) { if (v !== undefined) chainLexical = v; return chainLexical; })', []);
  const f = it.next().value;
  const r = [f(), f(), f()];
  vm.runInThisContext('let chainLexical = "lexical"');
  r.push(f(), f(), f(), f('written'), f(), f());
  assert.deepStrictEqual(r, ['property', 'property', 'property', 'lexical', 'lexical', 'lexical', 'written', 'written', 'written']);
  assert.strictEqual(G.chainLexical, 'property');
  delete G.chainLexical;
}

{
  const body = 'var r = []; for (var i = 0; i < 9; i++) { r.push(chainName); if (i === 2) eval("var chainName = \\"inner\\""); if (i === 5) eval("delete chainName"); } r';
  const it = outer(body, []);
  assert.deepStrictEqual(it.next().value, ['global', 'global', 'global', 'inner', 'inner', 'inner', 'global', 'global', 'global']);

  const stepped = '(function (step) { var r = []; for (var i = 0; i < 9; i++) { r.push(chainName); if (i === 2 || i === 5) step(); } return r; })';
  const steps = outer(stepped, ['var chainName = "stepped"', 'delete chainName']);
  assert.deepStrictEqual(steps.next().value(() => steps.next()),
    ['global', 'global', 'global', 'stepped', 'stepped', 'stepped', 'global', 'global', 'global']);
}

{
  const levels = (depth, core) => { let src = core; for (let k = 1; k < depth; k++) src = 'eval(' + JSON.stringify(src) + ')'; return src; };
  const core = 'var r = []; for (var i = 0; i < 6; i++) { r.push(chainName); chainCount = chainCount + 1; if (i === 3) eval(late); } r';
  const late = 'var chainName = "nested"';
  function nested(depth) { eval(declOuter); return eval(levels(depth, core)); }
  G.chainCount = 0;
  for (let depth = 1; depth <= 6; depth++)
    assert.deepStrictEqual(nested(depth), ['global', 'global', 'global', 'global', 'nested', 'nested']);
  assert.strictEqual(G.chainCount, 36);
  delete G.chainCount;
}

{
  const keep = [];
  for (let i = 0; i < 4; i++) {
    const it = outer(reader, ['var chainName = "kept' + i + '"']);
    const f = it.next().value;
    if (i & 1) it.next();
    keep.push(f);
  }
  let junk = null;
  for (let i = 0; i < 60000; i++) junk = { i };
  const row = ['global', 'kept1', 'global', 'kept3'];
  for (let round = 0; round < 3; round++) assert.deepStrictEqual(keep.map(call), row);
  G.chainName = 'moved';
  assert.deepStrictEqual(keep.map(call), ['moved', 'kept1', 'moved', 'kept3']);
  G.chainName = 'global';
  assert.strictEqual(junk.i, 59999);
}

{
  const declName = 'var chainName = "local"', delName = 'delete chainName';
  function reads() {
    eval(declTwo); eval(delGone);
    const r = [];
    for (let k = 0; k < 8; k++) {
      r.push(chainName);
      if (k === 2) eval(declName);
      if (k === 5) eval(delName);
    }
    return r;
  }
  function writes() {
    eval(declTwo); eval(delGone);
    const r = [];
    for (let k = 0; k < 8; k++) {
      chainCount = chainCount + 1;
      r.push(chainCount);
      if (k === 2) eval('var chainCount = 40');
      if (k === 5) eval('delete chainCount');
    }
    return r;
  }
  function keeps() {
    eval(declTwo); eval(delGone);
    let t = 0;
    for (let k = 0; k < 8; k++) { t += eval('kept = kept + 1; kept'); if (k === 3) eval('var gone = 100'); if (k > 3) t += gone; }
    return t;
  }
  for (let round = 0; round < 3; round++) {
    G.chainCount = 0;
    assert.deepStrictEqual(reads(), ['global', 'global', 'global', 'local', 'local', 'local', 'global', 'global']);
    assert.deepStrictEqual(writes(), [1, 2, 3, 41, 42, 43, 4, 5]);
    assert.strictEqual(G.chainCount, 5);
    assert.strictEqual(keeps(), 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10 + 400);
  }
  delete G.chainCount;
}

{
  const src = 'var r = []; delete chainBlock; for (var i = 0; i < 7; i++) { r.push(typeof chainBlock === "function" ? chainBlock() : chainBlock); if (i === 3) { function chainBlock() { return "block"; } } } r';
  function annex() { eval(declTwo); eval(delGone); return eval(src); }
  function annexDeep() { eval(declOuter); return (function () { eval(declInner); return eval(src); })(); }
  for (let round = 0; round < 3; round++) {
    G.chainBlock = 'global';
    assert.deepStrictEqual(annex(), ['global', 'global', 'global', 'global', 'block', 'block', 'block']);
    G.chainBlock = 'global';
    assert.deepStrictEqual(annexDeep(), ['global', 'global', 'global', 'global', 'block', 'block', 'block']);
  }
  delete G.chainBlock;
}

{
  const readCell = 'var r = []; for (var i = 0; i < 5; i++) { r.push(p + ":" + live + ":" + fixed); p = p + 1; live = live + "!"; } r';
  const writeFixed = 'var r = []; for (var i = 0; i < 4; i++) { try { fixed = i; r.push("stored"); } catch (e) { r.push(e.constructor.name); } } r';
  function cells(p) {
    let live = 'let';
    const fixed = 7;
    eval(declOuter);
    const out = (function mid() {
      eval(declInner);
      return (function leaf() { eval(declLeaf); return [eval(readCell), eval(writeFixed)]; })();
    })();
    return [out, p, live, arguments[0]];
  }
  for (let round = 0; round < 3; round++) assert.deepStrictEqual(cells(1), [
    [['1:let:7', '2:let!:7', '3:let!!:7', '4:let!!!:7', '5:let!!!!:7'], ['TypeError', 'TypeError', 'TypeError', 'TypeError']],
    6, 'let!!!!!', 6,
  ]);

  const shadowed = '(function (hide) { var r = []; for (var i = 0; i < 6; i++) { r.push(p); if (i === 2) hide(); } return r; })';
  function shadows(p) {
    eval(declOuter);
    const mid = (function* () {
      eval(declInner);
      yield (function leaf() { eval(declLeaf); return eval(shadowed); })();
      yield eval('var p = "hidden"');
    })();
    const f = mid.next().value;
    return f(() => mid.next());
  }
  for (let round = 0; round < 3; round++)
    assert.deepStrictEqual(shadows('param'), ['param', 'param', 'param', 'hidden', 'hidden', 'hidden']);

  const late = '(function () { try { return lateCell; } catch (e) { return e.constructor.name; } })';
  function tdz() {
    eval(declOuter);
    const f = (function mid() { eval(declInner); return (function leaf() { eval(declLeaf); return eval(late); })(); })();
    const r = [f(), f(), f()];
    let lateCell = 'ready';
    r.push(f(), f(), f());
    lateCell = 'changed';
    r.push(f(), f());
    return r;
  }
  for (let round = 0; round < 3; round++) assert.deepStrictEqual(tdz(),
    [...triple('ReferenceError'), ...triple('ready'), 'changed', 'changed']);
}

{
  const context = vm.createContext({ shared: 'context', log: [] });
  vm.runInContext(`
    var declOuter = 'var o1 = 1', declInner = 'var x1 = 1', body = '(function () { return shared; })';
    function make() { eval(declOuter); return (function inner() { eval(declInner); return eval(body); })(); }
    var read = make();
    function loop() {
      eval(declOuter);
      return (function inner() { eval(declInner); return eval('var r = []; for (var i = 0; i < 6; i++) { r.push(shared); if (i === 2) shared = "written"; } r'); })();
    }
  `, context);
  const r = [vm.runInContext('read()', context), vm.runInContext('read()', context), vm.runInContext('read()', context)];
  context.shared = 'replaced';
  r.push(vm.runInContext('read()', context), vm.runInContext('read()', context));
  assert.deepStrictEqual(r, ['context', 'context', 'context', 'replaced', 'replaced']);
  assert.deepStrictEqual(Array.from(vm.runInContext('loop()', context)), ['replaced', 'replaced', 'replaced', 'written', 'written', 'written']);
  assert.strictEqual(context.shared, 'written');
  assert.strictEqual('shared' in G, false);
}

{
  const declShadow = 'var reuseName = "local"', declPlain = 'var reusePad = 1';
  const probe = 'var r = ""; for (var i = 0; i < n; i++) r = typeof reuseName; r';
  function make(shadow) {
    eval(shadow ? declShadow : declPlain);
    return function mid(n) { eval(declInner); return (function leaf() { eval(declLeaf); return eval(probe); })(); };
  }
  const shadowed = make(true), plain = make(false);
  const ring = [0, 0, 0, 0, 0, 0, 0, 0];
  for (const spread of [0, 23]) {
    for (let round = 0; round < 24; round++) {
      assert.strictEqual(shadowed(3), 'string');
      for (let k = 0; k < (spread ? round % spread : 0); k++) ring[k & 7] = { k };
      let stale = 0;
      for (let k = 0; k < 9000; k++) if (plain(1) !== 'undefined') stale++;
      assert.strictEqual(stale, 0);
    }
  }
}

console.log('ok');
