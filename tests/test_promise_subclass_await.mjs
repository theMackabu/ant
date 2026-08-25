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

const native = Promise.resolve('native');
native.then = resolve => resolve('override');
assertEqual('await reuses exact intrinsic Promise', await native, 'native');
assertEqual('Promise.resolve reuses exact intrinsic Promise', Promise.resolve(native) === native, true);
assertEqual(
  'resolving function observes exact Promise then override',
  await new Promise(resolve => resolve(native)),
  'override'
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
