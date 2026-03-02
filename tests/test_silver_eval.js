let x = 1 + 2;
console.log('1 + 2 =', x);

let y = x * 10;
console.log('x * 10 =', y);

function add(a, b) {
  return a + b;
}
console.log('add(3, 4) =', add(3, 4));

// closures
function counter() {
  let n = 0;
  return function () {
    n = n + 1;
    return n;
  };
}
let c = counter();
console.log('counter:', c(), c(), c());

// control flow
let sum = 0;
for (let i = 0; i < 10; i = i + 1) {
  sum = sum + i;
}
console.log('sum 0..9 =', sum);

// fibonacci
function fib(n) {
  if (n <= 1) return n;
  return fib(n - 1) + fib(n - 2);
}
console.log('fib(10) =', fib(10));
