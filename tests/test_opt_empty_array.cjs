const assert = require('node:assert');

const WARM = 3000;

{
  function fill(n) { const out = []; for (let j = 0; j < n; j++) out.push(j); return out; }
  function tagged(tag, ...rest) { rest.push(tag); return rest; }
  function restCount(tag, ...rest) { return rest.length; }
  function restGrow(...rest) { try { rest.push(1, 2); rest[rest.length + 1] = 3; return rest; } catch (error) { return null; } }
  function restKept(...rest) { try { return rest; } catch (error) { return null; } }
  function sparse(i) { const out = []; out[3] = i; return out; }
  function lengthened(i) { const out = []; out.length = 2; out.push(i); return out; }
  function fresh() { return []; }
  function nested() { return [[], [[]], []]; }
  function literalGrow(i) { const out = [i, 2, 3]; out.push(4); out.push(5, 6, 7); return out; }
  function popEmpty() { const out = []; return [out.pop(), out.length]; }
  const kept = [];
  for (let i = 0; i < WARM * 4; i++) {
    const n = i % 23;
    const filled = fill(n);
    assert.strictEqual(filled.length, n);
    if (n) assert.strictEqual(filled[n - 1], n - 1);
    assert.deepStrictEqual(tagged(i), [i]);
    assert.deepStrictEqual(tagged(i, 1, 2, 3), [1, 2, 3, i]);
    assert.strictEqual(restCount(i), 0);
    assert.strictEqual(restCount(i, 1), 1);
    const grown = restGrow(i, 5, 6);
    assert.strictEqual(grown.length, 7);
    assert.deepStrictEqual(grown.slice(0, 5), [i, 5, 6, 1, 2]);
    assert.strictEqual(5 in grown, false);
    assert.strictEqual(grown[6], 3);
    assert.deepStrictEqual(restGrow(), [1, 2, , 3]);
    assert.deepStrictEqual(restKept(), []);
    assert.deepStrictEqual(restKept(1, 2, 3, 4, 5, 6, 7, 8, 9), [1, 2, 3, 4, 5, 6, 7, 8, 9]);
    const holes = sparse(i);
    assert.strictEqual(holes.length, 4);
    assert.strictEqual(0 in holes, false);
    assert.strictEqual(holes[3], i);
    const longer = lengthened(i);
    assert.strictEqual(longer.length, 3);
    assert.strictEqual(1 in longer, false);
    assert.strictEqual(longer[2], i);
    const empty = fresh();
    assert.strictEqual(empty.length, 0);
    assert.strictEqual(Array.isArray(empty), true);
    assert.strictEqual(0 in empty, false);
    assert.strictEqual(Object.getPrototypeOf(empty), Array.prototype);
    assert.notStrictEqual(empty, fresh());
    assert.deepStrictEqual(literalGrow(i), [i, 2, 3, 4, 5, 6, 7]);
    assert.deepStrictEqual(popEmpty(), [undefined, 0]);
    if (i % 500 === 0) kept.push(empty, nested(), restKept(i, 1));
  }
  for (let k = 0; k < kept.length; k += 3) {
    assert.strictEqual(kept[k].length, 0);
    assert.deepStrictEqual(kept[k + 1], [[], [[]], []]);
    kept[k].push(k);
    assert.deepStrictEqual(kept[k], [k]);
    assert.strictEqual(kept[k + 2].length, 2);
    kept[k + 2].push(k);
    assert.deepStrictEqual(kept[k + 2].slice(1), [1, k]);
  }
  assert.throws(() => Object.freeze([]).push(1), TypeError);
  function pushInto(target, value) { return target.push(value); }
  for (let i = 0; i < WARM; i++) assert.strictEqual(pushInto([1, 2, 3], i), 4);
  assert.throws(() => pushInto(Object.freeze([1, 2, 3]), 1), TypeError);
  assert.throws(() => pushInto(Object.preventExtensions([1, 2, 3]), 1), TypeError);
  assert.strictEqual(pushInto({ length: 2, push: Array.prototype.push }, 1), 3);
  for (let i = 0; i < WARM; i++) assert.strictEqual(pushInto([1, 2, 3], i), 4);
}

console.log('ok');
