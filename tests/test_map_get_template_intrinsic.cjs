const assert = require('assert');
const { spawnSync } = require('child_process');

if (!process.env.ANT_MAP_TEMPLATE_CHILD) {
  const stackChildLocals = Array.from(
    { length: 2500 },
    (_, i) => `v${i}`,
  ).join(',');
  const stackChildSource = [
    `function get(key) { let ${stackChildLocals}; return key; }`,
    'function lookup(target, left, right) {',
    '  return target.get(`${left}-${right}`);',
    '}',
    'if (lookup({ get }, 1, 2) !== "1-2") process.exit(1);',
  ].join('\n');
  const stackChild = spawnSync(
    process.execPath,
    ['--stack-size=8', '-e', stackChildSource],
    { encoding: 'utf8' },
  );
  assert.strictEqual(stackChild.status, 0,
    stackChild.stdout + stackChild.stderr);

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
    /jit: ineligible op (?:CALL|TAIL)_MAP_TEMPLATE/,
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

function lookupOne(map, value) {
  const result = map.get(`id:${value}`);
  return result;
}

function lookupOneBare(map, value) {
  return map.get(`${value}`);
}

function lookupThree(map, first, second, third) {
  return map.get(`head:${first}/${second}/${third}:tail`);
}

const generalizedMap = new Map([
  ['42', 'bare'],
  ['id:42', 'one'],
  ['head:1/2/3:tail', 'three'],
]);
for (let i = 0; i < 500; i++) {
  assert.strictEqual(lookupOneBare(generalizedMap, 42), 'bare');
  assert.strictEqual(lookupOne(generalizedMap, 42), 'one');
  assert.strictEqual(lookupThree(generalizedMap, 1, 2, 3), 'three');
}
assert.strictEqual(lookupOne(generalizedMap, 7), undefined);
assert.strictEqual(lookupThree(generalizedMap, 3, 2, 1), undefined);

function hasOne(map, value) {
  const result = map.has(`id:${value}`);
  return result;
}

function hasTwo(map, left, right) {
  return map.has(`${left}-${right}`);
}

function hasThree(map, first, second, third) {
  return map.has(`head:${first}/${second}/${third}:tail`);
}

for (let i = 0; i < 500; i++) {
  assert.strictEqual(hasOne(generalizedMap, 42), true);
  assert.strictEqual(hasTwo(map, 0, 0), true);
  assert.strictEqual(hasThree(generalizedMap, 1, 2, 3), true);
}
assert.strictEqual(hasOne(generalizedMap, 7), false);
assert.strictEqual(hasTwo(map, 123, 456), false);
assert.strictEqual(hasThree(generalizedMap, 3, 2, 1), false);

function lookupFour(map, first, second, third, fourth) {
  return map.get(`${first}-${second}-${third}-${fourth}`);
}
const fourMap = new Map([['1-2-3-4', 'four']]);
assert.strictEqual(lookupFour(fourMap, 1, 2, 3, 4), 'four');

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

let secondFactory;
let thirdFactory;
function customLookupThree(first) {
  const result = receiver.get(
    `head:${first}/${secondFactory()}/${thirdFactory()}:tail`,
  );
  return result;
}
for (let i = 0; i < 500; i++) {
  secondFactory = () => 'second';
  thirdFactory = () => 'third';
  assert.strictEqual(
    customLookupThree('first'),
    'head:first/second/third:tail',
  );
}

trace = [];
secondFactory = () => {
  trace.push('second-eval');
  return {
    [Symbol.toPrimitive](hint) {
      trace.push(`second:${hint}`);
      return 'second-object';
    },
  };
};
thirdFactory = () => {
  trace.push('third-eval');
  return {
    [Symbol.toPrimitive](hint) {
      trace.push(`third:${hint}`);
      return 'third-object';
    },
  };
};
assert.strictEqual(
  customLookupThree(leftObject),
  'head:left-object/second-object/third-object:tail',
);
assert.deepStrictEqual(trace, [
  'get',
  'left:string',
  'second-eval',
  'second:string',
  'third-eval',
  'third:string',
  'call:head:left-object/second-object/third-object:tail',
]);

map.get = function (key) {
  return `override:${key}`;
};
assert.strictEqual(lookup(map, 7, 8), 'override:7-8');

const hasOverride = new Map();
hasOverride.has = function (key) {
  assert.strictEqual(this, hasOverride);
  return `override:${key}`;
};
assert.strictEqual(hasThree(hasOverride, 7, 8, 9),
  'override:head:7/8/9:tail');

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

function optionalTailHas(target, left, right) {
  return target?.map.has(`${left}-${right}`);
}
assert.strictEqual(optionalTailHas(null, 'left', 'right'), undefined);
assert.strictEqual(optionalTailHas(undefined, 'left', 'right'), undefined);
assert.strictEqual(
  optionalTailHas({ map: optionalMap }, 'left', 'right'),
  true,
);

let finallyCleanupCalls = 0;
function lookupWithFinally(target, left, right) {
  try {
    return target.get(`${left}-${right}`);
  } finally {
    finallyCleanupCalls++;
  }
}
assert.strictEqual(lookupWithFinally(optionalMap, 'left', 'right'),
  'optional-value');
assert.strictEqual(finallyCleanupCalls, 1);

let disposeCalls = 0;
function lookupWithUsing(target, left, right) {
  using resource = {
    [Symbol.dispose]() {
      disposeCalls++;
    },
  };
  return target.get(`${left}-${right}`);
}
assert.strictEqual(lookupWithUsing(optionalMap, 'left', 'right'),
  'optional-value');
assert.strictEqual(disposeCalls, 1);

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

console.log('Map template intrinsic tests passed');
