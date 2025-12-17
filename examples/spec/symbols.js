import { test, summary } from './helpers.js';

console.log('Symbol Tests\n');

const sym1 = Symbol('test');
test('symbol typeof', typeof sym1, 'symbol');

const sym2 = Symbol('test');
test('symbols are unique', sym1 !== sym2, true);

test('symbol description', sym1.description, 'test');

const sym3 = Symbol();
test('symbol without description', sym3.description, undefined);

const obj = {};
const symKey = Symbol('key');
obj[symKey] = 'value';
test('symbol as key', obj[symKey], 'value');

const symFor1 = Symbol.for('shared');
const symFor2 = Symbol.for('shared');
test('Symbol.for same', symFor1 === symFor2, true);

test('Symbol.keyFor', Symbol.keyFor(symFor1), 'shared');
test('Symbol.keyFor local', Symbol.keyFor(sym1), undefined);

test('Symbol.iterator exists', typeof Symbol.iterator, 'symbol');
test('Symbol.toStringTag exists', typeof Symbol.toStringTag, 'symbol');
test('Symbol.hasInstance exists', typeof Symbol.hasInstance, 'symbol');

summary();
