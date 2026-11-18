const assert = require('node:assert');

const N = 8000;

{
  class C0 { m(x) { return x + 1; } get g() { return 1; } set s(x) { this.kept = x; } }
  class C1 extends C0 {}
  class C2 extends C1 {}
  class C3 extends C2 {}
  class C4 extends C3 {}
  class C5 extends C4 {}
  class C6 extends C5 { m(x) { super.s = x; return super.m(x) + super.g + this.kept; } }
  class C7 extends C6 {}
  const deep = new C7();
  const direct = (o, i) => o.m(i) + o.g;
  for (let i = 0; i < N; i++) {
    if (i === N / 4) C3.prototype.m = function (x) { return x + 50; };
    if (i === N / 2) Object.defineProperty(C2.prototype, 'g', { get() { return 7; }, configurable: true });
    if (i === 3 * N / 4) Object.defineProperty(C4.prototype, 's', { set(x) { this.kept = -x; }, configurable: true });
    const m = i < N / 4 ? i + 1 : i + 50, g = i < N / 2 ? 1 : 7, kept = i < 3 * N / 4 ? i : -i;
    assert.strictEqual(direct(deep, i), m + g + kept + g, `deep ${i}`);
  }
}

console.log('ok');
