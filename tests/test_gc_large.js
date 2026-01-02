console.log('=== GC Test with Large Objects ===\n');

let alloc1 = Ant.alloc();
console.log('Initial:');
console.log('  heapSize:', alloc1.heapSize);
console.log('  totalBytes:', alloc1.totalBytes);
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
console.log('  heapSize:', alloc2.heapSize);
console.log('  totalBytes:', alloc2.totalBytes);
console.log('');

// Free the objects
largeObjects = null;

let gc1 = Ant.gc();
console.log('GC 1: freed:', gc1.freed, 'arenaFreed:', gc1.arenaFreed);
let gc2 = Ant.gc();
console.log('GC 2: freed:', gc2.freed, 'arenaFreed:', gc2.arenaFreed);
let gc3 = Ant.gc();
console.log('GC 3: freed:', gc3.freed, 'arenaFreed:', gc3.arenaFreed);

let alloc3 = Ant.alloc();
console.log('After 3x GC:');
console.log('  heapSize:', alloc3.heapSize);
console.log('  totalBytes:', alloc3.totalBytes);
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
console.log('  heapSize:', alloc4.heapSize);
console.log('  totalBytes:', alloc4.totalBytes);
console.log('  heap increase from GC point:', alloc4.heapSize - alloc3.heapSize);
console.log('');

console.log('If heapSize stayed same, GC reclaimed memory for reuse!');
console.log('=== Test Complete ===');
