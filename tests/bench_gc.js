function fmt(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(2) + ' KB';
  return (bytes / 1024 / 1024).toFixed(2) + ' MB';
}

function bench(name, fn, iterations = 1) {
  fn();

  const before = Ant.stats().arenaUsed;
  for (let iter = 0; iter < iterations; iter++) fn();
  const afterAlloc = Ant.stats().arenaUsed;

  console.log('  Waiting 5s before GC...');
  Ant.sleep(5);
  Ant.gc();

  console.log(`${name}:`);
  console.log(`  Before: ${fmt(before)}, After alloc: ${fmt(afterAlloc)}`);
  console.log(`  Allocated: ${fmt(afterAlloc - before)} (${iterations} iterations)`);
  console.log('');
}

function manySmallObjects() {
  const objects = [];
  for (let i = 0; i < 1000; i++) {
    objects.push({ id: i, value: i * 2 });
  }
  objects.length = 500;
}

function deepObjectGraph() {
  let obj = { value: 0 };
  for (let i = 1; i < 1000; i++) {
    obj = { value: i, child: obj };
  }
  return obj;
}

function wideObject() {
  const obj = {};
  for (let i = 0; i < 1000; i++) {
    obj['prop' + i] = i;
  }
  return obj;
}

function stringArray() {
  const arr = [];
  for (let i = 0; i < 5000; i++) {
    arr.push('string_value_' + i + '_with_some_extra_content');
  }
  arr.length = 2500;
}

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

  let nested = { level: 0 };
  for (let i = 1; i < 100; i++) {
    nested = { level: i, prev: nested, data: { a: i, b: i * 2 } };
  }
  data.nested = nested;

  data.objects.length = 1000;
  data.strings.length = 1000;
}

console.log('=== GC Benchmark ===\n');

function showStats(label, s) {
  console.log(`${label}:`);
  console.log(`  arenaUsed: ${fmt(s.arenaUsed)}`);
  console.log(`  arenaSize: ${fmt(s.arenaSize)}`);
  console.log(`  external.buffers: ${fmt(s.external.buffers)}`);
  console.log(`  external.code: ${fmt(s.external.code)}`);
  console.log(`  external.collections: ${fmt(s.external.collections)}`);
  console.log(`  external.total: ${fmt(s.external.total)}`);
  console.log(`  cstack: ${fmt(s.cstack)}`);
  console.log(`  rss: ${fmt(s.rss)}`);
  console.log('');
}

showStats('Initial state', Ant.stats());

bench('Many small objects (1k objects)', manySmallObjects, 5);
bench('Deep object graph (1000 levels)', deepObjectGraph, 5);
bench('Wide object (1000 properties)', wideObject, 5);
bench('String array (5000 strings)', stringArray, 5);
bench('Mixed workload', mixedWorkload, 5);

showStats('Final state', Ant.stats());

console.log('Note: GC runs automatically at safe points.');
console.log('Use Ant.gc() as a hint to request collection.');
