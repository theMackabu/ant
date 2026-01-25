function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

console.log('=== Testing Ant.alloc() (bdwgc) ===');
let alloc1 = Ant.alloc();
console.log('Initial allocation:');
console.log('  heapSize:', fmt(alloc1.heapSize));
console.log('  usedBytes:', fmt(alloc1.usedBytes));
console.log('  freeBytes:', fmt(alloc1.freeBytes));
console.log('  totalBytes:', fmt(alloc1.totalBytes));

console.log('\n=== Creating objects to allocate memory ===');
let arr = [];
for (let i = 0; i < 100; i = i + 1) {
  arr.push({ value: i, name: 'item' + i });
}
console.log('Created array with 100 objects');

let alloc2 = Ant.alloc();
console.log('After allocation:');
console.log('  heapSize:', fmt(alloc2.heapSize));
console.log('  usedBytes:', fmt(alloc2.usedBytes));
console.log('  totalBytes:', fmt(alloc2.totalBytes));
console.log('  totalBytes increase:', fmt(alloc2.totalBytes - alloc1.totalBytes));

console.log('\n=== Testing Ant.stats() ===');
let stats1 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', fmt(stats1.arenaUsed));
console.log('  cstack:', fmt(stats1.cstack));
console.log('  gcHeapSize:', fmt(stats1.gcHeapSize));
console.log('  gcUsedBytes:', fmt(stats1.gcUsedBytes));
console.log('  gcFreeBytes:', fmt(stats1.gcFreeBytes));

console.log('\n=== Testing Ant.gc() ===');
arr = null;

let gcResult = Ant.gc();
console.log('GC result:');
console.log('  heapBefore:', fmt(gcResult.heapBefore));
console.log('  heapAfter:', fmt(gcResult.heapAfter));
console.log('  usedBefore:', fmt(gcResult.usedBefore));
console.log('  usedAfter:', fmt(gcResult.usedAfter));
console.log('  freed:', fmt(gcResult.freed));
console.log('  arenaBefore:', fmt(gcResult.arenaBefore));
console.log('  arenaAfter:', fmt(gcResult.arenaAfter));
console.log('  arenaFreed:', fmt(gcResult.arenaFreed));

console.log('\n=== Testing Ant.stats() ===');
let stats2 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', fmt(stats2.arenaUsed));
console.log('  cstack:', fmt(stats2.cstack));
console.log('  gcHeapSize:', fmt(stats2.gcHeapSize));
console.log('  gcUsedBytes:', fmt(stats2.gcUsedBytes));
console.log('  gcFreeBytes:', fmt(stats2.gcFreeBytes));

console.log('\n=== Verifying memory after GC ===');
let alloc3 = Ant.alloc();
console.log('After GC:');
console.log('  heapSize:', fmt(alloc3.heapSize));
console.log('  usedBytes:', fmt(alloc3.usedBytes));
console.log('  freeBytes:', fmt(alloc3.freeBytes));

console.log('\n=== Testing multiple GC cycles ===');
for (let cycle = 0; cycle < 3; cycle = cycle + 1) {
  console.log('Cycle', cycle + 1);

  let temp = [];
  for (let i = 0; i < 50; i = i + 1) {
    temp.push({ data: 'test data ' + i });
  }
  let beforeGc = Ant.alloc();
  console.log('  Before GC - usedBytes:', fmt(beforeGc.usedBytes));

  temp = null;
  let gc = Ant.gc();
  console.log('  After GC - usedAfter:', fmt(gc.usedAfter), 'freed:', fmt(gc.freed), 'arenaFreed:', fmt(gc.arenaFreed));
}

console.log('\n=== Testing Ant.stats() ===');
let stats3 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', fmt(stats3.arenaUsed));
console.log('  cstack:', fmt(stats3.cstack));
console.log('  gcHeapSize:', fmt(stats3.gcHeapSize));
console.log('  gcUsedBytes:', fmt(stats3.gcUsedBytes));
console.log('  gcFreeBytes:', fmt(stats3.gcFreeBytes));

console.log('\n=== Testing stats consistency ===');
let statsA = Ant.stats();
let allocA = Ant.alloc();
console.log('Stats and alloc should match:');
console.log('  stats.gcUsedBytes:', fmt(statsA.gcUsedBytes));
console.log('  alloc.usedBytes:', fmt(allocA.usedBytes));
console.log('  match:', statsA.gcUsedBytes === allocA.usedBytes);

console.log('\n=== Test complete ===');
