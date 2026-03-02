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

console.log('\nObject.keys:');
console.log(Object.keys(obj));

console.log('\nObject.getOwnPropertySymbols:');
console.log(Object.getOwnPropertySymbols(obj));

console.log('\nJSON.stringify:');
console.log(JSON.stringify(obj));
