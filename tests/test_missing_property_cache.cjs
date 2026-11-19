const assert = require('node:assert');

const N = 20000;
const marks = (i) => i === N / 2 - 1 || i === N / 2 || i === N - 1;

function track(read, receiver, change) {
  const seen = [];
  for (let i = 0; i < N; i++) {
    if (i === N / 2) change();
    if (marks(i)) seen.push(read(receiver));
  }
  return seen;
}

{
  function target() {}
  assert.deepStrictEqual(track((f) => f.late, target, () => { target.late = 7; }), [undefined, 7, 7]);
}

{
  function target() {}
  const seen = track((f) => f.onFunctionProto, target, () => { Function.prototype.onFunctionProto = 3; });
  delete Function.prototype.onFunctionProto;
  assert.deepStrictEqual(seen, [undefined, 3, 3]);
  assert.strictEqual(target.onFunctionProto, undefined);
}

{
  function target() {}
  const seen = track((f) => f.onObjectProto, target, () => { Object.prototype.onObjectProto = 4; });
  delete Object.prototype.onObjectProto;
  assert.deepStrictEqual(seen, [undefined, 4, 4]);
}

{
  function target() {}
  const seen = track((f) => f.viaGetter, target, () => {
    Object.defineProperty(Function.prototype, 'viaGetter', { get() { return this === target; }, configurable: true });
  });
  delete Function.prototype.viaGetter;
  assert.deepStrictEqual(seen, [undefined, true, true]);
}

{
  function target() {}
  assert.deepStrictEqual(track((f) => f.viaProto, target, () => { Object.setPrototypeOf(target, { viaProto: 5 }); }), [undefined, 5, 5]);
}

{
  class Base {}
  class Derived extends Base {}
  assert.deepStrictEqual(track((f) => f.staticLate, Derived, () => { Base.staticLate = 'base'; }), [undefined, 'base', 'base']);
}

{
  const functions = [];
  for (let i = 0; i < 40; i++) {
    const fn = new Function('return ' + i);
    if (i % 4 === 1) fn.tag = i;
    if (i % 4 === 2) fn.other = 1;
    functions.push(fn);
  }
  const read = (f) => f.tag;
  let sum = 0;
  for (let i = 0; i < N; i++) {
    const value = read(functions[i % 40]);
    sum += value === undefined ? 1 : value;
  }
  assert.strictEqual(sum, (N / 40) * (30 + 1 + 5 + 9 + 13 + 17 + 21 + 25 + 29 + 33 + 37));
}

{
  const receivers = [function () {}, { a: 1 }, Object.create(Function.prototype), [], Math.max, 'text', 5];
  const read = (value) => value.nope;
  let missing = 0;
  for (let i = 0; i < N; i++) missing += read(receivers[i % receivers.length]) === undefined ? 1 : 0;
  assert.strictEqual(missing, N);
}

{
  const object = { a: 1 };
  assert.deepStrictEqual(track((o) => o.late, object, () => { object.late = 7; }), [undefined, 7, 7]);
}

{
  const proto = { a: 1 };
  const object = Object.create(Object.create(proto));
  assert.deepStrictEqual(track((o) => o.late, object, () => { proto.late = 8; }), [undefined, 8, 8]);
}

{
  const object = { a: 1 };
  const seen = track((o) => o.onObjectProtoLate, object, () => { Object.prototype.onObjectProtoLate = 9; });
  delete Object.prototype.onObjectProtoLate;
  assert.deepStrictEqual(seen, [undefined, 9, 9]);
  assert.strictEqual(object.onObjectProtoLate, undefined);
}

{
  const object = Object.create(null);
  object.a = 1;
  assert.deepStrictEqual(track((o) => o.late, object, () => { object.late = 3; }), [undefined, 3, 3]);
}

{
  const object = { a: 1 };
  assert.deepStrictEqual(track((o) => o.via, object, () => { Object.setPrototypeOf(object, { via: 2 }); }), [undefined, 2, 2]);
}

{
  const first = Object.create({ k: 1 });
  const secondProto = { k: 1 };
  const second = Object.create(secondProto);
  const read = (o) => o.late;
  const seen = [];
  for (let i = 0; i < N; i++) {
    if (i === N / 2) secondProto.late = 'second';
    if (marks(i)) seen.push(read(first), read(second));
  }
  assert.deepStrictEqual(seen, [undefined, undefined, undefined, 'second', undefined, 'second']);
}

{
  function Holder() {}
  const object = Object.create(Holder);
  assert.deepStrictEqual(track((o) => o.late, object, () => { Holder.late = 2; }), [undefined, 2, 2]);
}

{
  let traps = 0;
  const object = Object.create(new Proxy({}, { get(target, key) { traps++; return key === 'late' ? 4 : undefined; } }));
  const read = (o) => o.late;
  let sum = 0;
  for (let i = 0; i < N; i++) sum += read(object);
  assert.deepStrictEqual([sum, traps], [4 * N, N]);
}

console.log('ok');
