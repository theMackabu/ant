console.log('=== Testing Ant.alloc() (bdwgc) ===');
let alloc1 = Ant.alloc();
console.log('Initial allocation:');
console.log('  heapSize:', alloc1.heapSize);
console.log('  usedBytes:', alloc1.usedBytes);
console.log('  freeBytes:', alloc1.freeBytes);
console.log('  totalBytes:', alloc1.totalBytes);

console.log('\n=== Creating objects to allocate memory ===');
let arr = [];
for (let i = 0; i < 100; i = i + 1) {
  arr.push({ value: i, name: 'item' + i });
}
console.log('Created array with 100 objects');

let alloc2 = Ant.alloc();
console.log('After allocation:');
console.log('  heapSize:', alloc2.heapSize);
console.log('  usedBytes:', alloc2.usedBytes);
console.log('  totalBytes:', alloc2.totalBytes);
console.log('  totalBytes increase:', alloc2.totalBytes - alloc1.totalBytes);

console.log('\n=== Testing Ant.stats() ===');
let stats1 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', stats1.arenaUsed);
console.log('  arenaLwm:', stats1.arenaLwm);
console.log('  cstack:', stats1.cstack);
console.log('  gcHeapSize:', stats1.gcHeapSize);
console.log('  gcUsedBytes:', stats1.gcUsedBytes);
console.log('  gcFreeBytes:', stats1.gcFreeBytes);

console.log('\n=== Testing Ant.gc() ===');
arr = null;

let gcResult = Ant.gc();
console.log('GC result:');
console.log('  heapBefore:', gcResult.heapBefore);
console.log('  heapAfter:', gcResult.heapAfter);
console.log('  usedBefore:', gcResult.usedBefore);
console.log('  usedAfter:', gcResult.usedAfter);
console.log('  freed:', gcResult.freed);
console.log('  arenaBefore:', gcResult.arenaBefore);
console.log('  arenaAfter:', gcResult.arenaAfter);
console.log('  arenaFreed:', gcResult.arenaFreed);

console.log('\n=== Testing Ant.stats() ===');
let stats2 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', stats2.arenaUsed);
console.log('  arenaLwm:', stats2.arenaLwm);
console.log('  cstack:', stats2.cstack);
console.log('  gcHeapSize:', stats2.gcHeapSize);
console.log('  gcUsedBytes:', stats2.gcUsedBytes);
console.log('  gcFreeBytes:', stats2.gcFreeBytes);

console.log('\n=== Verifying memory after GC ===');
let alloc3 = Ant.alloc();
console.log('After GC:');
console.log('  heapSize:', alloc3.heapSize);
console.log('  usedBytes:', alloc3.usedBytes);
console.log('  freeBytes:', alloc3.freeBytes);

console.log('\n=== Testing multiple GC cycles ===');
for (let cycle = 0; cycle < 3; cycle = cycle + 1) {
  console.log('Cycle', cycle + 1);

  let temp = [];
  for (let i = 0; i < 50; i = i + 1) {
    temp.push({ data: 'test data ' + i });
  }
  let beforeGc = Ant.alloc();
  console.log('  Before GC - usedBytes:', beforeGc.usedBytes);

  temp = null;
  let gc = Ant.gc();
  console.log('  After GC - usedAfter:', gc.usedAfter, 'freed:', gc.freed, 'arenaFreed:', gc.arenaFreed);
}

console.log('\n=== Testing Ant.stats() ===');
let stats3 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', stats3.arenaUsed);
console.log('  arenaLwm:', stats3.arenaLwm);
console.log('  cstack:', stats3.cstack);
console.log('  gcHeapSize:', stats3.gcHeapSize);
console.log('  gcUsedBytes:', stats3.gcUsedBytes);
console.log('  gcFreeBytes:', stats3.gcFreeBytes);

console.log('\n=== Testing stats consistency ===');
let statsA = Ant.stats();
let allocA = Ant.alloc();
console.log('Stats and alloc should match:');
console.log('  stats.gcUsedBytes:', statsA.gcUsedBytes);
console.log('  alloc.usedBytes:', allocA.usedBytes);
console.log('  match:', statsA.gcUsedBytes === allocA.usedBytes);

console.log('\n=== Test complete ===');
