function assertEqual(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${name}: expected ${expected}, got ${actual}`);
  }
}

const globalPromiseDescriptor = Object.getOwnPropertyDescriptor(
  globalThis,
  'Promise'
);
const intrinsicPromise = globalPromiseDescriptor.value;
const exactPromise = intrinsicPromise.resolve('exact');

function resolveGlobal(value) {
  return Promise.resolve(value);
}

function resolveGlobalFrom(thunk) {
  return Promise.resolve(thunk());
}

assertEqual(
  'interpreter global Promise lookup reuses exact Promise',
  resolveGlobal(exactPromise),
  exactPromise
);
for (let i = 0; i < 20_000; i++) {
  assertEqual(
    'hot global Promise lookup reuses exact Promise',
    resolveGlobal(exactPromise),
    exactPromise
  );
}

function setPromise(target, value) {
  target.Promise = value;
}

const ordinaryTarget = { Promise: intrinsicPromise };
for (let i = 0; i < 125; i++) setPromise(ordinaryTarget, intrinsicPromise);

const replacementResult = { replacement: true };
const replacementPromise = {
  resolve(value) {
    return value === 'replacement' ? replacementResult : value;
  },
};
setPromise(globalThis, replacementPromise);
assertEqual(
  'hot global Promise store invalidates protected lookup',
  resolveGlobal('replacement'),
  replacementResult
);
Object.defineProperty(globalThis, 'Promise', globalPromiseDescriptor);

const order = [];
Object.defineProperty(globalThis, 'Promise', {
  configurable: true,
  get() {
    order.push('get Promise');
    return intrinsicPromise;
  },
});
function orderedArgument() {
  order.push('evaluate argument');
  return exactPromise;
}
assertEqual(
  'global getter result',
  resolveGlobalFrom(orderedArgument),
  exactPromise
);
assertEqual(
  'global Promise lookup precedes argument evaluation',
  order.join(','),
  'get Promise,evaluate argument'
);
Object.defineProperty(globalThis, 'Promise', globalPromiseDescriptor);

let lateReplacementCalls = 0;
const lateReplacementPromise = {
  resolve() {
    lateReplacementCalls++;
    return replacementResult;
  },
};
function mutateGlobalArgument() {
  Object.defineProperty(globalThis, 'Promise', {
    ...globalPromiseDescriptor,
    value: lateReplacementPromise,
  });
  return exactPromise;
}
assertEqual(
  'captured global Promise and resolve survive argument mutation',
  resolveGlobalFrom(mutateGlobalArgument),
  exactPromise
);
assertEqual('late global replacement was not called', lateReplacementCalls, 0);
Object.defineProperty(globalThis, 'Promise', globalPromiseDescriptor);

const getterError = { getterError: true };
let throwingGetterArgumentRan = false;
Object.defineProperty(globalThis, 'Promise', {
  configurable: true,
  get() {
    throw getterError;
  },
});
try {
  resolveGlobalFrom(() => (throwingGetterArgumentRan = true));
  throw new Error('throwing global Promise getter did not throw');
} catch (error) {
  assertEqual('throwing global Promise getter propagates', error, getterError);
}
assertEqual(
  'throwing global Promise getter skips argument evaluation',
  throwingGetterArgumentRan,
  false
);
Object.defineProperty(globalThis, 'Promise', globalPromiseDescriptor);

let deletedArgumentRan = false;
delete globalThis.Promise;
try {
  resolveGlobalFrom(() => (deletedArgumentRan = true));
  throw new Error('missing global Promise did not throw');
} catch (error) {
  assertEqual('missing global Promise throws ReferenceError', error.name, 'ReferenceError');
}
assertEqual(
  'missing global Promise skips argument evaluation',
  deletedArgumentRan,
  false
);
Object.defineProperty(globalThis, 'Promise', globalPromiseDescriptor);

console.log('global Promise lookup protector: ok');
