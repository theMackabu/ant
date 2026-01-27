console.log('=== Comprehensive GC Test ===');
console.log('Starting...\n');

function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

let failures = 0;
function assert(condition, msg) {
  if (!condition) {
    console.log('FAIL:', msg);
    failures = failures + 1;
  }
}

// Test 1: Basic objects and arrays survive GC
console.log('Test 1: Objects and Arrays');
let obj1 = { name: 'test', value: 42, nested: { deep: 'value' } };
let arr1 = [1, 2, 3, { inner: 'object' }, 'string'];
Ant.gc();
assert(obj1.name === 'test', 'obj1.name should be "test"');
assert(obj1.nested.deep === 'value', 'nested object should survive');
assert(arr1.length === 5, 'array length should be 5');
assert(arr1[3].inner === 'object', 'array nested object should survive');
console.log('  Objects and arrays: OK\n');

// Test 2: Map survives GC
console.log('Test 2: Map');
let map = new Map();
map.set('key1', { data: 'value1' });
map.set('key2', [1, 2, 3]);
map.set('key3', 'simple string');
Ant.gc();
assert(map.get('key1').data === 'value1', 'map value object should survive');
assert(map.get('key2')[1] === 2, 'map value array should survive');
assert(map.get('key3') === 'simple string', 'map value string should survive');
assert(map.size() === 3, 'map size should be 3');
console.log('  Map: OK\n');

// Test 3: Set survives GC
console.log('Test 3: Set');
let set = new Set();
let setObj = { id: 123 };
set.add('value1');
set.add(42);
set.add(setObj);
Ant.gc();
assert(set.has('value1'), 'set should have "value1"');
assert(set.has(42), 'set should have 42');
assert(set.size() === 3, 'set size should be 3');
console.log('  Set: OK\n');

// Test 4: Property descriptors (getters/setters) survive GC
console.log('Test 4: Property Descriptors');
let descObj = {};
let _hidden = 'initial';
Object.defineProperty(descObj, 'prop', {
  get: function () {
    return _hidden;
  },
  set: function (v) {
    _hidden = v;
  },
  enumerable: true,
  configurable: true
});
descObj.prop = 'updated';
Ant.gc();
assert(descObj.prop === 'updated', 'getter should return updated value');
descObj.prop = 'after gc';
assert(descObj.prop === 'after gc', 'setter should work after GC');
console.log('  Property descriptors: OK\n');

// Test 5: Promises survive GC
console.log('Test 5: Promises');
let promiseResolved = false;
let promiseValue = null;
let p = new Promise(resolve => {
  resolve({ result: 'success' });
});
p.then(val => {
  promiseResolved = true;
  promiseValue = val;
});
Ant.gc();
console.log('  Waiting for promise...');

// Test 6: Proxy survives GC
console.log('Test 6: Proxy');
let proxyTarget = { x: 10, y: 20 };
let proxyHandler = {
  get: function (target, prop) {
    if (prop === 'sum') return target.x + target.y;
    return target[prop];
  }
};
let proxy = new Proxy(proxyTarget, proxyHandler);
Ant.gc();
assert(proxy.x === 10, 'proxy.x should be 10');
assert(proxy.y === 20, 'proxy.y should be 20');
assert(proxy.sum === 30, 'proxy.sum should be 30');
console.log('  Proxy: OK\n');

// Test 7: Closures survive GC
console.log('Test 7: Closures');
function makeCounter() {
  let count = 0;
  return {
    inc: function () {
      count = count + 1;
      return count;
    },
    get: function () {
      return count;
    }
  };
}
let counter = makeCounter();
counter.inc();
counter.inc();
Ant.gc();
assert(counter.get() === 2, 'closure should preserve count');
counter.inc();
assert(counter.get() === 3, 'closure should work after GC');
console.log('  Closures: OK\n');

// Test 8: Multiple GC cycles
console.log('Test 8: Multiple GC Cycles');
let cycleData = { iteration: 0 };
for (let i = 0; i < 5; i = i + 1) {
  cycleData.iteration = i;
  cycleData['data' + i] = { value: i * 10 };
  Ant.gc();
}
assert(cycleData.iteration === 4, 'iteration should be 4');
assert(cycleData.data3.value === 30, 'data3.value should be 30');
console.log('  Multiple cycles: OK\n');

// Test 9: Large allocations + GC
console.log('Test 9: Large Allocations');
let largeArr = [];
for (let i = 0; i < 1000; i = i + 1) {
  largeArr.push({ index: i, data: 'item ' + i });
}
let statsBefore = Ant.stats();
Ant.gc();
let statsAfter = Ant.stats();
assert(largeArr.length === 1000, 'large array should have 1000 elements');
assert(largeArr[500].index === 500, 'element 500 should be correct');
assert(largeArr[999].data === 'item 999', 'last element should be correct');
console.log('  Large allocations: OK');
console.log('  arenaUsed before:', fmt(statsBefore.arenaUsed), 'after:', fmt(statsAfter.arenaUsed), '\n');

// Test 10: Nested structures
console.log('Test 10: Nested Structures');
let nested = {
  level1: {
    level2: {
      level3: {
        level4: {
          value: 'deep'
        }
      }
    }
  }
};
let nestedArr = [[[[['innermost']]]]];
Ant.gc();
assert(nested.level1.level2.level3.level4.value === 'deep', 'deep nesting should survive');
assert(nestedArr[0][0][0][0][0] === 'innermost', 'nested array should survive');
console.log('  Nested structures: OK\n');

// Summary
console.log('=== Test Summary ===');
if (failures === 0) {
  console.log('All tests passed!');
} else {
  console.log('Failures:', failures);
}

// Test async separately (needs event loop)
console.log('\n=== Async GC Test ===');
async function testAsyncGC() {
  let asyncData = { value: 'before await' };

  await new Promise(resolve => setTimeout(resolve, 10));

  // GC inside coroutine (should skip compaction)
  Ant.gc();

  asyncData.value = 'after first await';

  await new Promise(resolve => setTimeout(resolve, 10));

  assert(asyncData.value === 'after first await', 'async data should survive await');

  console.log('  Async GC: OK');
  return 'async complete';
}

testAsyncGC().then(result => {
  console.log('  Result:', result);
  console.log('\n=== All Tests Complete ===');
});
