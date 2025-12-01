console.log('Test 1: Promise Constructor');
let p = new Promise((resolve, reject) => {
  resolve(42);
});

console.log('p is Promise: ' + (p instanceof Promise));

p.then(v => {
  console.log('Resolved with ' + v);
});

console.log('Test 2: Chaining');
let p2 = new Promise(resolve => resolve(10));
p2.then(v => {
  return v * 2;
}).then(v => {
  console.log('Chained result: ' + v); // Should be 20
});

console.log('Test 3: Catch');
let p3 = new Promise((_, reject) => reject('error'));
p3.catch(e => {
  console.log('Caught: ' + e);
});

console.log('Test 4: Static resolve');
Promise.resolve('static').then(v => {
  console.log('Static resolve: ' + v);
});

console.log('Test 5: Promise.try');
Promise.try(() => {
  return 'try';
}).then(v => {
  console.log('Try result: ' + v);
});

console.log('Test 6: Finally');
Promise.resolve('fin').finally(() => {
  console.log('Finally called');
});
