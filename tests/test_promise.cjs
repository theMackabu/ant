Ant.println('Test 1: Promise Constructor');
let p = new Promise((resolve, reject) => {
  resolve(42);
});

Ant.println('p is Promise: ' + (p instanceof Promise));

p.then(v => {
  Ant.println('Resolved with ' + v);
});

Ant.println('Test 2: Chaining');
let p2 = new Promise(resolve => resolve(10));
p2.then(v => {
  return v * 2;
}).then(v => {
  Ant.println('Chained result: ' + v); // Should be 20
});

Ant.println('Test 3: Catch');
let p3 = new Promise((_, reject) => reject('error'));
p3.catch(e => {
  Ant.println('Caught: ' + e);
});

Ant.println('Test 4: Static resolve');
Promise.resolve('static').then(v => {
  Ant.println('Static resolve: ' + v);
});

Ant.println('Test 5: Promise.try');
Promise.try(() => {
  return 'try';
}).then(v => {
  Ant.println('Try result: ' + v);
});

Ant.println('Test 6: Finally');
Promise.resolve('fin').finally(() => {
  Ant.println('Finally called');
});
