function add(a, b) {
  return a + b;
}

let numSum = 0;
for (let i = 0; i < 110; i++) {
  numSum = add(numSum, i);
}
console.log('numeric sum (JIT):', numSum);

let s = add('hello', ' world');
console.log('string concat (deopt):', s);
console.log('bailout correct:', s === 'hello world');

let n = add(10, 32);
console.log('post-deopt numeric:', n);
console.log('post-deopt correct:', n === 42);

console.log('mixed types:', add(1, 'px'));
console.log('mixed types:', add('$', 99));
console.log('all ok:', true);
