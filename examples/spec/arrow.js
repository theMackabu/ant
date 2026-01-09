import { test, summary } from './helpers.js';

console.log('Arrow Function Tests\n');

const basic = (a, b) => a + b;
test('basic arrow function', basic(2, 3), 5);

const single = x => x * 2;
test('arrow single param no parens', single(5), 10);

const singleParen = x => x * 3;
test('arrow single param with parens', singleParen(4), 12);

const noParams = () => 42;
test('arrow no params', noParams(), 42);

const implicit = (a, b) => a - b;
test('arrow implicit return', implicit(10, 3), 7);

const block = (a, b) => {
  const sum = a + b;
  return sum * 2;
};
test('arrow block body', block(2, 3), 10);

const withDefault = (a, b = 10) => a + b;
test('arrow default param used', withDefault(5), 15);
test('arrow default param overridden', withDefault(5, 3), 8);

const defaultExpr = (x = 5 * 2) => x + 1;
test('arrow default param expression', defaultExpr(), 11);
test('arrow default param override expr', defaultExpr(7), 8);

const multiDefault = (a = 1, b = 2, c = 3) => a + b + c;
test('arrow multiple defaults all used', multiDefault(), 6);
test('arrow multiple defaults partial', multiDefault(10), 15);
test('arrow multiple defaults override all', multiDefault(10, 20, 30), 60);

const defaultCall = (x = Math.random()) => x >= 0 && x <= 1;
test('arrow default with call', defaultCall(), true);

const assignmentDefault = (u = 42) => u * 2;
test('arrow assignment in default', assignmentDefault(), 84);

const parenDefault = (x = 5) => x + 1;
test('arrow parenthesized default', parenDefault(), 6);

const complexDefault = (x = (2 + 3) * 4) => x;
test('arrow complex expression default', complexDefault(), 20);

const rest = (first, ...others) => first + others.length;
test('arrow rest params', rest(1, 2, 3, 4), 4);

const restWithDefault = (a = 10, ...rest) => a + rest.length;
test('arrow rest with default', restWithDefault(5, 1, 2), 7);

const objReturn = (a, b) => ({ x: a, y: b });
test('arrow object return x', objReturn(1, 2).x, 1);
test('arrow object return y', objReturn(1, 2).y, 2);

const add = x => y => x + y;
test('arrow currying', add(5)(3), 8);

const fns = [x => x, x => x * 2, x => x * 3];
test('arrow in array [0]', fns[0](5), 5);
test('arrow in array [1]', fns[1](5), 10);
test('arrow in array [2]', fns[2](5), 15);

const obj = {
  double: x => x * 2,
  triple: x => x * 3
};
test('arrow in object double', obj.double(5), 10);
test('arrow in object triple', obj.triple(5), 15);

const context = {
  value: 100,
  getValue: function () {
    return (() => this.value)();
  }
};
test('arrow this binding', context.getValue(), 100);

const mapped = [1, 2, 3].map(x => x * 2);
test('arrow in map [0]', mapped[0], 2);
test('arrow in map [1]', mapped[1], 4);
test('arrow in map [2]', mapped[2], 6);

const filtered = [1, 2, 3, 4, 5].filter(x => x > 2);
test('arrow in filter length', filtered.length, 3);
test('arrow in filter [0]', filtered[0], 3);

const sum = [1, 2, 3, 4].reduce((acc, x) => acc + x, 0);
test('arrow in reduce', sum, 10);

const iife = (x => x * 2)(5);
test('arrow iife', iife, 10);

const iifeTwoLevel = ((u = 10) => u * 2)();
test('arrow iife with default', iifeTwoLevel, 20);

const nested = a => b => c => a + b + c;
test('arrow nested 1', nested(1)(2)(3), 6);

const conditional = x => (x > 0 ? x : -x);
test('arrow conditional positive', conditional(5), 5);
test('arrow conditional negative', conditional(-5), 5);

const logical = (a, b) => (a && b ? a + b : 0);
test('arrow logical true', logical(3, 4), 7);
test('arrow logical false', logical(0, 4), 0);

const template = name => `Hello, ${name}!`;
test('arrow template literal', template('World'), 'Hello, World!');

const makeMultiplier = n => x => x * n;
const double = makeMultiplier(2);
const triple = makeMultiplier(3);
test('arrow returning arrow double', double(5), 10);
test('arrow returning arrow triple', triple(5), 15);

const spreadArrow = (a, b, c) => a + b + c;
test('arrow with spread call', spreadArrow(...[1, 2, 3]), 6);

summary();
