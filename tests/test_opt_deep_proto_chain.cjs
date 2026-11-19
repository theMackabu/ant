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

{
  class D0 {}
  D0.prototype.x = 7;
  let Leaf = D0;
  const protos = [D0.prototype];
  for (let i = 1; i < 8; i++) {
    Leaf = class extends Leaf {};
    protos.push(Leaf.prototype);
  }
  const SHAPES = 14;
  const objs = [];
  for (let s = 0; s < SHAPES; s++) {
    const o = new Leaf();
    for (let j = 0; j < s; j++) o['p' + j] = j;
    objs.push(o);
  }
  const read = (o) => o.x;
  for (let round = 1; round <= SHAPES; round++)
    for (let i = 0; i < N; i++) assert.strictEqual(read(objs[i % round]), 7, `widen ${round} ${i}`);
  for (let i = 0; i < 4 * N; i++) {
    if (i === N) D0.prototype.x = 11;
    if (i === 2 * N) protos[4].x = 13;
    if (i === 3 * N) objs[3].x = 17;
    const expected = i < N ? 7 : i < 2 * N ? 11 : i >= 3 * N && i % SHAPES === 3 ? 17 : 13;
    assert.strictEqual(read(objs[i % SHAPES]), expected, `wide ${i}`);
  }
}

if (!process.env.ANT_DEEP_PROTO_CHILD) {
  const { spawnSync } = require('node:child_process');
  const child = spawnSync(process.execPath, [__filename], {
    env: { ...process.env, ANT_DEEP_PROTO_CHILD: '1', ANT_DEBUG: 'dump/vm:op-warn' },
    encoding: 'utf8',
  });
  if (child.error) throw child.error;
  assert.strictEqual(child.status, 0, child.stderr);
  assert.ok(!child.stderr.includes('jit: opt rejected'), child.stderr.split('\n').filter((line) => line.includes('opt rejected')).join('\n'));
}

console.log('ok');
