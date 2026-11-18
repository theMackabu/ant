const assert = require('node:assert');

const N = 8000;

{
  const METHOD = 'm', GETTER = 'g', MISSING = 'nope';
  class A { m(x) { return x + 1; } get g() { return this.base; } }
  class Other { m(x) { return x * 2; } get g() { return -this.base; } }
  class B extends A {
    constructor() { super(); this.base = 10; }
    m(x) { return super[METHOD](x) + super[GETTER]; }
    missing() { return super[MISSING]; }
    viaTemplate(x) { return super[`${METHOD}`](x); }
  }
  const b = new B();
  for (let i = 0; i < N; i++) {
    if (i === N / 4) A.prototype.m = function (x) { return x + 100; };
    if (i === N / 2) Object.defineProperty(A.prototype, 'g', { get() { return this.base * 3; }, configurable: true });
    if (i === 3 * N / 4) Object.setPrototypeOf(B.prototype, Other.prototype);
    if (i === 3 * N / 4 + 500) Other.prototype.nope = 'found';
    const expected = i < N / 4 ? i + 1 + 10 : i < N / 2 ? i + 100 + 10 : i < 3 * N / 4 ? i + 100 + 30 : 2 * i - 10;
    assert.strictEqual(b.m(i), expected, `computed ${i}`);
    assert.strictEqual(b.missing(), i < 3 * N / 4 + 500 ? undefined : 'found');
    assert.strictEqual(b.viaTemplate(i), i < N / 4 ? i + 1 : i < 3 * N / 4 ? i + 100 : 2 * i);
  }
}

{
  const parent = { m() { return 'parent'; } };
  const other = { m() { return 'other'; } };
  const KEY = 'm';
  const child = { __proto__: parent, read() { return super[KEY](); } };
  for (let i = 0; i < N; i++) {
    if (i === N / 2) Object.setPrototypeOf(child, other);
    assert.strictEqual(child.read(), i < N / 2 ? 'parent' : 'other');
  }
}

console.log('ok');
