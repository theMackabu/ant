import { test, testDeep, summary } from './helpers.js';

console.log('Generator Tests\n');

function* simple() {
  yield 1;
  yield 2;
  yield 3;
}

const gen = simple();
test('generator object typeof', typeof gen, 'object');
test('generator is iterable', gen[Symbol.iterator](), gen);

try {
  new simple();
  test('new generator function throws', false, true);
} catch {
  test('new generator function throws', true, true);
}

test('first yield', gen.next().value, 1);
test('second yield', gen.next().value, 2);
test('third yield', gen.next().value, 3);
test('done', gen.next().done, true);

function* withReturn() {
  yield 1;
  return 'final';
}

const gen2 = withReturn();
test('yield before return', gen2.next().value, 1);
const last = gen2.next();
test('return value', last.value, 'final');
test('return done', last.done, true);

function* counter(start) {
  let i = start;
  while (true) yield i++;
}

const c = counter(10);
test('counter 10', c.next().value, 10);
test('counter 11', c.next().value, 11);
test('counter 12', c.next().value, 12);

function* range(start, end) {
  for (let i = start; i <= end; i++) yield i;
}

const arr = [...range(1, 5)];
testDeep('spread generator', arr, [1, 2, 3, 4, 5]);

let forOfSum = 0;
for (const n of range(1, 4)) {
  forOfSum += n;
}
test('for-of generator', forOfSum, 10);

function* twoWay() {
  const x = yield 1;
  const y = yield x + 1;
  yield y + 1;
}

const tw = twoWay();
test('two way first', tw.next().value, 1);
test('two way second', tw.next(10).value, 11);
test('two way third', tw.next(20).value, 21);

function runDynamicBool(source) {
  try {
    return Function(source)();
  } catch {
    return false;
  }
}

function* yieldStarArray() {
  yield* [4, 5, 6];
}

testDeep('yield* array', [...yieldStarArray()], [4, 5, 6]);

function* yieldStarString() {
  yield* 'a\uD83D\uDCABb';
}

testDeep('yield* string keeps code points', [...yieldStarString()], ['a', '\uD83D\uDCAB', 'b']);

function* yieldStarSparseArray() {
  yield* [7, , 9];
}

const sparse = yieldStarSparseArray();
test('yield* sparse array first', sparse.next().value, 7);
test('yield* sparse array hole value', sparse.next().value, undefined);
test('yield* sparse array third', sparse.next().value, 9);
test('yield* sparse array done', sparse.next().done, true);

function makeIterable(values, methods = {}) {
  values = values.slice();
  values.push(undefined);
  const iterator = {
    next(value) {
      if (methods.next) return methods.next.call(this, value);
      return { value: values.shift(), done: values.length === 0 };
    },
    return: methods.return,
    throw: methods.throw
  };
  return {
    [Symbol.iterator]() {
      return iterator;
    }
  };
}

function* yieldStarIterable() {
  yield* makeIterable([10, 11, 12]);
}

testDeep('yield* generic iterable', [...yieldStarIterable()], [10, 11, 12]);

function* yieldStarIterableInstance() {
  yield* Object.create(makeIterable([13, 14]));
}

testDeep('yield* iterable through prototype', [...yieldStarIterableInstance()], [13, 14]);

function* innerGenerator() {
  yield 15;
  yield 16;
  return 17;
}

function* yieldStarGenerator() {
  return yield* innerGenerator();
}

const ysg = yieldStarGenerator();
test('yield* generator first', ysg.next().value, 15);
test('yield* generator second', ysg.next().value, 16);
const ysgDone = ysg.next();
test('yield* generator return value', ysgDone.value, 17);
test('yield* generator return done', ysgDone.done, true);

function* yieldStarReceivesSentValues() {
  return yield* (function* () {
    const received = yield 'ready';
    return received + 1;
  })();
}

const yss = yieldStarReceivesSentValues();
test('yield* sent first', yss.next().value, 'ready');
const yssDone = yss.next(40);
test('yield* sent return value', yssDone.value, 41);
test('yield* sent return done', yssDone.done, true);

let yieldStarThrowCaught = false;
function* yieldStarThrowForwarding() {
  yield* (function* () {
    try {
      yield 'before throw';
    } catch (e) {
      yieldStarThrowCaught = e === 'boom';
      return 'caught';
    }
  })();
  yield 'after throw';
}

const yst = yieldStarThrowForwarding();
test('yield* throw forwarding first', yst.next().value, 'before throw');
test('yield* throw forwarding continues outer', yst.throw('boom').value, 'after throw');
test('yield* throw forwarding caught by delegate', yieldStarThrowCaught, true);

let yieldStarClosed = '';
const closingIter = makeIterable([1, 2, 3], {
  return() {
    yieldStarClosed += 'a';
    return { done: true };
  }
});

function* yieldStarReturnClosing() {
  try {
    yield* closingIter;
  } finally {
    yieldStarClosed += 'b';
  }
}

const ysrc = yieldStarReturnClosing();
test('yield* return closing first', ysrc.next().value, 1);
ysrc.return('closed');
test('yield* return calls delegate and finally', yieldStarClosed, 'ab');

let yieldStarThrowClosed = false;
const noThrowIter = makeIterable([1, 2], {
  throw: undefined,
  return() {
    yieldStarThrowClosed = true;
    return { done: true };
  }
});

function* yieldStarThrowClosing() {
  try {
    yield* noThrowIter;
  } catch {}
}

const ystc = yieldStarThrowClosing();
test('yield* throw closing first', ystc.next().value, 1);
ystc.throw('close please');
test('yield* throw without delegate throw closes iterator', yieldStarThrowClosed, true);

try {
  (function* () {
    yield* 123;
  })().next();
  test('yield* non-iterable throws', false, true);
} catch {
  test('yield* non-iterable throws', true, true);
}

function* catchesThrow() {
  try {
    yield 'waiting';
  } catch (e) {
    return e;
  }
}

const ct = catchesThrow();
test('generator throw setup', ct.next().value, 'waiting');
const ctDone = ct.throw('thrown value');
test('generator throw caught value', ctDone.value, 'thrown value');
test('generator throw caught done', ctDone.done, true);

let finallyRan = false;
function* returnRunsFinally() {
  try {
    yield 'open';
  } finally {
    finallyRan = true;
  }
}

const rrf = returnRunsFinally();
test('generator return setup', rrf.next().value, 'open');
const rrfDone = rrf.return('done now');
test('generator return result value', rrfDone.value, 'done now');
test('generator return result done', rrfDone.done, true);
test('generator return runs finally', finallyRan, true);

function* protoShapeFn() {}
const protoShapeGen = protoShapeFn();
const ownGeneratorProto = Object.getPrototypeOf(protoShapeGen);
const sharedGeneratorProto = Object.getPrototypeOf(ownGeneratorProto);
const iteratorProto = Object.getPrototypeOf(sharedGeneratorProto);
test('generator instance uses function prototype', ownGeneratorProto, protoShapeFn.prototype);
test('generator shared prototype has next', sharedGeneratorProto.hasOwnProperty('next'), true);
test('generator Symbol.iterator inherited from iterator prototype', iteratorProto.hasOwnProperty(Symbol.iterator), true);

test(
  'shorthand object generator method',
  runDynamicBool(`
  const o = {
    *generator() {
      yield 5;
      yield 6;
    }
  };
  const it = o.generator();
  return it.next().value === 5 &&
    it.next().value === 6 &&
    it.next().done === true;
`),
  true
);

test(
  'shorthand computed generator method',
  runDynamicBool(`
  const name = 'generator';
  const o = {
    *[name]() {
      yield 7;
      yield 8;
    }
  };
  const it = o.generator();
  return it.next().value === 7 &&
    it.next().value === 8 &&
    it.next().done === true;
`),
  true
);

test(
  'shorthand string-keyed generator method',
  runDynamicBool(`
  const o = {
    *"foo bar"() {
      yield 9;
      yield 10;
    }
  };
  const it = o['foo bar']();
  return it.next().value === 9 &&
    it.next().value === 10 &&
    it.next().done === true;
`),
  true
);

test(
  'class generator method',
  runDynamicBool(`
  class C {
    *generator() {
      yield 11;
      yield 12;
    }
  }
  const it = new C().generator();
  return it.next().value === 11 &&
    it.next().value === 12 &&
    it.next().done === true;
`),
  true
);

test(
  'class computed generator method',
  runDynamicBool(`
  const name = 'generator';
  class C {
    *[name]() {
      yield 13;
      yield 14;
    }
  }
  const it = new C().generator();
  return it.next().value === 13 &&
    it.next().value === 14 &&
    it.next().done === true;
`),
  true
);

test(
  'generator method cannot be constructor',
  runDynamicBool(`
  const o = {
    *generator() {
      yield 1;
    }
  };
  try {
    new o.generator();
    return false;
  } catch {
    return true;
  }
`),
  true
);

test(
  'class generator constructor syntax rejected',
  runDynamicBool(`
  try {
    Function('class Bad { *constructor() { yield 1; } }');
    return false;
  } catch {
    return true;
  }
`),
  true
);

summary();
