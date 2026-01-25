// GC Benchmark - tests forwarding table and compaction performance

function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

function bench(name, fn, iterations = 1) {
  // Warmup
  fn();
  Ant.gc();

  let totalTime = 0;
  let totalFreed = 0;

  for (let iter = 0; iter < iterations; iter++) {
    fn(); // Create garbage

    const t0 = Date.now();
    const result = Ant.gc();
    const elapsed = Date.now() - t0;

    totalTime += elapsed;
    totalFreed += result.arenaFreed;
  }

  console.log(`${name}:`);
  console.log(`  Time: ${totalTime}ms (${iterations} iterations, avg ${(totalTime / iterations).toFixed(2)}ms)`);
  console.log(`  Freed: ${fmt(totalFreed)} total`);
  console.log('');
}

// Test 1: Many small objects (stresses forwarding table with many entries)
function manySmallObjects() {
  const objects = [];
  for (let i = 0; i < 1000; i++) {
    objects.push({ id: i, value: i * 2 });
  }
  // Let half become garbage
  objects.length = 5000;
}

// Test 2: Deep object graph (stresses recursive copying)
function deepObjectGraph() {
  let obj = { value: 0 };
  for (let i = 1; i < 1000; i++) {
    obj = { value: i, child: obj };
  }
  return obj;
}

// Test 3: Wide object (many properties)
function wideObject() {
  const obj = {};
  for (let i = 0; i < 1000; i++) {
    obj['prop' + i] = i;
  }
  return obj;
}

// Test 4: Array of strings (tests string copying)
function stringArray() {
  const arr = [];
  for (let i = 0; i < 5000; i++) {
    arr.push('string_value_' + i + '_with_some_extra_content');
  }
  arr.length = 2500; // Half become garbage
}

// Test 5: Mixed workload
function mixedWorkload() {
  const data = {
    objects: [],
    strings: [],
    nested: null
  };

  for (let i = 0; i < 2000; i++) {
    data.objects.push({ x: i, y: i * 2, name: 'obj' + i });
    data.strings.push('item_' + i);
  }

  // Create some nested structure
  let nested = { level: 0 };
  for (let i = 1; i < 100; i++) {
    nested = { level: i, prev: nested, data: { a: i, b: i * 2 } };
  }
  data.nested = nested;

  // Make some garbage
  data.objects.length = 1000;
  data.strings.length = 1000;
}

// Test 6: Repeated GC cycles (tests table reuse efficiency)
function repeatedGcCycles() {
  for (let cycle = 0; cycle < 10; cycle++) {
    const temp = [];
    for (let i = 0; i < 1000; i++) {
      temp.push({ cycle, i, data: 'x'.repeat(50) });
    }
  }
}

console.log('=== GC Benchmark ===\n');

let initial = Ant.alloc();
console.log('Initial state:');
console.log('  heapSize:', fmt(initial.heapSize));
console.log('  usedBytes:', fmt(initial.usedBytes));
console.log('  totalBytes:', fmt(initial.totalBytes));
console.log('');

bench('Many small objects (10k objects, 5k garbage)', manySmallObjects, 5);
bench('Deep object graph (1000 levels)', deepObjectGraph, 5);
bench('Wide object (1000 properties)', wideObject, 5);
bench('String array (5000 strings, 2500 garbage)', stringArray, 5);
bench('Mixed workload', mixedWorkload, 5);
bench('Repeated GC cycles (10 cycles x 1000 objects)', repeatedGcCycles, 3);

let final = Ant.alloc();
console.log('Final state:');
console.log('  heapSize:', fmt(final.heapSize));
console.log('  usedBytes:', fmt(final.usedBytes));
console.log('  totalBytes:', fmt(final.totalBytes));
