const assert = require('node:assert');

{
  const seen = [];
  const base = { toJSON() { seen.push('base'); return 'from-base'; } };
  const viaTrap = Object.create(new Proxy({}, {
    get(target, key, receiver) {
      if (key === 'toJSON') return () => 'from-trap';
      return Reflect.get(target, key, receiver);
    },
  }));
  assert.strictEqual(JSON.stringify(viaTrap), '"from-trap"');
  assert.strictEqual(JSON.stringify(Object.create(new Proxy(base, {}))), '"from-base"');
  assert.deepStrictEqual(seen, ['base']);
  assert.strictEqual(JSON.stringify({ nested: viaTrap }), '{"nested":"from-trap"}');
}

console.log('ok');
