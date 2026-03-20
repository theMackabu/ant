// bench_prototype_dynamic.js
// Tests whether ant's O(1) instanceof breaks under dynamic mutations.
//
// Key finding from baseline: instanceof is O(1) but isPrototypeOf is O(n).
// This means ant likely stores type/class metadata separately from the
// prototype chain. These tests try to break that.

const ITERATIONS = 2000000;

function buildChain(depth) {
  let chain = [class Base {}];
  for (let i = 1; i < depth; i++) {
    chain.push(class extends chain[i - 1] {});
  }
  return chain;
}

function median(arr) {
  arr.sort((a, b) => a - b);
  const mid = arr.length >> 1;
  return arr.length % 2 ? arr[mid] : (arr[mid - 1] + arr[mid]) / 2;
}

function bench(label, fn, iterations = ITERATIONS) {
  for (let i = 0; i < 1000; i++) fn();

  const samples = [];
  for (let run = 0; run < 5; run++) {
    const start = performance.now();
    for (let i = 0; i < iterations; i++) fn();
    samples.push(performance.now() - start);
  }

  const med = median(samples);
  const ops = ((iterations / med) * 1000).toFixed(2);
  const p95 = samples.sort((a, b) => a - b)[Math.floor(samples.length * 0.95)];
  console.log(
    `  ${label}: median ${med.toFixed(2)}ms (${ops} ops/ms), ` +
      `p95 ${p95.toFixed(2)}ms, min ${Math.min(...samples).toFixed(2)}ms, ` +
      `max ${Math.max(...samples).toFixed(2)}ms result=${iterations}`
  );
  return med;
}

// ============================================================
// Test 1: Baseline — confirm instanceof O(1), isPrototypeOf O(n)
// ============================================================
console.log('=== Test 1: Static chain (baseline) ===');
for (const depth of [8, 24, 40]) {
  const chain = buildChain(depth);
  const Leaf = chain[chain.length - 1];
  const Root = chain[0];
  const obj = new Leaf();

  console.log(`\n--- depth=${depth} ---`);
  bench('instanceof', () => obj instanceof Root);
}

// ============================================================
// Test 2: setPrototypeOf — reparent an object to a different chain
//   Creates two separate chains, then moves an object from one
//   to the other. If ant caches "is instance of Root" at creation
//   time, the result will be wrong after the reparent.
// ============================================================
console.log('\n=== Test 2: setPrototypeOf — reparent to different chain ===');
{
  const chainA = buildChain(24);
  const chainB = buildChain(24);
  const LeafA = chainA[chainA.length - 1];
  const RootA = chainA[0];
  const RootB = chainB[0];

  const obj = new LeafA();

  const beforeA = obj instanceof RootA; // should be true
  const beforeB = obj instanceof RootB; // should be false

  // reparent: point obj's proto to chainB's leaf prototype instead
  Object.setPrototypeOf(obj, chainB[chainB.length - 1].prototype);

  const afterA = obj instanceof RootA; // should be FALSE now
  const afterB = obj instanceof RootB; // should be TRUE now

  console.log(`  before: instanceofA=${beforeA}(expect true), instanceofB=${beforeB}(expect false)`);
  console.log(`  after:  instanceofA=${afterA}(expect false), instanceofB=${afterB}(expect true)`);

  if (afterA !== false || afterB !== true) {
    console.log('  *** CORRECTNESS BUG: instanceof returned wrong result after setPrototypeOf ***');
  } else {
    console.log('  correctness OK');
  }

  bench('instanceof reparented obj vs RootB', () => obj instanceof RootB);
  bench('instanceof reparented obj vs RootA', () => obj instanceof RootA);
}

// ============================================================
// Test 3: setPrototypeOf on a mid-chain prototype
//   Detach the middle of a chain and reattach to a different root.
//   All objects below the splice point should stop being instanceof
//   the original root.
// ============================================================
console.log('\n=== Test 3: Splice mid-chain prototype to new root ===');
{
  const depth = 24;
  const chain = buildChain(depth);
  const Leaf = chain[chain.length - 1];
  const Root = chain[0];
  const obj = new Leaf();

  const midIdx = Math.floor(depth / 2);
  const before = obj instanceof Root; // true

  // create a totally separate root
  class NewRoot {}

  // detach mid-chain from old chain, attach to NewRoot
  Object.setPrototypeOf(chain[midIdx].prototype, NewRoot.prototype);

  const afterOldRoot = obj instanceof Root; // should be FALSE
  const afterNewRoot = obj instanceof NewRoot; // should be TRUE

  console.log(`  before: instanceof Root=${before}(expect true)`);
  console.log(`  after:  instanceof Root=${afterOldRoot}(expect false), instanceof NewRoot=${afterNewRoot}(expect true)`);

  if (afterOldRoot !== false || afterNewRoot !== true) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }

  bench('instanceof (post-splice, vs NewRoot)', () => obj instanceof NewRoot);
  bench('instanceof (post-splice, vs old Root)', () => obj instanceof Root);
}

// ============================================================
// Test 4: Constructor.prototype reassignment
//   Replace .prototype entirely. Old objects should no longer
//   be instanceof, new objects should be.
// ============================================================
console.log('\n=== Test 4: Constructor.prototype reassignment ===');
{
  class Foo {}
  const obj1 = new Foo();
  const before = obj1 instanceof Foo; // true

  // nuke it
  Foo.prototype = { constructor: Foo };

  const obj1After = obj1 instanceof Foo; // should be FALSE
  const obj2 = new Foo();
  const obj2After = obj2 instanceof Foo; // should be TRUE

  console.log(`  obj1 before=${before}(expect true), obj1 after=${obj1After}(expect false), obj2=${obj2After}(expect true)`);

  if (obj1After !== false || obj2After !== true) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }
}

// ============================================================
// Test 5: Symbol.hasInstance override
//   Completely bypasses the prototype chain walk per spec.
// ============================================================
console.log('\n=== Test 5: Symbol.hasInstance ===');
{
  class Foo {}
  const obj = new Foo();

  // default behavior
  const normalResult = obj instanceof Foo; // true
  const normalFake = {} instanceof Foo; // false

  // override: always true
  Object.defineProperty(Foo, Symbol.hasInstance, {
    value: instance => true,
    configurable: true
  });

  const overrideResult = {} instanceof Foo; // should be TRUE now

  console.log(`  normal: obj=${normalResult}(expect true), {}=${normalFake}(expect false)`);
  console.log(`  override: {} instanceof Foo = ${overrideResult}(expect true)`);

  if (overrideResult !== true) {
    console.log('  *** Symbol.hasInstance NOT RESPECTED ***');
  } else {
    console.log('  correctness OK');
  }

  bench('instanceof (Symbol.hasInstance always-true)', () => ({}) instanceof Foo);

  // override: custom logic
  let callCount = 0;
  Object.defineProperty(Foo, Symbol.hasInstance, {
    value: instance => {
      callCount++;
      return instance.special === true;
    },
    configurable: true
  });

  const customA = { special: true } instanceof Foo; // true
  const customB = { special: false } instanceof Foo; // false

  console.log(`  custom: {special:true}=${customA}(expect true), {special:false}=${customB}(expect false)`);
  console.log(`  Symbol.hasInstance was called ${callCount} times`);

  if (customA !== true || customB !== false) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }
}

// ============================================================
// Test 6: Megamorphic — many different shapes through same check
// ============================================================
console.log('\n=== Test 6: Megamorphic instanceof ===');
{
  const depth = 16;
  const chain = buildChain(depth);
  const Root = chain[0];

  const objects = [];
  for (let i = 0; i < 100; i++) {
    const obj = new chain[chain.length - 1]();
    obj['prop_' + i] = i;
    obj['extra_' + i * 7] = true;
    objects.push(obj);
  }

  let idx = 0;
  bench('instanceof (100 shapes)', () => {
    idx = (idx + 1) % objects.length;
    return objects[idx] instanceof Root;
  });
}

// ============================================================
// Test 7: Prototype swap every iteration (the nuclear option)
// ============================================================
console.log('\n=== Test 7: Prototype swap every iteration ===');
{
  const depth = 16;
  const chain = buildChain(depth);
  const Leaf = chain[chain.length - 1];
  const Root = chain[0];

  class Alt {}
  const midIdx = Math.floor(depth / 2);
  const originalProto = Object.getPrototypeOf(chain[midIdx].prototype);

  let toggle = false;
  bench(
    'instanceof (swap each iter)',
    () => {
      toggle = !toggle;
      if (toggle) {
        Object.setPrototypeOf(chain[midIdx].prototype, Alt.prototype);
      } else {
        Object.setPrototypeOf(chain[midIdx].prototype, originalProto);
      }
      return new Leaf() instanceof Root;
    },
    100000
  );
}

// ============================================================
// Test 8: Cross-realm-like — objects from different "worlds"
//   Simulates the cross-realm problem where Array from iframe
//   isn't instanceof Array from parent. Tests if ant has this
//   same behavior (it should, per spec).
// ============================================================
console.log('\n=== Test 8: Unrelated classes with same structure ===');
{
  class FooA {
    method() {
      return 1;
    }
  }
  class FooB {
    method() {
      return 1;
    }
  }

  const objA = new FooA();
  const crossCheck = objA instanceof FooB; // should be FALSE

  console.log(`  FooA instance instanceof FooB = ${crossCheck}(expect false)`);

  if (crossCheck !== false) {
    console.log('  *** CORRECTNESS BUG: unrelated classes should not match ***');
  } else {
    console.log('  correctness OK');
  }
}

// ============================================================
// Test 9: prop_lookup with dynamic shadowing/unshadowing
// ============================================================
console.log('\n=== Test 9: prop_lookup with prototype mutation ===');
for (const depth of [8, 24, 40]) {
  const chain = buildChain(depth);
  const Root = chain[0];
  Root.prototype.marker = 1;
  const Leaf = chain[chain.length - 1];
  const obj = new Leaf();

  console.log(`\n--- depth=${depth} ---`);
  console.log(`  sanity marker=${obj.marker}`);

  const midIdx = Math.floor(depth / 2);
  chain[midIdx].prototype.marker = 42;
  console.log(`  after shadow=${obj.marker} (expected 42)`);

  bench('prop_lookup (shadowed)', () => obj.marker);

  delete chain[midIdx].prototype.marker;
  console.log(`  after delete=${obj.marker} (expected 1)`);

  bench('prop_lookup (after delete)', () => obj.marker);
}

console.log('\n=== done ===');
