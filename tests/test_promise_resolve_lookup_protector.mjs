function assertEqual(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${name}: expected ${expected}, got ${actual}`);
  }
}

const resolveDescriptor = Object.getOwnPropertyDescriptor(Promise, 'resolve');
const intrinsicResolve = resolveDescriptor.value;
const exactPromise = intrinsicResolve.call(Promise, 'exact');

function resolveIntrinsic(value) {
  return Promise.resolve(value);
}

function resolveIntrinsicFrom(thunk) {
  return Promise.resolve(thunk());
}

assertEqual(
  'interpreter intrinsic Promise.resolve reuses exact Promise',
  resolveIntrinsic(exactPromise),
  exactPromise
);

for (let i = 0; i < 20_000; i++) {
  assertEqual(
    'hot intrinsic Promise.resolve reuses exact Promise',
    resolveIntrinsic(exactPromise),
    exactPromise
  );
}

function setResolve(target, value) {
  target.resolve = value;
}

const ordinaryTarget = { resolve: intrinsicResolve };
for (let i = 0; i < 125; i++) setResolve(ordinaryTarget, intrinsicResolve);

const replacementResult = { replacement: true };
function replacementResolve() {
  return replacementResult;
}
setResolve(Promise, replacementResolve);
assertEqual(
  'hot resolve store invalidates protected lookup',
  resolveIntrinsic('replacement'),
  replacementResult
);
Object.defineProperty(Promise, 'resolve', resolveDescriptor);

const order = [];
Object.defineProperty(Promise, 'resolve', {
  configurable: true,
  get() {
    order.push('get resolve');
    return intrinsicResolve;
  },
});
function orderedArgument() {
  order.push('evaluate argument');
  return exactPromise;
}
assertEqual(
  'resolve getter result',
  resolveIntrinsicFrom(orderedArgument),
  exactPromise
);
assertEqual(
  'resolve lookup precedes argument evaluation',
  order.join(','),
  'get resolve,evaluate argument'
);
Object.defineProperty(Promise, 'resolve', resolveDescriptor);

let lateReplacementCalls = 0;
function lateReplacement() {
  lateReplacementCalls++;
  return replacementResult;
}
function mutateResolveArgument() {
  Object.defineProperty(Promise, 'resolve', {
    ...resolveDescriptor,
    value: lateReplacement,
  });
  return exactPromise;
}
assertEqual(
  'captured resolve survives argument mutation',
  resolveIntrinsicFrom(mutateResolveArgument),
  exactPromise
);
assertEqual('late replacement was not called', lateReplacementCalls, 0);
Object.defineProperty(Promise, 'resolve', resolveDescriptor);

const getterError = { getterError: true };
let throwingGetterArgumentRan = false;
Object.defineProperty(Promise, 'resolve', {
  configurable: true,
  get() {
    throw getterError;
  },
});
try {
  resolveIntrinsicFrom(() => (throwingGetterArgumentRan = true));
  throw new Error('throwing resolve getter did not throw');
} catch (error) {
  assertEqual('throwing resolve getter propagates', error, getterError);
}
assertEqual(
  'throwing resolve getter skips argument evaluation',
  throwingGetterArgumentRan,
  false
);
Object.defineProperty(Promise, 'resolve', resolveDescriptor);

function localShadow(value) {
  const Promise = {
    resolve(input) {
      return `local:${input}`;
    },
  };
  return Promise.resolve(value);
}
assertEqual('local Promise shadow stays observable', localShadow('value'), 'local:value');

function makeUpvalueShadow() {
  const Promise = {
    resolve(input) {
      return `upvalue:${input}`;
    },
  };
  return value => Promise.resolve(value);
}
assertEqual(
  'upvalue Promise shadow stays observable',
  makeUpvalueShadow()('value'),
  'upvalue:value'
);

console.log('Promise.resolve lookup protector: ok');
