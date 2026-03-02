function testA() {
  function inner(x) {
    return x + 1;
  }
  return inner(41);
}
for (let i = 0; i < 110; i++) testA();
let rA = testA();
console.log('[testA] non-inlined closure call:', rA, 'ok:', rA === 42);

function testB(val) {
  function reader() {
    return val;
  }
  return reader();
}
for (let i = 0; i < 110; i++) testB(i);
let rB = testB(77);
console.log('[testB] upval read via call:', rB, 'ok:', rB === 77);

function makeAdder(x) {
  return function (y) {
    return x + y;
  };
}
for (let i = 0; i < 110; i++) makeAdder(i);
let add5 = makeAdder(5);
let rC = add5(10);
console.log('[testC] returned closure:', rC, 'ok:', rC === 15);

function apply(fn, x) {
  return fn(x);
}
function testD(n) {
  function double_(x) {
    return x + x;
  }
  return apply(double_, n);
}
for (let i = 0; i < 110; i++) testD(i);
let rD = testD(21);
console.log('[testD] closure as argument:', rD, 'ok:', rD === 42);
