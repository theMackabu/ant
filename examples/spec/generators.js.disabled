import { test, testDeep, summary } from './helpers.js';

console.log('Generator Tests\n');

function* simple() {
  yield 1;
  yield 2;
  yield 3;
}

const gen = simple();
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
  while (true) {
    yield i++;
  }
}

const c = counter(10);
test('counter 10', c.next().value, 10);
test('counter 11', c.next().value, 11);
test('counter 12', c.next().value, 12);

function* range(start, end) {
  for (let i = start; i <= end; i++) {
    yield i;
  }
}

const arr = [...range(1, 5)];
testDeep('spread generator', arr, [1, 2, 3, 4, 5]);

function* twoWay() {
  const x = yield 1;
  const y = yield x + 1;
  yield y + 1;
}

const tw = twoWay();
test('two way first', tw.next().value, 1);
test('two way second', tw.next(10).value, 11);
test('two way third', tw.next(20).value, 21);

summary();
