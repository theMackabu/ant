import { test, summary } from './helpers.js';

console.log('TCO Bracket Edge Cases\n');

function indexCallResult(n) {
  if (n <= 0) return [42];
  return indexCallResult(n - 1);
}
test('f() then index (small)', indexCallResult(5)[0], 42);

function indexByCall(n) {
  if (n <= 0) return [99];
  return indexByCall(n - 1);
}
function zeroFn() {
  return 0;
}
test('f()[g()] (small)', indexByCall(3)[zeroFn()], 99);

function returnBracketAccess(arr) {
  return arr[0];
}
test('bare bracket access', returnBracketAccess([7]), 7);

function indexArrByCall(arr) {
  return arr[zeroFn()];
}
test('arr[f()] access', indexArrByCall([55]), 55);

const methods = {
  greet() {
    return 'hello';
  },
  farewell() {
    return 'bye';
  }
};
function computedCall(key) {
  return methods[key]();
}
test('obj[key]() simple', computedCall('greet'), 'hello');
test('obj[key]() simple 2', computedCall('farewell'), 'bye');

const ops = {
  dec(n) {
    return recurseViaComputed(n - 1);
  }
};
function recurseViaComputed(n) {
  if (n <= 0) return 'done';
  return ops['dec'](n);
}
test('obj[key]() deep recursion', recurseViaComputed(100000), 'done');

function callWithBracketArg(arr) {
  return identity(arr[0]);
}
function identity(x) {
  return x;
}
test('f(a[0]) bracket in arg', callWithBracketArg([33]), 33);

function callWithBracketBinopArg(arr) {
  return identity(arr[0] + arr[1]);
}
test('f(a[0]+a[1]) binop inside args', callWithBracketBinopArg([10, 20]), 30);

function recurseWithBracketArg(arr, i) {
  if (i >= arr.length) return 0;
  return recurseWithBracketArg(arr, i + 1);
}
test('deep with bracket arg', recurseWithBracketArg(new Array(100000), 0), 0);

const handlers = {};
for (let i = 0; i < 10; i++) {
  handlers['h' + i] = function (n) {
    if (n <= 0) return 'handled';
    return handlers['h' + i](n - 1);
  };
}
test('obj[key+expr]() result', handlers['h0'](50), 'handled');

const dispatch = {
  action_run(n) {
    if (n <= 0) return 'ran';
    return dispatch['action' + '_' + 'run'](n - 1);
  }
};
test('obj[a+b]() deep (may fail without bracket fix)', dispatch['action_run'](100000), 'ran');

const table = [];
for (let i = 0; i < 20; i++) {
  table[i] = function () {
    return i;
  };
}
function callFromTable(a) {
  return table[a * 2 + 1]();
}
test('table[a*2+1]() computed', callFromTable(3), 7);

function ternaryBracket(arr, flag) {
  return flag ? identity(arr[0]) : identity(arr[1]);
}
test('ternary with bracket args true', ternaryBracket([10, 20], true), 10);
test('ternary with bracket args false', ternaryBracket([10, 20], false), 20);

function ternaryComputedVsPlain(flag, n) {
  if (n <= 0) return 'end';
  return flag ? ops['dec'](n) : ternaryComputedVsPlain(flag, n - 1);
}
test('ternary computed vs plain', ternaryComputedVsPlain(false, 100000), 'end');

summary();
