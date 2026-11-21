const assert = require('node:assert');

class P {
  set s(v) { this._s = v * 2; }
}
Object.defineProperty(P.prototype, 'ro', { value: 1 });

class Q extends P {
  run() {
    super.k = 1;
    super[3] = 2;
    assert.ok(Object.hasOwn(this, 'k'));
    assert.strictEqual(this[3], 2);
    assert.ok(!Object.hasOwn(Q.prototype, 'k') && !Object.hasOwn(P.prototype, 'k'));

    super.s = 4;
    assert.strictEqual(this._s, 8);
    assert.ok(!Object.hasOwn(this, 's'));

    this.k = 1;
    assert.ok(Number.isNaN((super.k += 10)));
    assert.ok(Number.isNaN(super.k++));
    super.n ??= 7;
    super.n ??= 8;
    assert.strictEqual(this.n, 8);

    [super.a, super['b']] = [1, 2];
    ({ x: super.c } = { x: 3 });
    assert.deepStrictEqual([this.a, this.b, this.c], [1, 2, 3]);

    const sym = Symbol('q');
    super[sym] = 'sym';
    assert.strictEqual(this[sym], 'sym');
    assert.strictEqual((super.z = 'z'), 'z');
    (() => { super.w = 'arrow'; })();
    assert.strictEqual(this.w, 'arrow');

    assert.throws(() => { super.ro = 2; }, TypeError);
  }

  static st() {
    super.sv = 1;
    return Object.hasOwn(Q, 'sv');
  }
}

new Q().run();
assert.ok(Q.st());

class A extends Array {
  set() {
    super[0] = 'x';
    super.length = 1;
    return [this[0], this.length];
  }
}
const a = new A();
a.push(1, 2, 3);
assert.deepStrictEqual(a.set(), ['x', 1]);

const o = {
  __proto__: { set t(v) { this.tt = v; } },
  m() {
    super.t = 1;
    super.u = 2;
    return [this.tt, Object.hasOwn(this, 'u')];
  },
};
assert.deepStrictEqual(o.m(), [1, true]);

console.log('ok');
