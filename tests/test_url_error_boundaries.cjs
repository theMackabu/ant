const assert = require('node:assert');

const operations = [
  (params, bad) => params.get(bad),
  (params, bad) => params.getAll(bad),
  (params, bad) => params.has(bad),
  (params, bad) => params.has('a', bad),
  (params, bad) => params.set(bad, 'c'),
  (params, bad) => params.set('a', bad),
  (params, bad) => params.append(bad, 'c'),
  (params, bad) => params.append('a', bad),
  (params, bad) => params.delete(bad),
  (params, bad) => params.delete('a', bad),
];

for (const reason of [undefined, null, new Error('URL coercion')]) {
  for (const operation of operations) {
    const url = new URL('https://example.test/?a=b');
    const bad = { toString() { throw reason; } };
    let caught = false;
    try {
      operation(url.searchParams, bad);
    } catch (error) {
      caught = true;
      assert.strictEqual(error, reason);
    }
    assert.ok(caught, 'URLSearchParams must propagate coercion errors');
    assert.strictEqual(url.search, '?a=b');
  }
}

const format = require('node:url').format;
for (const reason of [undefined, null, new Error('URL format')]) {
  for (const property of ['protocol', 'auth', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'slashes', 'query']) {
    const value = new Proxy({}, {
      get(target, key) {
        if (key === property) throw reason;
      },
    });
    let caught = false;
    try {
      format(value);
    } catch (error) {
      caught = true;
      assert.strictEqual(error, reason);
    }
    assert.ok(caught, 'url.format must propagate ' + property + ' getter failure');
  }
  let conversions = 0;
  const unsupported = {
    toString() { conversions++; throw reason; },
    [Symbol.toPrimitive]() { conversions++; throw reason; },
  };
  assert.strictEqual(format({ query: { a: unsupported } }), '?a=');
  assert.strictEqual(conversions, 0, 'url.format must not coerce unsupported query values');

  let caught = false;
  try {
    format({ query: { get a() { throw reason; } } });
  } catch (error) {
    caught = true;
    assert.strictEqual(error, reason);
  }
  assert.ok(caught, 'url.format must propagate query getters');

  caught = false;
  const values = ['value'];
  Object.defineProperty(values, '0', { get() { throw reason; } });
  try {
    format({ query: { a: values } });
  } catch (error) {
    caught = true;
    assert.strictEqual(error, reason);
  }
  assert.ok(caught, 'url.format must propagate query array getters');
}

for (const value of [
  undefined, null, NaN, Infinity, -Infinity, Symbol('query'), {}, /query/,
  new Date(0), function queryValue() {}, new String('boxed'), Object(1), Object(true), Object(1n),
]) {
  assert.strictEqual(format({ query: { a: value } }), '?a=');
}
assert.strictEqual(format({ query: {
  text: 'a b&c', integer: 42, decimal: 1.5, zero: -0,
  yes: true, no: false, bigint: 123456789012345678901234567890n,
} }), '?text=a%20b%26c&integer=42&decimal=1.5&zero=0&yes=true&no=false&bigint=123456789012345678901234567890');
assert.strictEqual(format({ query: {
  empty: [], a: ['a b', 1, true, 12n, null, {}, ['nested']], tail: 'z', other: [],
} }), '?a=a%20b&a=1&a=true&a=12&a=&a=&a=&tail=z');
assert.strictEqual(format({ query: { a: [] } }), '');
assert.strictEqual(format({ query: { a: [, 'x'] } }), '?a=&a=x');
assert.strictEqual(format({ query: { a: new Proxy(['x', null], {}) } }), '?a=x&a=');
queueMicrotask(() => console.log('URL error boundaries ok'));
