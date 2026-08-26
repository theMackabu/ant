function assertEqual(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${name}: expected ${expected}, got ${actual}`);
  }
}

async function assertRejectsWith(name, promise, expected) {
  try {
    await promise;
  } catch (error) {
    assertEqual(name, error, expected);
    return;
  }
  throw new Error(`${name}: expected rejection`);
}

function assertThrows(name, callback) {
  try {
    callback();
  } catch {
    return;
  }
  throw new Error(`${name}: expected throw`);
}

class LazyPromise extends Promise {
  constructor(value = 'parsed') {
    super(resolve => resolve(null));
    this.publicValue = value;
  }

  then(onFulfilled, onRejected) {
    return Promise.resolve(this.publicValue).then(onFulfilled, onRejected);
  }
}

const subclass = new LazyPromise();
assertEqual('direct subclass then override', await subclass.then(value => value), 'parsed');
assertEqual('await observes subclass then override', await subclass, 'parsed');

const wrappedSubclass = Promise.resolve(subclass);
assertEqual('Promise.resolve wraps subclass', wrappedSubclass === subclass, false);
assertEqual('Promise.resolve adopts subclass then override', await wrappedSubclass, 'parsed');
assertEqual(
  'resolving function adopts subclass then override',
  await new Promise(resolve => resolve(new LazyPromise())),
  'parsed'
);

const pristineNative = Promise.resolve('pristine');
assertEqual('pristine await reuses exact intrinsic Promise', await pristineNative, 'pristine');
assertEqual(
  'pristine Promise.resolve reuses exact intrinsic Promise',
  Promise.resolve(pristineNative) === pristineNative,
  true
);
assertEqual(
  'pristine resolving function adopts exact intrinsic Promise',
  await new Promise(resolve => resolve(pristineNative)),
  'pristine'
);

const intrinsicConstructorDescriptor = Object.getOwnPropertyDescriptor(
  Promise.prototype,
  'constructor'
);
const constructorDeletionPromise = Promise.resolve('constructor deletion');
delete Promise.prototype.constructor;
const constructorDeletionWrapped = Promise.resolve(constructorDeletionPromise);
Object.defineProperty(
  Promise.prototype,
  'constructor',
  intrinsicConstructorDescriptor
);
assertEqual(
  'deleting Promise.prototype.constructor disables exact Promise reuse',
  constructorDeletionWrapped === constructorDeletionPromise,
  false
);
assertEqual(
  'constructor deletion wrapper adopts the original Promise',
  await constructorDeletionWrapped,
  'constructor deletion'
);

const intrinsicThenDescriptor = Object.getOwnPropertyDescriptor(
  Promise.prototype,
  'then'
);
const thenDeletionPromise = Promise.resolve('then deletion');
delete Promise.prototype.then;
const thenDeletionOuter = new Promise(resolve => resolve(thenDeletionPromise));
const thenDeletionObserved = intrinsicThenDescriptor.value.call(
  thenDeletionOuter,
  value => value === thenDeletionPromise
);
Object.defineProperty(Promise.prototype, 'then', intrinsicThenDescriptor);
assertEqual(
  'deleting Promise.prototype.then disables intrinsic then reuse',
  await thenDeletionObserved,
  true
);

const native = Promise.resolve('native');
native.then = resolve => resolve('override');
assertEqual('await reuses exact intrinsic Promise', await native, 'native');
assertEqual('Promise.resolve reuses exact intrinsic Promise', Promise.resolve(native) === native, true);
assertEqual(
  'resolving function observes exact Promise then override',
  await new Promise(resolve => resolve(native)),
  'override'
);

const proxyConstructorPromise = Promise.resolve('proxy constructor');
const originalPromisePrototypeParent = Object.getPrototypeOf(Promise.prototype);
let constructorTrapCalls = 0;
const proxyPromisePrototypeParent = new Proxy(originalPromisePrototypeParent, {
  get(target, key, receiver) {
    if (key === 'constructor') {
      constructorTrapCalls++;
      return Promise;
    }
    return Reflect.get(target, key, receiver);
  },
});
delete Promise.prototype.constructor;
Object.setPrototypeOf(Promise.prototype, proxyPromisePrototypeParent);
assertEqual(
  'Promise.resolve observes constructor through Proxy prototype',
  Promise.resolve(proxyConstructorPromise) === proxyConstructorPromise,
  true
);
assertEqual('Promise.resolve invokes constructor Proxy trap once', constructorTrapCalls, 1);
Object.setPrototypeOf(Promise.prototype, originalPromisePrototypeParent);
Object.defineProperty(
  Promise.prototype,
  'constructor',
  intrinsicConstructorDescriptor
);

const callableThen = new Proxy(
  resolve => resolve(42),
  {}
);
assertEqual(
  'Promise.resolve invokes callable Proxy then',
  await Promise.resolve({ then: callableThen }),
  42
);

const overriddenCombinatorPromise = Promise.resolve('internal');
overriddenCombinatorPromise.then = onFulfilled =>
  Promise.resolve(onFulfilled('observable'));
assertEqual(
  'Promise.all observes exact Promise then override',
  (await Promise.all([overriddenCombinatorPromise]))[0],
  'observable'
);
assertEqual(
  'Promise.all adopts Promise subclass then override',
  (await Promise.all([new LazyPromise('all parsed')]))[0],
  'all parsed'
);
assertEqual(
  'Promise.allSettled adopts Promise subclass then override',
  (await Promise.allSettled([new LazyPromise('settled parsed')]))[0].value,
  'settled parsed'
);
assertEqual(
  'Promise.race adopts Promise subclass then override',
  await Promise.race([new LazyPromise('race parsed')]),
  'race parsed'
);
assertEqual(
  'Promise.any adopts Promise subclass then override',
  await Promise.any([new LazyPromise('any parsed')]),
  'any parsed'
);

let combinatorResolveGets = 0;
let combinatorResolveCalls = 0;
class CombinatorPromise extends Promise {
  constructor(executor) {
    super(executor);
    this.constructedBySubclass = true;
  }
}
Object.defineProperty(CombinatorPromise, 'resolve', {
  configurable: true,
  get() {
    combinatorResolveGets++;
    return value => {
      combinatorResolveCalls++;
      return Promise.resolve(value);
    };
  },
});

const subclassAll = CombinatorPromise.all([1, 2]);
assertEqual('Promise.all constructs receiver capability', subclassAll.constructedBySubclass, true);
assertEqual('Promise.all calls receiver resolve per item', (await subclassAll).join(','), '1,2');

const subclassAllSettled = CombinatorPromise.allSettled([3]);
assertEqual(
  'Promise.allSettled constructs receiver capability',
  subclassAllSettled.constructedBySubclass,
  true
);
assertEqual('Promise.allSettled calls receiver resolve', (await subclassAllSettled)[0].value, 3);

const subclassRace = CombinatorPromise.race([4]);
assertEqual('Promise.race constructs receiver capability', subclassRace.constructedBySubclass, true);
assertEqual('Promise.race calls receiver resolve', await subclassRace, 4);

const subclassAny = CombinatorPromise.any([5]);
assertEqual('Promise.any constructs receiver capability', subclassAny.constructedBySubclass, true);
assertEqual('Promise.any calls receiver resolve', await subclassAny, 5);
assertEqual('combinators read receiver resolve once each', combinatorResolveGets, 4);
assertEqual('combinators call receiver resolve per item', combinatorResolveCalls, 5);

const tamperedCallbackPromise = Promise.resolve('internal');
tamperedCallbackPromise.then = onFulfilled => {
  onFulfilled.index = 99;
  onFulfilled.tracker = null;
  onFulfilled('not corrupted');
  return Promise.resolve();
};
assertEqual(
  'Promise.all callback metadata is not observable',
  (await Promise.all([tamperedCallbackPromise]))[0],
  'not corrupted'
);

for (const [name, combine] of [
  ['all', value => Promise.all([value])],
  ['allSettled', value => Promise.allSettled([value])],
  ['race', value => Promise.race([value])],
  ['any', value => Promise.any([value])],
]) {
  const combinatorThenError = new Error(`${name} then getter failed`);
  const throwingCombinatorThen = Promise.resolve('ignored');
  Object.defineProperty(throwingCombinatorThen, 'then', {
    configurable: true,
    get() {
      throw combinatorThenError;
    },
  });
  let threwSynchronously = false;
  let combinatorResult;
  try {
    combinatorResult = combine(throwingCombinatorThen);
  } catch {
    threwSynchronously = true;
  }
  assertEqual(`Promise.${name} does not throw synchronously`, threwSynchronously, false);
  await assertRejectsWith(
    `Promise.${name} rejects on throwing then getter`,
    combinatorResult,
    combinatorThenError
  );
}

const proxyExecutorPromise = new Promise(
  new Proxy(resolve => resolve('proxy executor'), {})
);
assertEqual('Promise accepts callable Proxy executor', await proxyExecutorPromise, 'proxy executor');
assertEqual(
  'Promise chain invokes callable Proxy handler',
  await Promise.resolve(6).then(new Proxy(value => value + 1, {})),
  7
);

let unhandledProxyRejections = 0;
globalThis.onunhandledrejection = () => {
  unhandledProxyRejections++;
};
await Promise.reject('handled by Proxy').catch(new Proxy(() => {}, {}));
await new Promise(resolve => setTimeout(resolve, 0));
assertEqual(
  'callable Proxy rejection handler marks rejection handled',
  unhandledProxyRejections,
  0
);
globalThis.onunhandledrejection = null;

let finallyProxyCalls = 0;
const finallyProxyCallback = new Proxy(() => {
  finallyProxyCalls++;
  return {
    then: new Proxy(resolve => {
      resolve();
    }, {}),
  };
}, {});
assertEqual(
  'Promise.finally invokes callable Proxy callback and then',
  await Promise.resolve('finally value').finally(finallyProxyCallback),
  'finally value'
);
await assertRejectsWith(
  'rejected Promise.finally invokes callable Proxy callback and then',
  Promise.reject('finally reason').finally(finallyProxyCallback),
  'finally reason'
);
assertEqual('Promise.finally invokes Proxy callback twice', finallyProxyCalls, 2);

let finallyPromiseThenCalls = 0;
const finallyReturnedPromise = Promise.resolve('ignored');
finallyReturnedPromise.then = new Proxy((onFulfilled, onRejected) => {
  finallyPromiseThenCalls++;
  return intrinsicThenDescriptor.value.call(
    finallyReturnedPromise,
    onFulfilled,
    onRejected
  );
}, {});
assertEqual(
  'Promise.finally observes callable Proxy then on exact Promise',
  await Promise.resolve('preserved').finally(() => finallyReturnedPromise),
  'preserved'
);
assertEqual(
  'Promise.finally invokes exact Promise then override once',
  finallyPromiseThenCalls,
  1
);

const finallyThenError = new Error('finally then getter failed');
const finallyThrowingThen = {};
Object.defineProperty(finallyThrowingThen, 'then', {
  get() {
    throw finallyThenError;
  },
});
await assertRejectsWith(
  'Promise.finally rejects from callback result then getter',
  Promise.resolve('ignored').finally(() => finallyThrowingThen),
  finallyThenError
);

Math.max.then = resolve => resolve('builtin thenable');
assertEqual('await adopts builtin function thenable', await Math.max, 'builtin thenable');
assertEqual(
  'resolving function adopts builtin function thenable',
  await new Promise(resolve => resolve(Math.max)),
  'builtin thenable'
);
delete Math.max.then;

let subclassConstructorCalls = 0;
class CountingPromise extends Promise {
  constructor(executor) {
    subclassConstructorCalls++;
    super(executor);
  }
}

const countedPromise = CountingPromise.resolve('counted');
assertEqual('subclass Promise.resolve invokes subclass constructor', subclassConstructorCalls, 1);
assertEqual('subclass Promise.resolve returns subclass instance', countedPromise instanceof CountingPromise, true);
assertEqual('subclass Promise.resolve reuses exact subclass Promise', CountingPromise.resolve(countedPromise), countedPromise);
assertEqual('reusing subclass Promise does not invoke constructor', subclassConstructorCalls, 1);
assertEqual('subclass Promise.resolve settles through captured resolve', await countedPromise, 'counted');

assertThrows(
  'Promise.resolve rejects non-constructible receiver',
  () => Promise.resolve.call(() => {}, 'ignored')
);

let customResolvedValue;
function CustomCapability(executor) {
  executor(value => {
    customResolvedValue = value;
  }, () => {});
  this.kind = 'custom capability';
}
const customCapability = Promise.resolve.call(CustomCapability, 'custom value');
assertEqual('generic Promise.resolve constructs receiver', customCapability instanceof CustomCapability, true);
assertEqual('generic Promise.resolve returns constructed capability', customCapability.kind, 'custom capability');
assertEqual('generic Promise.resolve calls captured resolve', customResolvedValue, 'custom value');

function MissingCapability() {}
assertThrows(
  'generic Promise.resolve requires capability functions',
  () => Promise.resolve.call(MissingCapability, 'ignored')
);

const constructorError = new Error('constructor getter failed');
const constructorThrowing = Promise.resolve('ignored');
Object.defineProperty(constructorThrowing, 'constructor', {
  configurable: true,
  get() {
    throw constructorError;
  },
});

await assertRejectsWith(
  'await propagates throwing constructor getter',
  (async () => await constructorThrowing)(),
  constructorError
);

let constructorThrown;
try {
  Promise.resolve(constructorThrowing);
} catch (error) {
  constructorThrown = error;
}
assertEqual('Promise.resolve throws from constructor getter', constructorThrown, constructorError);

const thenError = new Error('then getter failed');
const throwingThenable = {};
Object.defineProperty(throwingThenable, 'then', {
  configurable: true,
  get() {
    throw thenError;
  },
});

await assertRejectsWith(
  'Promise.resolve rejects from then getter',
  Promise.resolve(throwingThenable),
  thenError
);
await assertRejectsWith(
  'resolving function rejects from then getter',
  new Promise(resolve => resolve(throwingThenable)),
  thenError
);

let thenReads = 0;
const plainThenable = {
  get then() {
    thenReads++;
    return resolve => resolve('plain');
  },
};
assertEqual('await adopts plain thenable', await plainThenable, 'plain');
assertEqual('await reads plain then once', thenReads, 1);
assertEqual('ordinary Promise chain', await Promise.resolve(2).then(value => value + 3), 5);

const firstCallWins = new Promise((resolve, reject) => {
  resolve({
    then(onFulfilled, onRejected) {
      onFulfilled('first');
      onRejected('second');
      throw new Error('third');
    },
  });
  reject('outer second');
});
assertEqual('resolving functions are first-call-wins', await firstCallWins, 'first');

const executorThrowAfterResolve = new Promise(resolve => {
  resolve({ then: onFulfilled => onFulfilled('resolved before throw') });
  throw new Error('executor throw after resolve');
});
assertEqual(
  'executor throw cannot replace a thenable resolution',
  await executorThrowAfterResolve,
  'resolved before throw'
);

async function hotAwaitSubclass(value) {
  return await value;
}

let hotResult;
for (let i = 0; i < 125; i++) {
  hotResult = await hotAwaitSubclass(new LazyPromise(i));
}
assertEqual('hot await subclass path', hotResult, 124);

console.log('promise subclass await: ok');
