const assert = require('assert');
const { spawnSync } = require('child_process');

if (!process.env.ANT_MAP_TEMPLATE_CHILD) {
  const child = spawnSync(process.execPath, [__filename], {
    encoding: 'utf8',
    env: {
      ...process.env,
      ANT_DEBUG: 'dump/vm:op-warn',
      ANT_MAP_TEMPLATE_CHILD: '1',
    },
  });
  assert.strictEqual(child.status, 0, child.stdout + child.stderr);
  assert.doesNotMatch(
    child.stderr,
    /jit: ineligible op (?:CALL|TAIL)_MAP_GET_TEMPLATE2/,
  );
  process.stdout.write(child.stdout);
  process.exit(0);
}

function lookup(map, left, right) {
  return map.get(`${left}-${right}`);
}

const numericCases = [
  [0, 0],
  [-0, 2],
  [-17, 91],
  [NaN, Infinity],
  [-Infinity, 1e21],
  [1e-7, 1.25],
];
const map = new Map();
for (let i = 0; i < numericCases.length; i++) {
  const [left, right] = numericCases[i];
  map.set(`${left}-${right}`, `value-${i}`);
}

for (let round = 0; round < 500; round++) {
  for (let i = 0; i < numericCases.length; i++) {
    const [left, right] = numericCases[i];
    assert.strictEqual(lookup(map, left, right), `value-${i}`);
  }
}
assert.strictEqual(lookup(map, 123, 456), undefined);

let trace = null;
const stableGet = function (key) {
  if (trace) trace.push(`call:${key}`);
  assert.strictEqual(this, receiver);
  return key;
};
const receiver = {};
Object.defineProperty(receiver, 'get', {
  configurable: true,
  get() {
    if (trace) trace.push('get');
    return stableGet;
  },
});

let rightFactory = () => 'right';
function customLookup(left) {
  return receiver.get(`${left}-${rightFactory()}`);
}
for (let i = 0; i < 500; i++)
  assert.strictEqual(customLookup('left'), 'left-right');

trace = [];
const leftObject = {
  [Symbol.toPrimitive](hint) {
    trace.push(`left:${hint}`);
    return 'left-object';
  },
};
rightFactory = () => {
  trace.push('right-eval');
  return {
    [Symbol.toPrimitive](hint) {
      trace.push(`right:${hint}`);
      return 'right-object';
    },
  };
};
assert.strictEqual(customLookup(leftObject), 'left-object-right-object');
assert.deepStrictEqual(trace, [
  'get',
  'left:string',
  'right-eval',
  'right:string',
  'call:left-object-right-object',
]);

trace = [];
rightFactory = () => {
  trace.push('right-should-not-run');
  return 1;
};
assert.throws(
  () => customLookup(Symbol('left')),
  /Cannot convert a Symbol value to a string/,
);
assert.deepStrictEqual(trace, ['get']);

map.get = function (key) {
  return `override:${key}`;
};
assert.strictEqual(lookup(map, 7, 8), 'override:7-8');

function optionalTailLookup(target, left, right) {
  return target?.map.get(`${left}-${right}`);
}
const optionalMap = new Map([['left-right', 'optional-value']]);
assert.strictEqual(optionalTailLookup(null, 'left', 'right'), undefined);
assert.strictEqual(optionalTailLookup(undefined, 'left', 'right'), undefined);
assert.strictEqual(
  optionalTailLookup({ map: optionalMap }, 'left', 'right'),
  'optional-value',
);

let tailCalls = 0;
function tailLookup(target, left, right) {
  'use strict';
  return target.get(`${left}-${right}`);
}
const tailReceiver = {
  get(key) {
    'use strict';
    tailCalls++;
    if (tailCalls === 20000) return key;
    return tailLookup(this, tailCalls, tailCalls + 1);
  },
};
assert.strictEqual(tailLookup(tailReceiver, 0, 1), '19999-20000');
assert.strictEqual(tailCalls, 20000);

console.log('Map.get template intrinsic tests passed');
