import { test, summary } from './helpers.js';

console.log('Function Tests\n');

function add(a, b) {
  return a + b;
}
test('function declaration', add(2, 3), 5);

const multiply = function(a, b) {
  return a * b;
};
test('function expression', multiply(3, 4), 12);

const arrow = (a, b) => a - b;
test('arrow function', arrow(10, 3), 7);

const arrowSingle = x => x * 2;
test('arrow single param', arrowSingle(5), 10);

const arrowBlock = (a, b) => {
  const sum = a + b;
  return sum * 2;
};
test('arrow block body', arrowBlock(2, 3), 10);

function defaultParam(a, b = 10) {
  return a + b;
}
test('default param used', defaultParam(5), 15);
test('default param overridden', defaultParam(5, 3), 8);

function restParams(first, ...rest) {
  return first + rest.length;
}
test('rest params', restParams(1, 2, 3, 4), 4);

function spreadCall(a, b, c) {
  return a + b + c;
}
test('spread in call', spreadCall(...[1, 2, 3]), 6);

const obj = {
  value: 100,
  getValue() {
    return this.value;
  }
};
test('method this', obj.getValue(), 100);

const arrowThis = {
  value: 50,
  getArrow() {
    return (() => this.value)();
  }
};
test('arrow this binding', arrowThis.getArrow(), 50);

function outer() {
  let x = 10;
  return function inner() {
    return x;
  };
}
test('closure', outer()(), 10);

const counter = (() => {
  let count = 0;
  return {
    inc() { return ++count; },
    get() { return count; }
  };
})();
test('iife closure inc', counter.inc(), 1);
test('iife closure get', counter.get(), 1);

function factorial(n) {
  if (n <= 1) return 1;
  return n * factorial(n - 1);
}
test('recursion', factorial(5), 120);

test('function name', add.name, 'add');
test('function length', add.length, 2);

function bindTest(greeting) {
  return greeting + ' ' + this.name;
}
const boundFn = bindTest.bind({ name: 'World' });
test('bind', boundFn('Hello'), 'Hello World');

test('call', bindTest.call({ name: 'Call' }, 'Hi'), 'Hi Call');
test('apply', bindTest.apply({ name: 'Apply' }, ['Hey']), 'Hey Apply');

const gen = (function() {
  const fns = [];
  for (let i = 0; i < 3; i++) {
    fns.push(() => i);
  }
  return fns;
})();
test('closure in loop 0', gen[0](), 0);
test('closure in loop 1', gen[1](), 1);
test('closure in loop 2', gen[2](), 2);

summary();
