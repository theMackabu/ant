const assert = require('node:assert');

const N = 4000;

function hasKeys(obj) {
  let n = 0;
  try {
    if ('a' in obj) n += 1;
    if ('length' in obj) n += 10;
    if ('toString' in obj) n += 100;
  } finally {
    n += 1000;
  }
  return n;
}

const receivers = [{ a: 1 }, [1, 2], Object.create({ a: 2 }), Object.create(null), function f() {}, new Map()];
const expected = [1101, 1110, 1101, 1000, 1110, 1100];
for (let i = 0; i < N; i++) {
  const k = i % receivers.length;
  assert.strictEqual(hasKeys(receivers[k]), expected[k]);
}

let trapCalls = 0;
const proxy = new Proxy({}, { has(t, key) { trapCalls++; return key === 'a'; } });
for (let i = 0; i < N; i++) assert.strictEqual(hasKeys(proxy), 1001);
assert.strictEqual(trapCalls, N * 3);

for (const bad of [1, 'str', null, undefined, Symbol('s')]) {
  assert.throws(() => hasKeys(bad), TypeError);
}

const sym = Symbol('s');
function hasDynamic(key, obj) { return key in obj; }
const dynamicCases = [[0, [5]], [1, [5]], ['0', [5]], [sym, { [sym]: 1 }], ['x', { x: undefined }], ['y', { x: 1 }], [2.5, { '2.5': 1 }]];
const dynamicExpected = [true, false, true, true, true, false, true];
for (let i = 0; i < N; i++) {
  const k = i % dynamicCases.length;
  assert.strictEqual(hasDynamic(dynamicCases[k][0], dynamicCases[k][1]), dynamicExpected[k]);
}
assert.throws(() => hasDynamic('x', 5), TypeError);

function keysOf(obj) {
  const out = [];
  try {
    for (const key in obj) {
      if (Object.prototype.hasOwnProperty.call(obj, key)) out.push(key);
    }
  } finally {
    out.push('|');
  }
  return out.join(',');
}

const proto = { inherited: 1 };
const child = Object.create(proto);
child.own = 1;
const ownKeysProxy = new Proxy({ x: 1, y: 2 }, { ownKeys(t) { return ['y', 'x']; } });
const cases = [
  [{ b: 1, a: 2, 1: 3 }, '1,b,a,|'],
  [[7, 8], '0,1,|'],
  [child, 'own,|'],
  [null, '|'],
  [undefined, '|'],
  ['ab', '0,1,|'],
  [ownKeysProxy, 'y,x,|'],
];
for (let i = 0; i < N; i++) {
  const [value, keys] = cases[i % cases.length];
  assert.strictEqual(keysOf(value), keys);
}

const hasOwn = Object.prototype.hasOwnProperty;
function countOwn(objs) {
  let n = 0;
  for (let i = 0; i < objs.length; i++) {
    const call = hasOwn.call;
    if (call.call(hasOwn, objs[i], 'k')) n++;
    n += typeof Math.max.name === 'string' ? 0 : 1;
  }
  return n;
}
const objs = [{ k: 1 }, { j: 1 }, Object.create({ k: 1 }), { k: 2, z: 1 }];
for (let i = 0; i < N; i++) assert.strictEqual(countOwn(objs), 2);

function readField(value) { return value.name; }
const fieldReceivers = [{ name: 'obj' }, function named() {}, Math.max, class K {}, 'str'];
const fieldNames = ['obj', 'named', 'max', 'K', undefined];
for (let i = 0; i < N; i++) {
  const k = i % fieldReceivers.length;
  assert.strictEqual(readField(fieldReceivers[k]), fieldNames[k]);
}

console.log('opt in field and for-in ok');
