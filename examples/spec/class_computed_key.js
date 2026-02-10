import { test, summary } from './helpers.js';

console.log('Class Computed Property Key Tests\n');

const key = 'x';
class BasicComputed {
  [key] = 10;
}
test('basic computed field', new BasicComputed().x, 10);

class ComputedExpr {
  ['a' + 'b'] = 42;
}
test('computed field with concat', new ComputedExpr().ab, 42);

let counter = 0;
function makeKey() {
  counter++;
  return 'field' + counter;
}

class SideEffectKey {
  [makeKey()] = 'val';
}
const seInstance = new SideEffectKey();
test('side-effect key evaluated (field name)', seInstance.field1, 'val');
test('side-effect key eval count after class def', counter, 1);

const before = counter;
const se2 = new SideEffectKey();
test('side-effect key not re-evaluated on new instance', counter, before);
test('second instance has same key', se2.field1, 'val');

let order = [];
function track(name) {
  order.push(name);
  return name;
}

class MultiComputed {
  [track('a')] = 1;
  [track('b')] = 2;
  [track('c')] = 3;
}
const mc = new MultiComputed();
test('multi computed field a', mc.a, 1);
test('multi computed field b', mc.b, 2);
test('multi computed field c', mc.c, 3);
test('computed key evaluation order', JSON.stringify(order), JSON.stringify(['a', 'b', 'c']));

class ComputedMethod {
  ['say' + 'Hi']() {
    return 'hello';
  }
}
test('computed method', new ComputedMethod().sayHi(), 'hello');

const prefix = 'get';
const suffix = 'Name';
class ComputedVarExpr {
  [prefix + suffix]() {
    return 'Alice';
  }
}
test('computed method from vars', new ComputedVarExpr().getName(), 'Alice');

class StaticComputed {
  static ['s' + 'val'] = 99;
}
test('static computed field', StaticComputed.sval, 99);

let staticCounter = 0;
function staticKey() {
  staticCounter++;
  return 'sk' + staticCounter;
}

class StaticSideEffect {
  static [staticKey()] = 'static_val';
}
test('static side-effect key', StaticSideEffect.sk1, 'static_val');
test('static side-effect count', staticCounter, 1);

class NumericKey {
  [2 + 3] = 'five';
}
test('numeric computed key', new NumericKey()[5], 'five');

const dynKey = Symbol('dyn');
class SymbolKey {
  [dynKey] = 'symbol_val';
}
test('symbol computed key', new SymbolKey()[dynKey], 'symbol_val');

const flag = true;
class TernaryKey {
  [flag ? 'yes' : 'no'] = 100;
}
test('ternary computed key true', new TernaryKey().yes, 100);

const flag2 = false;
class TernaryKey2 {
  [flag2 ? 'yes' : 'no'] = 200;
}
test('ternary computed key false', new TernaryKey2().no, 200);

function base() {
  return 'item';
}
class CallArithKey {
  [base() + '_' + (1 + 2)] = 'combined';
}
test('call+arith computed key', new CallArithKey().item_3, 'combined');

summary();
