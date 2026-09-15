import { test, summary } from './helpers.js';

console.log('Symbol Tests\n');

const sym1 = Symbol('test');
test('symbol typeof', typeof sym1, 'symbol');

const sym2 = Symbol('test');
test('symbols are unique', sym1 !== sym2, true);

test('symbol description', sym1.description, 'test');

const sym3 = Symbol();
test('symbol without description', sym3.description, undefined);

const descriptionKey = ['descr', 'iption'].join('');
const descriptionWrapper = Object(sym1);
test('boxed symbol inherited description', descriptionWrapper.description, 'test');
test('boxed symbol computed description', descriptionWrapper[descriptionKey], 'test');
Object.defineProperty(descriptionWrapper, 'description', { value: 'own', configurable: true });
test('boxed symbol own description overrides getter', descriptionWrapper.description, 'own');
test('boxed symbol computed own description', descriptionWrapper[descriptionKey], 'own');
delete descriptionWrapper.description;
Object.setPrototypeOf(descriptionWrapper, null);
test('boxed symbol without prototype has no description', descriptionWrapper.description, undefined);
test('boxed symbol computed without prototype', descriptionWrapper[descriptionKey], undefined);

const descriptionDescriptor = Object.getOwnPropertyDescriptor(Symbol.prototype, 'description');
try {
  delete Symbol.prototype.description;
  test('deleted symbol description getter', sym1.description, undefined);
  test('deleted computed symbol description getter', sym1[descriptionKey], undefined);
  Object.defineProperty(Symbol.prototype, 'description', {
    configurable: true,
    get() { return 'replacement'; },
  });
  test('replaced symbol description getter', sym1.description, 'replacement');
  test('replaced computed symbol description getter', sym1[descriptionKey], 'replacement');
} finally {
  Object.defineProperty(Symbol.prototype, 'description', descriptionDescriptor);
}
test('restored symbol description getter', sym1.description, 'test');

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

const custom = { [Symbol.toStringTag]: 'MyCustomType' };
test('Symbol.toStringTag custom', Object.prototype.toString.call(custom), '[object MyCustomType]');
test('Symbol prototype toStringTag', Symbol.prototype[Symbol.toStringTag], 'Symbol');

const iteratorTags = [
  [String, 'String Iterator'],
  [Array, 'Array Iterator'],
  [Map, 'Map Iterator'],
  [Set, 'Set Iterator'],
];

for (const [Ctor, tag] of iteratorTags) {
  const iterProto = Object.getPrototypeOf(new Ctor()[Symbol.iterator]());
  test(`${tag} own toStringTag`, iterProto.hasOwnProperty(Symbol.toStringTag), true);
  test(`${tag} toStringTag value`, iterProto[Symbol.toStringTag], tag);
}

summary();
