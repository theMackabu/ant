function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

console.log('=== Testing Ant.stats() ===');
let stats1 = Ant.stats();
console.log('Initial:');
console.log('  arenaUsed:', fmt(stats1.arenaUsed));
console.log('  arenaSize:', fmt(stats1.arenaSize));
console.log('  rss:', fmt(stats1.rss));

console.log('\n=== Creating objects to allocate memory ===');
let arr = [];
for (let i = 0; i < 100; i = i + 1) {
  arr.push({ value: i, name: 'item' + i });
}
console.log('Created array with 100 objects');

let stats2 = Ant.stats();
console.log('After allocation:');
console.log('  arenaUsed:', fmt(stats2.arenaUsed));
console.log('  arenaSize:', fmt(stats2.arenaSize));
console.log('  rss:', fmt(stats2.rss));
console.log('  arenaUsed increase:', fmt(stats2.arenaUsed - stats1.arenaUsed));

console.log('\n=== Testing Ant.gc() ===');
arr = null;
Ant.gc();

console.log('\n=== Stats after GC ===');
let stats3 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', fmt(stats3.arenaUsed));
console.log('  arenaSize:', fmt(stats3.arenaSize));
console.log('  rss:', fmt(stats3.rss));

console.log('\n=== Testing multiple GC cycles ===');
for (let cycle = 0; cycle < 3; cycle = cycle + 1) {
  console.log('Cycle', cycle + 1);

  let temp = [];
  for (let i = 0; i < 50; i = i + 1) {
    temp.push({ data: 'test data ' + i });
  }
  let before = Ant.stats();
  console.log('  Before GC - arenaUsed:', fmt(before.arenaUsed));

  temp = null;
  Ant.gc();
  console.log('  After GC - arenaUsed:', fmt(Ant.stats().arenaUsed));
}

console.log('\n=== Final stats ===');
let stats4 = Ant.stats();
console.log('Memory stats:');
console.log('  arenaUsed:', fmt(stats4.arenaUsed));
console.log('  arenaSize:', fmt(stats4.arenaSize));
console.log('  rss:', fmt(stats4.rss));

console.log('\n=== Test complete ===');
