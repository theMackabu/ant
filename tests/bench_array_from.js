const ITERATIONS = 1_000_000;
const source = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
const mapFn = x => x * 2;

// --- Polyfill ---
const ArrayFrom_polyfill = function (src, mapFn, thisArg) {
  if (src == null) {
    console.error('TypeError: Cannot convert undefined or null to object');
    return [];
  }
  if (mapFn !== undefined && typeof mapFn !== 'function') {
    console.error('TypeError: mapFn is not a function');
    return [];
  }
  const arr = [];
  let i = 0;
  if (src[Symbol.iterator] != null) {
    for (const v of src) arr.push(mapFn ? mapFn.call(thisArg, v, i++) : v);
  } else {
    const len = src.length >>> 0;
    for (; i < len; i++) arr.push(mapFn ? mapFn.call(thisArg, src[i], i) : src[i]);
  }
  return arr;
};

// --- Builtin ---
function benchBuiltin() {
  const start = performance.now();
  for (let i = 0; i < ITERATIONS; i++) {
    Array.from(source, mapFn);
  }
  return performance.now() - start;
}

// --- Polyfill ---
function benchPolyfill() {
  const start = performance.now();
  for (let i = 0; i < ITERATIONS; i++) {
    ArrayFrom_polyfill(source, mapFn);
  }
  return performance.now() - start;
}

const builtinMs = benchBuiltin();
const polyfillMs = benchPolyfill();

console.log(`Array.from (builtin):  ${builtinMs.toFixed(2)} ms`);
console.log(`Array.from (polyfill): ${polyfillMs.toFixed(2)} ms`);
console.log(`Ratio: builtin is ${(polyfillMs / builtinMs).toFixed(2)}x ${polyfillMs > builtinMs ? 'faster' : 'slower'} than polyfill`);
