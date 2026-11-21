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
  const log = [];
  class A {
    set v(x) { this.stored = x; return 'ignored'; }
    get v() { return this.stored; }
    set loud(x) { if (x === N - 7) throw new RangeError('loud'); this.stored = -x; }
  }
  class Mid extends A {}
  class Other { set v(x) { this.stored = x * 2; } }
  class B extends Mid {
    put(x) { const result = (super.v = x); return [result, this.stored]; }
    putLoud(x) { super.loud = x; return this.stored; }
  }
  const b = new B();
  const frozen = Object.freeze(new B());
  for (let i = 0; i < N; i++) {
    if (i === N / 4) Object.defineProperty(A.prototype, 'v', { set(x) { this.stored = x + 1000; log.push(x); }, configurable: true });
    if (i === N / 2) Object.defineProperty(Mid.prototype, 'v', { set(x) { this.stored = x + 5000; }, configurable: true });
    if (i === N / 2 + 1000) delete Mid.prototype.v;
    if (i === 3 * N / 4) Object.setPrototypeOf(B.prototype, Other.prototype);
    if (i === 3 * N / 4 + 500) Object.defineProperty(Other.prototype, 'v', { get() { return 1; }, set: undefined, configurable: true });
    if (i === 3 * N / 4 + 1000) delete Other.prototype.v;
    if (i >= 3 * N / 4 + 500 && i < 3 * N / 4 + 1000) {
      assert.throws(() => b.put(i), TypeError);
      continue;
    }
    const stored = i < N / 4 ? i : i < N / 2 ? i + 1000 : i < N / 2 + 1000 ? i + 5000 : i < 3 * N / 4 ? i + 1000 : i < 3 * N / 4 + 500 ? 2 * i : undefined;
    const outcome = b.put(i);
    assert.strictEqual(outcome[0], i);
    if (stored !== undefined) assert.strictEqual(outcome[1], stored, `setter ${i}`);
    else assert.strictEqual(b.v, i, `own ${i}`);
    if (i < 3 * N / 4) {
      if (i === N - 7) assert.throws(() => b.putLoud(i), RangeError);
      else assert.strictEqual(b.putLoud(i), -i);
    }
    if (i < N / 4) assert.throws(() => B.prototype.put.call(frozen, i), TypeError);
  }
  assert.strictEqual(log.length, N / 4 + N / 4 - 1000);
  assert.strictEqual(Object.prototype.hasOwnProperty.call(b, 'v'), true);
}

{
  const parent = { m() { return 'parent'; }, set v(x) { this.seen = `parent:${x}`; } };
  const other = { m() { return 'other'; }, set v(x) { this.seen = `other:${x}`; } };
  const KEY = 'm';
  const child = { __proto__: parent, read() { return super[KEY](); }, write(x) { super.v = x; return this.seen; } };
  for (let i = 0; i < N; i++) {
    if (i === N / 2) Object.setPrototypeOf(child, other);
    assert.strictEqual(child.read(), i < N / 2 ? 'parent' : 'other');
    assert.strictEqual(child.write(i), `${i < N / 2 ? 'parent' : 'other'}:${i}`);
    assert.strictEqual(child.write.call(7, i), `${i < N / 2 ? 'parent' : 'other'}:${i}`);
  }
}

console.log('ok');
