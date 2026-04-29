import { test, testDeep, summary } from './helpers.js';
import { inspect } from 'node:util';

console.log('Async Iterator Tests\n');

test('AsyncIterator global exists', typeof AsyncIterator, 'function');
test('AsyncIterator prototype async iterator tag', AsyncIterator.prototype[Symbol.toStringTag], 'AsyncIterator');

async function* protoShapeAsyncGen() {
  yield 1;
}
const AsyncGeneratorFunction = protoShapeAsyncGen.constructor;
const protoShapeAsyncIter = protoShapeAsyncGen();
const ownAsyncGeneratorProto = Object.getPrototypeOf(protoShapeAsyncIter);
const sharedAsyncGeneratorProto = Object.getPrototypeOf(ownAsyncGeneratorProto);
const inheritedAsyncIteratorProto = Object.getPrototypeOf(sharedAsyncGeneratorProto);
test('async generator function has prototype property', protoShapeAsyncGen.hasOwnProperty('prototype'), true);
test('async generator function constructor name', AsyncGeneratorFunction.name, 'AsyncGeneratorFunction');
test('async generator function prototype chain', Object.getPrototypeOf(protoShapeAsyncGen), AsyncGeneratorFunction.prototype);
test('async generator function prototype tag', AsyncGeneratorFunction.prototype[Symbol.toStringTag], 'AsyncGeneratorFunction');
test('async generator function inspect tag', inspect(protoShapeAsyncGen), '[AsyncGeneratorFunction: protoShapeAsyncGen]');
test('async generator function prototype prototype', AsyncGeneratorFunction.prototype.prototype, sharedAsyncGeneratorProto);
test(
  'new async generator function throws',
  (() => {
    try {
      new protoShapeAsyncGen();
      return false;
    } catch (err) {
      return true;
    }
  })(),
  true
);
test('async generator instance uses function prototype', ownAsyncGeneratorProto, protoShapeAsyncGen.prototype);
test('async generator shared prototype tag', sharedAsyncGeneratorProto[Symbol.toStringTag], 'AsyncGenerator');
test('async generator shared prototype has next', sharedAsyncGeneratorProto.hasOwnProperty('next'), true);
test('async generator shared prototype inherits AsyncIterator', inheritedAsyncIteratorProto, AsyncIterator.prototype);
test('async generator Symbol.asyncIterator inherited from AsyncIterator', protoShapeAsyncIter[Symbol.asyncIterator](), protoShapeAsyncIter);
test('async generator inherits AsyncIterator helpers', typeof protoShapeAsyncIter.map, 'function');

const dynamicAsyncGen = AsyncGeneratorFunction('yield 4;');
const dynamicAsyncIter = dynamicAsyncGen();
test('AsyncGeneratorFunction constructs async generator function', Object.getPrototypeOf(dynamicAsyncGen), AsyncGeneratorFunction.prototype);
test('AsyncGeneratorFunction instance uses function prototype', Object.getPrototypeOf(dynamicAsyncIter), dynamicAsyncGen.prototype);
test('AsyncGeneratorFunction yielded value', (await dynamicAsyncIter.next()).value, 4);

class CustomAsyncIterator extends AsyncIterator {
  constructor(values) {
    super();
    this.values = values;
    this.index = 0;
  }

  next() {
    if (this.index >= this.values.length) return Promise.resolve({ done: true });
    return Promise.resolve({ done: false, value: this.values[this.index++] });
  }
}

const custom = new CustomAsyncIterator([4, 5]);
test('class extends AsyncIterator', custom[Symbol.asyncIterator](), custom);
test('class instance instanceof AsyncIterator', custom instanceof AsyncIterator, true);
testDeep('custom async iterator toArray', await custom.toArray(), [4, 5]);

const asyncSource = AsyncIterator.from(
  (async function* () {
    yield 1;
    yield 2;
    yield 3;
  })()
);
test('AsyncIterator.from async iterable instanceof', asyncSource instanceof AsyncIterator, true);
testDeep('AsyncIterator.from async iterable values', await asyncSource.toArray(), [1, 2, 3]);

testDeep('AsyncIterator.from iterable values', await AsyncIterator.from([1, 2, 3]).toArray(), [1, 2, 3]);
testDeep('AsyncIterator.from iterator values', await AsyncIterator.from([1, 2, 3].values()).toArray(), [1, 2, 3]);

const promisedStep = await AsyncIterator.from([Promise.resolve(7)]).next();
test('AsyncIterator.from sync iterable awaits promised value', promisedStep.value, 7);
test('AsyncIterator.from sync iterable promised value done', promisedStep.done, false);

let rejectedValueCaught;
try {
  await AsyncIterator.from([Promise.reject(new Error('sync value rejection'))]).next();
} catch (e) {
  rejectedValueCaught = e;
}
test('AsyncIterator.from sync iterable rejects promised value', rejectedValueCaught.message, 'sync value rejection');

const syncReturnSource = {
  next() {
    return { done: false, value: 0 };
  },
  return() {
    return { done: true, value: Promise.resolve(7) };
  },
  [Symbol.iterator]() {
    return this;
  }
};
test('AsyncIterator.from sync return awaits value', (await AsyncIterator.from(syncReturnSource).return()).value, 7);

const syncThrowSource = {
  next() {
    return { done: false, value: 0 };
  },
  throw() {
    return { done: true, value: Promise.resolve(8) };
  },
  [Symbol.iterator]() {
    return this;
  }
};
test('AsyncIterator.from sync throw awaits value', (await AsyncIterator.from(syncThrowSource).throw(new Error('x'))).value, 8);

testDeep(
  'async iterator map',
  await AsyncIterator.from([1, 2, 3])
    .map(x => x * x)
    .toArray(),
  [1, 4, 9]
);
testDeep(
  'async iterator map awaits callback',
  await AsyncIterator.from([1, 2])
    .map(async x => x + 1)
    .toArray(),
  [2, 3]
);
testDeep(
  'async iterator filter',
  await AsyncIterator.from([1, 2, 3, 4])
    .filter(x => x % 2)
    .toArray(),
  [1, 3]
);
testDeep(
  'async iterator filter awaits callback',
  await AsyncIterator.from([1, 2, 3, 4])
    .filter(async x => x > 2)
    .toArray(),
  [3, 4]
);
testDeep('async iterator take', await AsyncIterator.from([1, 2, 3]).take(2).toArray(), [1, 2]);
testDeep('async iterator drop', await AsyncIterator.from([1, 2, 3]).drop(1).toArray(), [2, 3]);
testDeep(
  'async iterator flatMap sync inner',
  await AsyncIterator.from([1, 2, 3])
    .flatMap(x => [x, 0])
    .toArray(),
  [1, 0, 2, 0, 3, 0]
);
testDeep(
  'async iterator flatMap awaits callback',
  await AsyncIterator.from([1, 2])
    .flatMap(async x => [x, x + 10])
    .toArray(),
  [1, 11, 2, 12]
);

async function* doubleAsync(x) {
  yield x;
  yield x * 2;
}

testDeep('async iterator flatMap async inner', await AsyncIterator.from([2, 3]).flatMap(doubleAsync).toArray(), [2, 4, 3, 6]);

test('async iterator every true', await AsyncIterator.from([1, 2, 3]).every(x => typeof x === 'number'), true);
test('async iterator every awaits callback', await AsyncIterator.from([1, 2, 3]).every(async x => x < 4), true);
test('async iterator some true', await AsyncIterator.from([1, 2, 3]).some(x => x === 2), true);
test('async iterator some awaits callback', await AsyncIterator.from([1, 2, 3]).some(async x => x === 3), true);
test('async iterator find', await AsyncIterator.from([1, 2, 3]).find(x => x > 1), 2);
test('async iterator find awaits callback', await AsyncIterator.from([1, 2, 3]).find(async x => x > 2), 3);

function closableAsyncIterator(log) {
  return {
    index: 0,
    next() {
      return Promise.resolve({ done: false, value: ++this.index });
    },
    return() {
      log.push('closed');
      return Promise.resolve({ done: true });
    },
    [Symbol.asyncIterator]() {
      return this;
    }
  };
}

let everyCloseLog = [];
test('async iterator every closes on false', await AsyncIterator.from(closableAsyncIterator(everyCloseLog)).every(x => x < 1), false);
testDeep('async iterator every close log', everyCloseLog, ['closed']);

let someCloseLog = [];
test('async iterator some closes on match', await AsyncIterator.from(closableAsyncIterator(someCloseLog)).some(x => x === 1), true);
testDeep('async iterator some close log', someCloseLog, ['closed']);

let findCloseLog = [];
test('async iterator find closes on match', await AsyncIterator.from(closableAsyncIterator(findCloseLog)).find(x => x === 1), 1);
testDeep('async iterator find close log', findCloseLog, ['closed']);

let throwCloseLog = [];
let throwCaught = false;
try {
  await AsyncIterator.from(closableAsyncIterator(throwCloseLog)).some(() => {
    throw new Error('sync callback failure');
  });
} catch (err) {
  throwCaught = err.message === 'sync callback failure';
}
test('async iterator closes on sync callback throw', throwCaught, true);
testDeep('async iterator sync callback throw close log', throwCloseLog, ['closed']);

let rejectCloseLog = [];
let rejectCaught = false;
try {
  await AsyncIterator.from(closableAsyncIterator(rejectCloseLog)).find(async () => {
    throw new Error('async callback failure');
  });
} catch (err) {
  rejectCaught = err.message === 'async callback failure';
}
test('async iterator closes on async callback rejection', rejectCaught, true);
testDeep('async iterator async callback rejection close log', rejectCloseLog, ['closed']);

const syncAsyncLike = {
  index: 0,
  next() {
    if (this.index >= 5000) return { done: true };
    return { done: false, value: this.index++ };
  },
  [Symbol.asyncIterator]() {
    return this;
  }
};
test('async iterator handles sync next results without recursive stack growth', (await AsyncIterator.from(syncAsyncLike).toArray()).length, 5000);

let forEachSum = 0;
await AsyncIterator.from([1, 2, 3]).forEach(x => {
  forEachSum += x;
});
test('async iterator forEach', forEachSum, 6);

let asyncForEachSum = 0;
await AsyncIterator.from([1, 2, 3]).forEach(async x => {
  asyncForEachSum += x;
});
test('async iterator forEach awaits callback', asyncForEachSum, 6);

test('async iterator reduce', await AsyncIterator.from([1, 2, 3]).reduce((a, b) => a + b), 6);
test('async iterator reduce awaits callback', await AsyncIterator.from([1, 2, 3]).reduce(async (a, b) => a + b), 6);
testDeep(
  'async generator helpers inherited',
  await (async function* () {
    yield 2;
    yield 4;
  })()
    .map(x => x + 1)
    .toArray(),
  [3, 5]
);

summary();
