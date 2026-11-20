const assert = require('node:assert');

function F() {}
const swapped = { swapped: true };
let change = false;

class A { constructor(v) { this.v = v; this.seen = Object.getPrototypeOf(this); } }
class D extends A { constructor(v) { if (change) F.prototype = swapped; super(v); this.d = 1; } }
class Mid extends A { constructor(v) { if (change) F.prototype = swapped; super(v); } }
class Top extends Mid { constructor(v) { super(v); this.t = 1; } }

for (let round = 0; round < 20000; round++) {
  const d = Reflect.construct(D, [round]);
  if (d.seen !== D.prototype || d.v !== round) throw new Error('warm D');
  const t = Reflect.construct(Top, [round]);
  if (t.seen !== Top.prototype || t.t !== 1) throw new Error('warm Top');
}

change = true;
const first = Reflect.construct(D, [1], F);
assert.strictEqual(Object.getPrototypeOf(first), swapped);
assert.strictEqual(first.seen, swapped);

F.prototype = {};
const second = Reflect.construct(Top, [2], F);
assert.strictEqual(Object.getPrototypeOf(second), swapped);
assert.strictEqual(second.seen, swapped);

change = false;
for (let round = 0; round < 20000; round++) {
  const d = Reflect.construct(D, [round]);
  if (d.seen !== D.prototype) throw new Error('after D');
}
change = true;
F.prototype = {};
assert.strictEqual(Reflect.construct(D, [3], F).seen, swapped);
assert.strictEqual(Object.getPrototypeOf(new D(4)), D.prototype);

console.log('ok');
