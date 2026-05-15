const __gcPerfNow = () => (
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? performance.now()
    : Date.now()
);
const __gcPerfStart = __gcPerfNow();
function __gcPerfLog() {
  console.log(`[perf] runtime: ${(__gcPerfNow() - __gcPerfStart).toFixed(2)}ms`);
}

console.log('=== GC Test with Large Objects ===\n');

function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

let stats1 = Ant.stats();
console.log('Initial:');
console.log('  arenaUsed:', fmt(stats1.arenaUsed));
console.log('  arenaSize:', fmt(stats1.arenaSize));
console.log('  rss:', fmt(stats1.rss));
console.log('');

// Allocate large objects
let largeObjects = [];
for (let i = 0; i < 5; i++) {
  let arr = new Array(1024 * 128);
  for (let j = 0; j < arr.length; j += 1024) {
    arr[j] = i;
  }
  largeObjects.push(arr);
}

let stats2 = Ant.stats();
console.log('After allocating large arrays:');
console.log('  arenaUsed:', fmt(stats2.arenaUsed));
console.log('  arenaSize:', fmt(stats2.arenaSize));
console.log('  rss:', fmt(stats2.rss));
console.log('');

// Free the objects
largeObjects = null;

let stats3 = Ant.stats();
console.log('\nAfter 3x GC:');
console.log('  arenaUsed:', fmt(stats3.arenaUsed));
console.log('  arenaSize:', fmt(stats3.arenaSize));
console.log('  rss:', fmt(stats3.rss));
console.log('');

// Allocate again - if GC worked, arena shouldn't grow much
let moreObjects = [];
for (let i = 0; i < 5; i++) {
  let arr = new Array(1024 * 128);
  for (let j = 0; j < arr.length; j += 1024) {
    arr[j] = i;
  }
  moreObjects.push(arr);
}

let stats4 = Ant.stats();
console.log('After allocating another batch:');
console.log('  arenaUsed:', fmt(stats4.arenaUsed));
console.log('  arenaSize:', fmt(stats4.arenaSize));
console.log('  rss:', fmt(stats4.rss));
console.log('  arenaUsed increase from GC point:', fmt(stats4.arenaUsed - stats3.arenaUsed));
console.log('');

console.log('If arenaSize stayed same, GC reclaimed memory for reuse!');
console.log('=== Test Complete ===');
__gcPerfLog();
