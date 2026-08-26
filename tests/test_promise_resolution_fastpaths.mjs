function assertEqual(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${name}: expected ${expected}, got ${actual}`);
  }
}

const adoptionOrder = [];
const settledSource = Promise.resolve('adopted');
const adoptedTarget = new Promise(resolve => resolve(settledSource));

queueMicrotask(() => {
  adoptionOrder.push('outer microtask');
  queueMicrotask(() => adoptionOrder.push('nested microtask'));
});
adoptedTarget.then(() => adoptionOrder.push('target reaction'));

assertEqual('canonical Promise adoption value', await adoptedTarget, 'adopted');
assertEqual(
  'canonical Promise adoption keeps the thenable-job turn',
  adoptionOrder.join(','),
  'outer microtask,nested microtask,target reaction'
);

const intrinsicSpeciesDescriptor = Object.getOwnPropertyDescriptor(
  Promise,
  Symbol.species
);
let speciesGetterCalls = 0;
Object.defineProperty(Promise, Symbol.species, {
  configurable: true,
  get() {
    speciesGetterCalls++;
    return Promise;
  },
});

const speciesSource = Promise.resolve('species value');
const speciesTarget = new Promise(resolve => resolve(speciesSource));
assertEqual(
  'Promise adoption after species mutation',
  await speciesTarget,
  'species value'
);
assertEqual(
  'Promise adoption observes mutated Symbol.species',
  speciesGetterCalls,
  1
);

Object.defineProperty(Promise, Symbol.species, intrinsicSpeciesDescriptor);

const primitiveOrder = [];
async function awaitPrimitive() {
  primitiveOrder.push('before await');
  const value = await 7;
  primitiveOrder.push('after await');
  return value;
}

const primitiveResult = awaitPrimitive();
primitiveOrder.push('synchronous caller');
queueMicrotask(() => primitiveOrder.push('later microtask'));
assertEqual('primitive await value', await primitiveResult, 7);
assertEqual(
  'primitive await keeps one microtask turn',
  primitiveOrder.join(','),
  'before await,synchronous caller,after await,later microtask'
);

for (let i = 0; i < 256; i++) {
  const marker = `primitive-${i}-${'x'.repeat(128)}`;
  assertEqual(`primitive await string ${i}`, await marker, marker);
}

function resolveFrom(Promise, value) {
  const result = Promise.resolve(value);
  return result;
}

const exactPromise = Promise.resolve('exact');
for (let i = 0; i < 20_000; i++) {
  assertEqual('hot intrinsic Promise.resolve', resolveFrom(Promise, exactPromise), exactPromise);
}

class DerivedPromise extends Promise {}
const derived = resolveFrom(DerivedPromise, 'derived');
assertEqual('subclass receiver fallback type', derived instanceof DerivedPromise, true);
assertEqual('subclass receiver fallback value', await derived, 'derived');

const resolveDescriptor = Object.getOwnPropertyDescriptor(Promise, 'resolve');
const replacementResult = { replacement: true };
let replacementThis;
let replacementArg;
Promise.resolve = function replacement(value) {
  replacementThis = this;
  replacementArg = value;
  return replacementResult;
};

assertEqual(
  'replaced Promise.resolve fallback result',
  resolveFrom(Promise, 'replacement input'),
  replacementResult
);
assertEqual('replaced Promise.resolve fallback receiver', replacementThis, Promise);
assertEqual('replaced Promise.resolve fallback argument', replacementArg, 'replacement input');
Object.defineProperty(Promise, 'resolve', resolveDescriptor);

const shadowedPromise = {
  resolve(value) {
    return `shadowed:${value}`;
  },
};
assertEqual(
  'shadowed Promise binding fallback',
  resolveFrom(shadowedPromise, 'value'),
  'shadowed:value'
);
assertEqual('restored intrinsic Promise.resolve', resolveFrom(Promise, exactPromise), exactPromise);

assertEqual('zero-argument Promise.resolve', await Promise.resolve(), undefined);
assertEqual('extra-argument Promise.resolve', await Promise.resolve('first', 'ignored'), 'first');

let resolveFirst;
let resolveSecond;
const firstPending = new Promise(resolve => {
  resolveFirst = resolve;
});
const secondPending = new Promise(resolve => {
  resolveSecond = resolve;
});
resolveFirst(secondPending);
resolveSecond(firstPending);
let mutualAdoptionCheckpoint = false;
queueMicrotask(() => {
  mutualAdoptionCheckpoint = true;
});
await 0;
assertEqual(
  'mutual pending Promise adoption reaches the next job',
  mutualAdoptionCheckpoint,
  true
);

console.log('Promise resolution fast paths: ok');
