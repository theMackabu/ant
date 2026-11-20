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

test('String and Array bases expose length and indices', () => {
  class Text extends String { baseLength() { return super.length; } }
  const onProto = { __proto__: String.prototype, read() { return super.length; } };
  const onWrapper = { __proto__: new String('xy'), read() { return [super[0], super[1], super[2], super.length]; } };
  class List extends Array { baseLength() { return super.length; } }
  const list = new List();
  list.push(1, 2);
  for (let i = 0; i < HOT; i++) {
    assert.strictEqual(new Text('abc').baseLength(), 0);
    assert.strictEqual(onProto.read(), 0);
    assert.deepStrictEqual(onWrapper.read(), ['x', 'y', undefined, 2]);
    assert.strictEqual(list.baseLength(), 0);
  }
  assert.strictEqual(Reflect.get(new String('abc'), 'length', {}), 3);
});

console.log('ok');
