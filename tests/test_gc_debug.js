console.log('=== Debug GC Test ===\n');

// Test 1
console.log('Test 1: Objects');
let obj1 = { name: 'test', value: 42, nested: { deep: 'value' } };
Ant.gc();
console.log('  GC requested');
console.log('  OK\n');

// Test 2
console.log('Test 2: Map');
let map = new Map();
map.set('key1', { data: 'value1' });
map.set('key2', [1, 2, 3]);
Ant.gc();
console.log('  GC requested');
console.log('  get key1:', map.get('key1'));
console.log('  OK\n');

// Test 3
console.log('Test 3: Set');
let set = new Set();
set.add('value1');
set.add(42);
Ant.gc();
console.log('  GC requested');
console.log('  OK\n');

// Test 4
console.log('Test 4: Property Descriptors');
let descObj = {};
let _hidden = 'initial';
Object.defineProperty(descObj, 'prop', {
  get: function() { return _hidden; },
  set: function(v) { _hidden = v; },
  enumerable: true,
  configurable: true
});
descObj.prop = 'updated';
Ant.gc();
console.log('  GC requested');
console.log('  prop:', descObj.prop);
console.log('  OK\n');

// Test 5
console.log('Test 5: Promises');
let p = new Promise((resolve) => {
  resolve({ result: 'success' });
});
Ant.gc();
console.log('  GC requested');
console.log('  OK\n');

// Test 6
console.log('Test 6: Proxy');
let proxyTarget = { x: 10, y: 20 };
let proxyHandler = {
  get: function(target, prop) {
    if (prop === 'sum') return target.x + target.y;
    return target[prop];
  }
};
let proxy = new Proxy(proxyTarget, proxyHandler);
Ant.gc();
console.log('  GC requested');
console.log('  proxy.x:', proxy.x);
console.log('  OK\n');

// Test 7
console.log('Test 7: Closures');
function makeCounter() {
  let count = 0;
  return {
    inc: function() { count = count + 1; return count; },
    get: function() { return count; }
  };
}
let counter = makeCounter();
counter.inc();
Ant.gc();
console.log('  GC requested');
console.log('  count:', counter.get());
console.log('  OK\n');

console.log('=== All tests passed ===');
