// bench_prototype_plain.js
// Same mutation/correctness tests as bench_prototype_dynamic.js
// but using Object.create chains instead of class extends.
// This targets ant's fast path (~9ms vs ~50ms for classes).

function nowMs() {
  if (typeof performance !== 'undefined' && performance && typeof performance.now === 'function') {
    return performance.now();
  }
  return Date.now();
}

const BENCH_WARMUP_RUNS = 2;
const BENCH_SAMPLE_RUNS = 7;
const ITERATIONS = 2_000_000;

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  if (sorted.length === 1) return sorted[0];
  const pos = (sorted.length - 1) * p;
  const base = Math.floor(pos);
  const frac = pos - base;
  const next = sorted[base + 1];
  if (next === undefined) return sorted[base];
  return sorted[base] + (next - sorted[base]) * frac;
}

function stableSort(values) {
  const out = values.slice();
  for (let i = 1; i < out.length; i++) {
    const x = out[i];
    let j = i - 1;
    while (j >= 0 && out[j] > x) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = x;
  }
  return out;
}

function bench(label, fn, iters) {
  if (iters === undefined) iters = ITERATIONS;
  const warmupIters = Math.max(100000, (iters / 4) | 0);
  for (let i = 0; i < BENCH_WARMUP_RUNS; i++) fn(warmupIters);

  const samples = [];
  let result = 0;
  for (let i = 0; i < BENCH_SAMPLE_RUNS; i++) {
    const t0 = nowMs();
    const r = fn(iters);
    const dt = nowMs() - t0;
    if (i === 0) result = r;
    samples.push(dt);
  }

  const sorted = stableSort(samples);
  const min = sorted[0];
  const med = percentile(sorted, 0.5);
  const p95 = percentile(sorted, 0.95);
  const max = sorted[sorted.length - 1];
  const opsPerMs = med > 0 ? (iters / med).toFixed(2) : 'inf';

  console.log(
    '  ' +
      label +
      ': median ' +
      med.toFixed(2) +
      'ms (' +
      opsPerMs +
      ' ops/ms)' +
      ', p95 ' +
      p95.toFixed(2) +
      'ms' +
      ', min ' +
      min.toFixed(2) +
      'ms' +
      ', max ' +
      max.toFixed(2) +
      'ms' +
      ' result=' +
      result
  );
  return med;
}

// Build a plain Object.create chain of given depth.
// Returns { C, obj, root, chain }
//   C     = constructor whose .prototype is the root
//   root  = top prototype (has .marker = 1)
//   obj   = leaf object, depth levels below root
//   chain = array of all prototype objects, chain[0] = root
function buildChain(depth) {
  function C() {}
  const root = { marker: 1 };
  C.prototype = root;

  const chain = [root];
  let proto = root;
  for (let i = 0; i < depth; i++) {
    proto = Object.create(proto);
    chain.push(proto);
  }
  const obj = Object.create(proto);
  return { C, obj, root, chain };
}

// ============================================================
// Test 1: Static chain baseline
// ============================================================
console.log('=== Test 1: Static chain (baseline) ===');
for (const depth of [8, 24, 40]) {
  const { C, obj, root } = buildChain(depth);

  console.log('\n--- depth=' + depth + ' ---');
  console.log('  sanity instanceof=' + (obj instanceof C));
  console.log('  sanity isPrototypeOf=' + root.isPrototypeOf(obj));
  console.log('  sanity marker=' + obj.marker);

  bench('instanceof', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) if (obj instanceof C) c++;
    return c;
  });

  bench('isPrototypeOf', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) if (root.isPrototypeOf(obj)) c++;
    return c;
  });

  bench('prop_lookup', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) c += obj.marker;
    return c;
  });
}

// ============================================================
// Test 2: setPrototypeOf — reparent to different chain
// ============================================================
console.log('\n=== Test 2: setPrototypeOf — reparent to different chain ===');
{
  const a = buildChain(24);
  const b = buildChain(24);

  const obj = a.obj;
  const beforeA = obj instanceof a.C;
  const beforeB = obj instanceof b.C;

  // reparent obj into chain B
  Object.setPrototypeOf(obj, b.chain[b.chain.length - 1]);

  const afterA = obj instanceof a.C;
  const afterB = obj instanceof b.C;

  console.log('  before: A=' + beforeA + '(expect true), B=' + beforeB + '(expect false)');
  console.log('  after:  A=' + afterA + '(expect false), B=' + afterB + '(expect true)');
  if (afterA !== false || afterB !== true) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }

  bench('instanceof reparented vs B', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) if (obj instanceof b.C) c++;
    return c;
  });

  bench('instanceof reparented vs A', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) if (obj instanceof a.C) c++;
    return c;
  });
}

// ============================================================
// Test 3: Splice mid-chain to new root
// ============================================================
console.log('\n=== Test 3: Splice mid-chain to new root ===');
{
  const { C, obj, root, chain } = buildChain(24);
  const before = obj instanceof C;

  function NewC() {}
  const newRoot = { tag: 'new' };
  NewC.prototype = newRoot;

  // detach mid-chain from old root, attach to new root
  const midIdx = Math.floor(chain.length / 2);
  Object.setPrototypeOf(chain[midIdx], newRoot);

  const afterOld = obj instanceof C;
  const afterNew = obj instanceof NewC;

  console.log('  before: instanceof C=' + before + '(expect true)');
  console.log('  after:  instanceof C=' + afterOld + '(expect false), instanceof NewC=' + afterNew + '(expect true)');
  if (afterOld !== false || afterNew !== true) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }

  bench('instanceof (post-splice, vs NewC)', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) if (obj instanceof NewC) c++;
    return c;
  });

  bench('instanceof (post-splice, vs old C)', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) if (obj instanceof C) c++;
    return c;
  });
}

// ============================================================
// Test 4: Constructor.prototype reassignment
// ============================================================
console.log('\n=== Test 4: Constructor.prototype reassignment ===');
{
  function Foo() {}
  Foo.prototype = { kind: 'original' };
  const obj1 = new Foo();
  const before = obj1 instanceof Foo;

  // swap .prototype to a new object
  Foo.prototype = { kind: 'replaced' };

  const obj1After = obj1 instanceof Foo; // should be FALSE
  const obj2 = new Foo();
  const obj2After = obj2 instanceof Foo; // should be TRUE

  console.log('  obj1 before=' + before + '(expect true), obj1 after=' + obj1After + '(expect false), obj2=' + obj2After + '(expect true)');
  if (obj1After !== false || obj2After !== true) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }
}

// ============================================================
// Test 5: Symbol.hasInstance
// ============================================================
console.log('\n=== Test 5: Symbol.hasInstance ===');
{
  function Foo() {}
  const obj = new Foo();
  const normalResult = obj instanceof Foo;
  const normalFake = {} instanceof Foo;

  Object.defineProperty(Foo, Symbol.hasInstance, {
    value: function (instance) {
      return true;
    },
    configurable: true
  });

  const overrideResult = {} instanceof Foo;

  console.log('  normal: obj=' + normalResult + '(expect true), {}=' + normalFake + '(expect false)');
  console.log('  override: {} instanceof Foo = ' + overrideResult + '(expect true)');
  if (overrideResult !== true) {
    console.log('  *** Symbol.hasInstance NOT RESPECTED ***');
  } else {
    console.log('  correctness OK');
  }

  bench('instanceof (Symbol.hasInstance always-true)', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) if ({} instanceof Foo) c++;
    return c;
  });

  // custom logic
  let callCount = 0;
  Object.defineProperty(Foo, Symbol.hasInstance, {
    value: function (inst) {
      callCount++;
      return inst.special === true;
    },
    configurable: true
  });

  const customA = { special: true } instanceof Foo;
  const customB = { special: false } instanceof Foo;
  console.log('  custom: {special:true}=' + customA + '(expect true), {special:false}=' + customB + '(expect false)');
  console.log('  Symbol.hasInstance was called ' + callCount + ' times');
  if (customA !== true || customB !== false) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }
}

// ============================================================
// Test 6: Megamorphic — many different shapes
// ============================================================
console.log('\n=== Test 6: Megamorphic instanceof ===');
{
  const { C, root, chain } = buildChain(16);

  const objects = [];
  for (let i = 0; i < 100; i++) {
    const obj = Object.create(chain[chain.length - 1]);
    obj['prop_' + i] = i;
    obj['extra_' + i * 7] = true;
    objects.push(obj);
  }

  let idx = 0;
  bench('instanceof (100 shapes)', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      idx = (idx + 1) % 100;
      if (objects[idx] instanceof C) c++;
    }
    return c;
  });
}

// ============================================================
// Test 7: Prototype swap every iteration
// ============================================================
console.log('\n=== Test 7: Prototype swap every iteration ===');
{
  const { C, root, chain } = buildChain(16);
  const midIdx = Math.floor(chain.length / 2);
  const originalProto = Object.getPrototypeOf(chain[midIdx]);

  const altRoot = { alt: true };
  let toggle = false;

  bench(
    'instanceof (swap each iter)',
    function (n) {
      let c = 0;
      for (let i = 0; i < n; i++) {
        toggle = !toggle;
        if (toggle) {
          Object.setPrototypeOf(chain[midIdx], altRoot);
        } else {
          Object.setPrototypeOf(chain[midIdx], originalProto);
        }
        const obj = Object.create(chain[chain.length - 1]);
        if (obj instanceof C) c++;
      }
      return c;
    },
    100000
  );
}

// ============================================================
// Test 8: Unrelated constructors with same shape
// ============================================================
console.log('\n=== Test 8: Unrelated constructors with same structure ===');
{
  function FooA() {
    this.x = 1;
  }
  function FooB() {
    this.x = 1;
  }

  const objA = new FooA();
  const cross = objA instanceof FooB;

  console.log('  FooA instance instanceof FooB = ' + cross + '(expect false)');
  if (cross !== false) {
    console.log('  *** CORRECTNESS BUG ***');
  } else {
    console.log('  correctness OK');
  }
}

// ============================================================
// Test 9: prop_lookup with shadowing/unshadowing
// ============================================================
console.log('\n=== Test 9: prop_lookup with prototype mutation ===');
for (const depth of [8, 24, 40]) {
  const { obj, chain } = buildChain(depth);

  console.log('\n--- depth=' + depth + ' ---');
  console.log('  sanity marker=' + obj.marker);

  const midIdx = Math.floor(chain.length / 2);
  chain[midIdx].marker = 42;
  console.log('  after shadow=' + obj.marker + ' (expected 42)');

  bench('prop_lookup (shadowed)', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) c += obj.marker;
    return c;
  });

  delete chain[midIdx].marker;
  console.log('  after delete=' + obj.marker + ' (expected 1)');

  bench('prop_lookup (after delete)', function (n) {
    let c = 0;
    for (let i = 0; i < n; i++) c += obj.marker;
    return c;
  });
}

console.log('\n=== done ===');
