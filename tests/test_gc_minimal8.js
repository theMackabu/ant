console.log('=== Minimal Test 8 ===');
console.log('Starting...');

// Simulate what comes before Test 8
console.log('Creating Map...');
let map = new Map();
map.set('key1', { data: 'value1' });
Ant.gc();

let set = new Set();
set.add('value1');
Ant.gc();

let descObj = {};
Object.defineProperty(descObj, 'prop', {
  get: function() { return 'test'; },
  set: function(v) { },
});
Ant.gc();

let proxyTarget = { x: 10 };
let proxy = new Proxy(proxyTarget, { get: function(t, p) { return t[p]; } });
Ant.gc();

function makeCounter() {
  let count = 0;
  return { inc: function() { count = count + 1; return count; } };
}
let counter = makeCounter();
counter.inc();
Ant.gc();

// Now Test 8
console.log('Test 8: Multiple GC Cycles');
let cycleData = { iteration: 0 };
for (let i = 0; i < 5; i = i + 1) {
  console.log('Cycle', i);
  cycleData.iteration = i;
  cycleData['data' + i] = { value: i * 10 };
  Ant.gc();
}
console.log('Done');
