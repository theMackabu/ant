function test1() {
  function inner() {
    return 42;
  }
  return inner();
}
for (let i = 0; i < 110; i++) test1();
console.log('[test1] simple inner fn:', test1(), 'ok:', test1() === 42);

function test2(x) {
  function inner() {
    return x;
  }
  return inner();
}
for (let i = 0; i < 110; i++) test2(i);
console.log('[test2] capture param:', test2(99), 'ok:', test2(99) === 99);

function test3() {
  let val = 100;
  function inner() {
    return val;
  }
  return inner();
}
for (let i = 0; i < 110; i++) test3();
console.log('[test3] capture local:', test3(), 'ok:', test3() === 100);

function test4() {
  let count = 0;
  function inc() {
    count = count + 1;
  }
  inc();
  inc();
  inc();
  return count;
}
for (let i = 0; i < 110; i++) test4();
console.log('[test4] mutate upval:', test4(), 'ok:', test4() === 3);

function test5() {
  let n = 0;
  function next() {
    n = n + 1;
    return n;
  }
  next();
  next();
  return next();
}
for (let i = 0; i < 110; i++) test5();
console.log('[test5] counter:', test5(), 'ok:', test5() === 3);

function test6() {
  let x = 10;
  return function () {
    return x;
  };
}
let fn6 = test6();
console.log('[test6] returned closure:', fn6(), 'ok:', fn6() === 10);

function test7() {
  let fns = [];
  for (let i = 0; i < 5; i++) {
    fns.push(function () {
      return i;
    });
  }
  return fns[0]() + fns[1]() + fns[2]() + fns[3]() + fns[4]();
}
console.log('[test7] loop closures:', test7(), 'ok:', test7() === 10);

function test8(n) {
  let sum = 0;
  function addTo(x) {
    sum = sum + x;
  }
  for (let i = 0; i < n; i++) addTo(i);
  return sum;
}
for (let i = 0; i < 110; i++) test8(10);
let r8 = test8(1000);
let e8 = (999 * 1000) / 2;
console.log('[test8] hot closure call:', r8, 'ok:', r8 === e8);
