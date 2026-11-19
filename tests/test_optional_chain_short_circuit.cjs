const assert = require('node:assert');

let evaluated;
const key = (k) => { evaluated.push(k); return k; };
const run = (fn) => { evaluated = []; const value = fn(); return [value, evaluated]; };

const target = { a: { b: 7 }, f(x) { return [this === target, x]; }, list: [1, 2] };

for (let i = 0; i < 3000; i++) {
  for (const nothing of [null, undefined]) {
    assert.deepStrictEqual(run(() => nothing?.[key('a')]), [undefined, []]);
    assert.deepStrictEqual(run(() => nothing?.[key('a')].b), [undefined, []]);
    assert.deepStrictEqual(run(() => nothing?.[key('a')][key('b')]), [undefined, []]);
    assert.deepStrictEqual(run(() => nothing?.[key('f')](key('arg'))), [undefined, []]);
    assert.deepStrictEqual(run(() => nothing?.a[key('b')]), [undefined, []]);
    assert.deepStrictEqual(run(() => nothing?.f(key('arg'))), [undefined, []]);
    assert.deepStrictEqual(run(() => nothing?.(key('arg'))), [undefined, []]);
    assert.deepStrictEqual(run(() => delete nothing?.[key('a')]), [true, []]);
    assert.deepStrictEqual(run(() => nothing?.[key('a')]?.[key('b')]), [undefined, []]);
    assert.deepStrictEqual(run(() => typeof nothing?.[key('a')]), ['undefined', []]);
    assert.deepStrictEqual(run(() => nothing?.[key('a')] ?? 'fallback'), ['fallback', []]);
    assert.deepStrictEqual(run(() => nothing?.[(() => { throw new Error('evaluated'); })()]), [undefined, []]);
  }

  assert.deepStrictEqual(run(() => target?.[key('a')]), [target.a, ['a']]);
  assert.deepStrictEqual(run(() => target?.[key('a')].b), [7, ['a']]);
  assert.deepStrictEqual(run(() => target?.[key('a')][key('b')]), [7, ['a', 'b']]);
  assert.deepStrictEqual(run(() => target?.[key('f')](key('arg'))), [[true, 'arg'], ['f', 'arg']]);
  assert.deepStrictEqual(run(() => target?.[key('missing')]?.[key('b')]), [undefined, ['missing']]);
  assert.deepStrictEqual(run(() => target?.list?.[key(1)]), [2, [1]]);
  assert.deepStrictEqual(run(() => target?.['a']), [target.a, []]);
  assert.deepStrictEqual(run(() => target?.[0]), [undefined, []]);
  assert.deepStrictEqual(run(() => 'str'?.[key(1)]), ['t', [1]]);
  assert.deepStrictEqual(run(() => 0?.[key('toFixed')]?.call(1.5, 0)), ['2', ['toFixed']]);

  const disposable = { k: 1 };
  assert.deepStrictEqual(run(() => delete disposable?.[key('k')]), [true, ['k']]);
  assert.strictEqual('k' in disposable, false);
}

function makeRoot() {
  const leaf = { a: 1, b: null, f() { return this; }, g() { return null; } };
  const mid = { a: leaf, b: null, f() { return this; }, g() { return null; } };
  return { a: mid, b: null, z: undefined, f() { return this; }, g() { return null; } };
}

let seed = 12345;
function rnd(n) { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed % n; }
const names = ['a', 'a', 'b', 'f', 'g', 'z'];

function makeChain() {
  const links = [];
  const count = 1 + rnd(4);
  let optionalSeen = false;
  for (let i = 0; i < count; i++) {
    const optional = rnd(3) === 0 || (i === count - 1 && !optionalSeen && rnd(2) === 0);
    optionalSeen = optionalSeen || optional;
    const kind = rnd(4);
    if (kind === 0 && i > 0) links.push({ call: true, optional, argument: rnd(2) === 0 });
    else links.push({ name: names[rnd(names.length)], computed: kind === 1, optional });
    if (rnd(6) === 0 && i < count - 1) links[links.length - 1].close = true;
  }
  return links;
}

function chainSource(links) {
  let text = 'x', opens = 0;
  for (const link of links) {
    if (link.call) text += (link.optional ? '?.' : '') + '(' + (link.argument ? "k('arg')" : '') + ')';
    else if (link.computed) text += (link.optional ? '?.' : '') + "[k('" + link.name + "')]";
    else text += (link.optional ? '?.' : '.') + link.name;
    if (link.close) { text = '(' + text + ')'; opens++; }
  }
  return text;
}

function chainExpected(links, base, log, stopBeforeLast) {
  let value = base, receiver;
  let skipped = false;
  const limit = stopBeforeLast ? links.length - 1 : links.length;
  for (let i = 0; i < limit; i++) {
    const link = links[i];
    if (!skipped) {
      if (link.optional && (value === null || value === undefined)) { skipped = true; value = undefined; receiver = undefined; }
      else if (link.call) {
        if (link.argument) log.push('arg');
        if (typeof value !== 'function') throw new TypeError('not a function');
        value = value.call(receiver);
        receiver = undefined;
      } else {
        if (link.computed) log.push(link.name);
        if (value === null || value === undefined) throw new TypeError('nullish');
        receiver = value;
        value = value[link.name];
      }
    }
    if (link.close) skipped = false;
  }
  return { value, skipped };
}

function show(v) {
  if (v === undefined) return 'undefined';
  if (typeof v === 'function') return 'function';
  if (v && typeof v === 'object') return 'object:' + Object.keys(v).join();
  return String(v);
}

for (let i = 0; i < 1500; i++) {
  const links = makeChain();
  const text = chainSource(links);
  const wrapper = rnd(8);
  const last = links[links.length - 1];
  const isDelete = wrapper === 0 && !last.call && !last.close;
  const source = isDelete ? 'return delete ' + text : wrapper === 1 ? 'return typeof ' + text : wrapper === 2 ? 'return [' + text + '][0]' : 'return ' + text;
  const compiled = new Function('x', 'k', source);
  const rounds = i % 5 === 0 ? 130 : 3;
  for (const pick of [0, 1, 2]) {
    const expectedLog = [];
    let expected;
    try {
      const base = pick === 0 ? makeRoot() : pick === 1 ? null : undefined;
      if (isDelete) {
        const before = chainExpected(links, base, expectedLog, true);
        if (before.skipped) expected = 'true';
        else {
          if (last.optional && (before.value === null || before.value === undefined)) expected = 'true';
          else {
            if (last.computed) expectedLog.push(last.name);
            if (before.value === null || before.value === undefined) throw new TypeError('nullish');
            expected = String(delete before.value[last.name]);
          }
        }
      } else {
        const result = chainExpected(links, base, expectedLog, false);
        expected = wrapper === 1 ? typeof result.value : show(result.value);
      }
    } catch (e) {
      expected = 'THROW ' + e.name;
    }
    for (let round = 0; round < rounds; round++) {
      const log = [];
      let actual;
      try {
        const value = compiled(pick === 0 ? makeRoot() : pick === 1 ? null : undefined, (name) => { log.push(name); return name; });
        actual = isDelete || wrapper === 1 ? String(value) : show(value);
      } catch (e) {
        actual = 'THROW ' + e.name;
      }
      assert.strictEqual(actual, expected, source + ' base ' + pick);
      assert.deepStrictEqual(log, expectedLog, source + ' base ' + pick + ' keys');
    }
  }
}

{
  const holder = { b: null };
  assert.throws(() => holder?.b.c, TypeError);
  assert.throws(() => holder?.b.c.d, TypeError);
  assert.throws(() => holder.b?.c.d.e === undefined && holder?.b.c(), TypeError);
  assert.throws(() => (null?.b).c, TypeError);
  assert.throws(() => (null?.b.c).d, TypeError);
  assert.throws(() => (null?.f)(), TypeError);
  assert.throws(() => delete holder?.b.c, TypeError);
  assert.strictEqual(delete null?.b.c, true);
  assert.strictEqual(null?.b.c.d.e, undefined);
  assert.strictEqual(null?.b().c().d, undefined);
  assert.strictEqual(null?.f()(), undefined);
  assert.strictEqual(null?.()().x, undefined);
  const self = { f() { return this; }, inner: { f() { return this; } } };
  assert.strictEqual(self?.f(), self);
  assert.strictEqual((self?.f)(), self);
  assert.strictEqual(self?.inner.f(), self.inner);
  assert.strictEqual(self.inner?.f?.(), self.inner);
  assert.strictEqual((self?.inner.f)(), self.inner);
  let effects = 0;
  assert.strictEqual(null?.b[effects++].c(effects++), undefined);
  assert.strictEqual(effects, 0);
  assert.strictEqual(self?.inner.f(null?.x.y, effects++), self.inner);
  assert.strictEqual(effects, 1);
  for (const bad of ['a?.b`x`', 'new a?.b()', 'a?.b.c`x`']) {
    assert.throws(() => new Function('a', bad), SyntaxError, bad);
  }
}

console.log('ok');
