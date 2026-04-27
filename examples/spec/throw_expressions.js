import { test, testThrows, summary } from './helpers.js';

console.log('Throw Expression Tests\n');

const must = value => value || throw new Error('missing');
test('throw expression in arrow skip', must('ok'), 'ok');
testThrows('throw expression in arrow throws', () => must(''));

function fallback(value) {
  return value ?? throw new Error('nullish');
}

test('throw expression in nullish skip', fallback('value'), 'value');
testThrows('throw expression in nullish throws', () => fallback(null));

function choose(value) {
  return value ? value : throw new Error('bad');
}

test('throw expression in conditional skip', choose(7), 7);
testThrows('throw expression in conditional throws', () => choose(0));

function defaultParam(value = throw new Error('default')) {
  return value;
}

test('throw expression in parameter initializer skip', defaultParam(3), 3);
testThrows('throw expression in parameter initializer throws', () => defaultParam());

let caught;
try {
  const value = false || throw 'sentinel';
  void value;
} catch (e) {
  caught = e;
}
test('throw expression throws raw value', caught, 'sentinel');

summary();
