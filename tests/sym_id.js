console.log('=== SYMBOL KEY STRINGIFICATION TEST ===\n');

const obj = {};

obj[Symbol.iterator] = 'iter-value';
obj[Symbol.asyncIterator] = 'async-iter-value';
obj[Symbol.toStringTag] = 'tag-value';
obj[Symbol.hasInstance] = 'has-instance-value';
obj[Symbol.observable] = 'observable-value';
obj[Symbol.toPrimitive] = 'to-primitive-value';
obj[Symbol('custom')] = 'custom-value';
obj[Symbol()] = 'anonymous-value';
obj.normalKey = 'normal-value';

console.log('Object with symbol keys:');
console.log(obj);

console.log('\nIndividual access:');
console.log('  [Symbol.iterator]: ' + obj[Symbol.iterator]);
console.log('  [Symbol.toStringTag]: ' + obj[Symbol.toStringTag]);

console.log('\nObject.keys (should NOT include symbols):');
console.log(Object.keys(obj));

console.log('\nObject.getOwnPropertySymbols (if supported):');
try {
  console.log(Object.getOwnPropertySymbols(obj));
} catch (e) {
  console.log('  Not supported: ' + e.message);
}

console.log('\nJSON.stringify (symbols should be skipped):');
console.log(JSON.stringify(obj));

console.log('\n=== DONE ===');
