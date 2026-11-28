const assert = require('node:assert/strict');

function sharedShapeIndependence() {
  const src = { a: 1, b: { c: 2 }, d: 'x' };
  const copy = structuredClone(src);
  assert.deepEqual(copy, src);
  assert.notEqual(copy.b, src.b);

  copy.added = true;
  copy.a = 10;
  delete copy.d;
  assert.deepEqual(Object.keys(src), ['a', 'b', 'd']);
  assert.equal(src.a, 1);
  assert.equal(src.added, undefined);

  src.b.c = 99;
  src.e = 'later';
  assert.equal(copy.b.c, 2);
  assert.deepEqual(Object.keys(copy), ['a', 'b', 'added']);

  const wide = Object.fromEntries(Array.from({ length: 40 }, (_, i) => ['k' + i, { i }]));
  const wideCopy = structuredClone(wide);
  assert.deepEqual(wideCopy, wide);
  for (let i = 0; i < 40; i++) assert.notEqual(wideCopy['k' + i], wide['k' + i]);
}

function identityAndCycles() {
  const shared = { v: 1 };
  const root = { a: shared, b: shared, list: [shared, { up: null }] };
  root.list[1].up = root;
  root.self = root;

  const copy = structuredClone(root);
  assert.equal(copy.a, copy.b);
  assert.equal(copy.list[0], copy.a);
  assert.equal(copy.self, copy);
  assert.equal(copy.list[1].up, copy);
  assert.notEqual(copy.a, shared);
}

function mutationDuringClone() {
  const o = {
    a: 1,
    b: { get g() { delete o.c; o.d = 9; o.e = 'changed'; o.f = 'added'; return 2; } },
    c: 3,
    d: 4,
    e: 'orig',
  };
  assert.deepEqual(structuredClone(o), { a: 1, b: { g: 2 }, d: 9, e: 'changed' });

  const q = {
    a: { get g() { Object.defineProperty(q, 'z', { get() { return 'acc'; }, enumerable: true }); return 1; } },
    z: 'data',
  };
  assert.deepEqual(structuredClone(q), { a: { g: 1 }, z: 'acc' });

  const thrower = { a: { get x() { throw new Error('boom'); } }, b: 1 };
  assert.throws(() => structuredClone(thrower), { message: 'boom' });

  const later = { a: { get g() { delete later.b; throw new Error('after delete'); } }, b: 1 };
  assert.throws(() => structuredClone(later), { message: 'after delete' });

  const wide = { first: { get g() { for (let i = 1; i < 60; i++) if (i !== 30) delete wide['k' + i]; return 0; } } };
  for (let i = 1; i < 60; i++) wide['k' + i] = { i };
  const wideCopy = structuredClone(wide);
  assert.deepEqual(Object.keys(wideCopy), ['first', 'k30']);
  assert.deepEqual(wideCopy.k30, { i: 30 });

  const selfAdd = { a: 1, get b() { selfAdd.c = 'added'; return 2; } };
  assert.deepEqual(structuredClone(selfAdd), { a: 1, b: 2 });

  const selfDelete = { get a() { delete selfDelete.b; return 1; }, b: 2, c: 3 };
  assert.deepEqual(structuredClone(selfDelete), { a: 1, c: 3 });

  class Holder { constructor() { this.x = 1; this.y = { get g() { delete holder.z; holder.w = 1; return 0; } }; this.z = 2; } }
  const holder = new Holder();
  assert.deepEqual(structuredClone(holder), { x: 1, y: { g: 0 } });
}

function arrayElementsChangedDuringClone() {
  const holes = value => [...Array(value.length).keys()].filter(i => !(i in value));

  const packed = [{ get g() { packed.pop(); delete packed[1]; return 0; } }, 'b', 'c', 'd'];
  const packedCopy = structuredClone(packed);
  assert.equal(packedCopy.length, 4);
  assert.deepEqual(holes(packedCopy), [1, 3]);
  assert.equal(packedCopy[2], 'c');

  const shrink = [{ get g() { shrink.length = 1; return 0; } }, 2, 3];
  const shrinkCopy = structuredClone(shrink);
  assert.equal(shrinkCopy.length, 3);
  assert.deepEqual(holes(shrinkCopy), [1, 2]);

  const grow = [{ get g() { grow.push('x'); return 0; } }, 2];
  assert.deepEqual(structuredClone(grow), [{ g: 0 }, 2]);

  const sparse = [];
  sparse[0] = { get g() { delete sparse[5]; sparse[7] = 'new'; return 0; } };
  sparse[5] = 'five';
  sparse[6] = 'six';
  const sparseCopy = structuredClone(sparse);
  assert.equal(sparseCopy.length, 7);
  assert.deepEqual(holes(sparseCopy), [1, 2, 3, 4, 5]);
  assert.equal(sparseCopy[6], 'six');
}

function nonPlainObjectsKeepTheirPath() {
  class Point { constructor() { this.x = 1; } get y() { return 2; } }
  const point = structuredClone(new Point());
  assert.equal(Object.getPrototypeOf(point), Object.prototype);
  assert.deepEqual(point, { x: 1 });

  const bare = structuredClone(Object.assign(Object.create(null), { a: 1 }));
  assert.equal(Object.getPrototypeOf(bare), Object.prototype);

  const frozen = structuredClone(Object.freeze({ a: 1 }));
  assert.equal(Object.isFrozen(frozen), false);
  frozen.b = 2;
  assert.equal(frozen.b, 2);

  const readonly = structuredClone(Object.defineProperty({}, 'r', { value: 1, enumerable: true }));
  assert.deepEqual(Object.getOwnPropertyDescriptor(readonly, 'r'), { value: 1, writable: true, enumerable: true, configurable: true });

  const date = new Date(7);
  Object.setPrototypeOf(date, Object.prototype);
  const dateCopy = structuredClone(date);
  assert.equal(Object.prototype.toString.call(dateCopy), '[object Date]');
  assert.equal(Date.prototype.getTime.call(dateCopy), 7);

  assert.throws(() => structuredClone({ s: Symbol('x') }), { name: 'DataCloneError' });
  assert.throws(() => structuredClone({ f() {} }), { name: 'DataCloneError' });
}

function packedArrays() {
  const inner = { v: 1 };
  const arr = [1, 'two', inner, [3, inner], null, 4n];
  const copy = structuredClone(arr);
  assert.deepEqual(copy, arr);
  assert.equal(copy[2], copy[3][1]);
  assert.notEqual(copy[2], inner);
  copy.push('more');
  assert.equal(arr.length, 6);

  const holes = structuredClone([1, , 3]);
  assert.equal(holes.length, 3);
  assert.equal(1 in holes, false);

  const empty = structuredClone([]);
  assert.deepEqual(empty, []);
  empty.push(1);
  assert.deepEqual(empty, [1]);
}

function transferWithPrimitiveRoot() {
  const buffer = new ArrayBuffer(8);
  assert.equal(structuredClone(5, { transfer: [buffer] }), 5);
  assert.equal(buffer.byteLength, 0);

  const moved = new ArrayBuffer(4);
  const result = structuredClone({ moved }, { transfer: [moved] });
  assert.equal(result.moved.byteLength, 4);
  assert.equal(moved.byteLength, 0);
}

function survivesCollectionsMidClone() {
  const items = [];
  for (let i = 0; i < 300; i++) items.push({ id: i, nested: { tag: 'n' + i, list: [i, { deep: i }] } });
  items.push({
    get churn() {
      for (let r = 0; r < 8; r++) { let keep = []; for (let i = 0; i < 50000; i++) keep.push({ i }); keep = null; }
      return 'done';
    },
  });
  items.push({ after: { id: 'last' } });

  const copy = structuredClone(items);
  assert.equal(copy.length, 302);
  for (let i = 0; i < 300; i++) {
    assert.equal(copy[i].id, i);
    assert.equal(copy[i].nested.tag, 'n' + i);
    assert.equal(copy[i].nested.list[1].deep, i);
  }
  assert.equal(copy[300].churn, 'done');
  assert.equal(copy[301].after.id, 'last');
}

sharedShapeIndependence();
identityAndCycles();
mutationDuringClone();
nonPlainObjectsKeepTheirPath();
arrayElementsChangedDuringClone();
packedArrays();
transferWithPrimitiveRoot();
survivesCollectionsMidClone();
console.log('ok');
