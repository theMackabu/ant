function assertEqual(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${name}: expected ${expected}, got ${actual}`);
  }
}

// Warm the setters before crossing from an ordinary object to a Promise. The
// Promise write may miss the property IC, so this covers a hot JIT caller's
// generic mutation path rather than claiming direct-store MIR coverage.
function setConstructor(target, value) {
  target.constructor = value;
}

const ordinaryConstructorTarget = { constructor: Promise };
for (let i = 0; i < 125; i++) {
  setConstructor(ordinaryConstructorTarget, Promise);
}

const constructorPromise = Promise.resolve('constructor value');
function OtherPromise() {}
setConstructor(constructorPromise, OtherPromise);
const wrappedConstructorPromise = Promise.resolve(constructorPromise);
assertEqual(
  'hot constructor store invalidates Promise reuse',
  wrappedConstructorPromise === constructorPromise,
  false
);
assertEqual(
  'hot constructor store wrapper adopts Promise value',
  await wrappedConstructorPromise,
  'constructor value'
);

function setThen(target, value) {
  target.then = value;
}

const ordinaryThenTarget = { then: Promise.prototype.then };
for (let i = 0; i < 125; i++) {
  setThen(ordinaryThenTarget, Promise.prototype.then);
}

const thenPromise = Promise.resolve('internal value');
setThen(thenPromise, resolve => resolve('observable value'));
assertEqual(
  'hot then store invalidates resolving-function fast path',
  await new Promise(resolve => resolve(thenPromise)),
  'observable value'
);

console.log('Promise protector hot-store invalidation: ok');
