const now = () =>
  typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now();

function bench(name, fn, iters = 1) {
  fn();
  const t0 = now();
  for (let i = 0; i < iters; i++) fn();
  const dt = now() - t0;
  const per = (dt / iters).toFixed(6);
  console.log(`${name}: ${dt.toFixed(2)} ms total, ${per} ms/iter (${iters} iters)`);
}

const visibleSymbol = Symbol('visible');
const getterSymbol = Symbol('getter');

function makePlainSource() {
  return {
    a: 1,
    b: 2,
    c: 3,
    d: 4,
    e: 5,
    f: 6,
  };
}

function makeMixedSource() {
  return {
    a: 1,
    b: 2,
    c: 3,
    d: 4,
    [visibleSymbol]: 5,
  };
}

function makeGetterSource() {
  const obj = {
    a: 1,
    b: 2,
    c: 3,
    d: 4,
  };

  Object.defineProperty(obj, getterSymbol, {
    enumerable: true,
    get() {
      return 42;
    },
  });

  return obj;
}

function spreadPlain() {
  const src = makePlainSource();
  return { ...src };
}

function spreadMixedSymbol() {
  const src = makeMixedSource();
  return { ...src };
}

function spreadSymbolGetter() {
  const src = makeGetterSource();
  return { ...src };
}

console.log('=== Object Spread Benchmark ===\n');

bench('spread plain object', spreadPlain, 200_000);
bench('spread enumerable symbol', spreadMixedSymbol, 200_000);
bench('spread enumerable symbol getter', spreadSymbolGetter, 100_000);
