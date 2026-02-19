function nowMs() {
  return Date.now();
}

function makeChain(depth) {
  function C() {}
  const root = { marker: 1 };
  C.prototype = root;

  let obj = Object.create(root);
  for (let i = 0; i < depth; i++) obj = Object.create(obj);
  return { C, obj, root };
}

function bench(label, fn, iters) {
  const t0 = nowMs();
  const result = fn(iters);
  const dt = nowMs() - t0;
  const opsPerMs = dt > 0 ? (iters / dt).toFixed(2) : 'inf';
  console.log(label + ': ' + dt + 'ms (' + opsPerMs + ' ops/ms) result=' + result);
}

function runDepth(depth) {
  const iters = 2_000_000;
  const { C, obj, root } = makeChain(depth);

  console.log('\n=== depth=' + depth + ' ===');
  console.log('sanity instanceof=' + (obj instanceof C));
  console.log('sanity isPrototypeOf=' + root.isPrototypeOf(obj));
  console.log('sanity marker=' + obj.marker);

  bench(
    'instanceof',
    n => {
      let c = 0;
      for (let i = 0; i < n; i++) if (obj instanceof C) c++;
      return c;
    },
    iters
  );

  bench(
    'isPrototypeOf',
    n => {
      let c = 0;
      for (let i = 0; i < n; i++) if (root.isPrototypeOf(obj)) c++;
      return c;
    },
    iters
  );

  bench(
    'prop_lookup',
    n => {
      let c = 0;
      for (let i = 0; i < n; i++) c += obj.marker;
      return c;
    },
    iters
  );
}

console.log('prototype-chain benchmark');
runDepth(8);
runDepth(24);
runDepth(40);
