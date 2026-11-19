const assert = require('node:assert');

const HOT = 400;

function makeClasses() {
  class A { constructor() { this.x = 0; } }
  class B extends A {
    put(v) { super.x = v; return v; }
    get() { return super.x; }
    getName() { return super.name; }
    call() { return super.m(); }
    update() { return super.n++; }
  }
  return { A, B };
}

function warm(fn) {
  for (let i = 0; i < HOT; i++) fn(i);
}

function test(name, body) {
  try {
    body();
  } catch (error) {
    error.message = `${name}: ${error.message}`;
    throw error;
  }
}

test('proxy on the super chain sees the real receiver', () => {
  const seen = [];
  const proxy = new Proxy({}, {
    get(target, key, receiver) { seen.push(['get', key, receiver]); return 'trapped'; },
    set(target, key, value, receiver) { seen.push(['set', key, receiver]); return true; },
  });
  const direct = { __proto__: proxy, read() { return super.x; }, readKey(k) { return super[k]; }, write() { super.x = 1; } };
  const deeper = { __proto__: { __proto__: proxy }, read() { return super.x; } };
  const tag = Symbol('tag');
  for (let i = 0; i < HOT; i++) {
    seen.length = 0;
    assert.strictEqual(direct.read(), 'trapped');
    assert.strictEqual(direct.readKey('y'), 'trapped');
    assert.strictEqual(direct.readKey(tag), 'trapped');
    direct.write();
    assert.strictEqual(deeper.read(), 'trapped');
    assert.deepStrictEqual(seen.map(([kind, key, receiver]) => [kind, key, receiver === direct || receiver === deeper]),
      [['get', 'x', true], ['get', 'y', true], ['get', tag, true], ['set', 'x', true], ['get', 'x', true]]);
  }
  const forwarding = { __proto__: new Proxy({ get self() { return this; } }, {}), read() { return super.self; } };
  assert.strictEqual(forwarding.read(), forwarding);
  const receiver = {};
  assert.strictEqual(Reflect.get(new Proxy({ get self() { return this; } }, {}), 'self', receiver), receiver);
});

console.log('ok');
