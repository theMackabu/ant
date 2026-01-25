console.log('=== GC Test with Large Objects ===\n');

function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

let alloc1 = Ant.alloc();
console.log('Initial:');
console.log('  heapSize:', fmt(alloc1.heapSize));
console.log('  totalBytes:', fmt(alloc1.totalBytes));
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

let alloc2 = Ant.alloc();
console.log('After allocating 5MB:');
console.log('  heapSize:', fmt(alloc2.heapSize));
console.log('  totalBytes:', fmt(alloc2.totalBytes));
console.log('');

// Free the objects
largeObjects = null;

let gc1 = Ant.gc();
console.log('GC 1: freed:', fmt(gc1.freed), 'arenaFreed:', fmt(gc1.arenaFreed));
let gc2 = Ant.gc();
console.log('GC 2: freed:', fmt(gc2.freed), 'arenaFreed:', fmt(gc2.arenaFreed));
let gc3 = Ant.gc();
console.log('GC 3: freed:', fmt(gc3.freed), 'arenaFreed:', fmt(gc3.arenaFreed));

let alloc3 = Ant.alloc();
console.log('After 3x GC:');
console.log('  heapSize:', fmt(alloc3.heapSize));
console.log('  totalBytes:', fmt(alloc3.totalBytes));
console.log('');

// Allocate again - if GC worked, heap shouldn't grow much
let moreObjects = [];
for (let i = 0; i < 5; i++) {
  let arr = new Array(1024 * 128);
  for (let j = 0; j < arr.length; j += 1024) {
    arr[j] = i;
  }
  moreObjects.push(arr);
}

let alloc4 = Ant.alloc();
console.log('After allocating another 5MB:');
console.log('  heapSize:', fmt(alloc4.heapSize));
console.log('  totalBytes:', fmt(alloc4.totalBytes));
console.log('  heap increase from GC point:', fmt(alloc4.heapSize - alloc3.heapSize));
console.log('');

console.log('If heapSize stayed same, GC reclaimed memory for reuse!');
console.log('=== Test Complete ===');
