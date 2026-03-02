function test1() {
  let obj = {};
  obj.x = 10;
  obj.y = 20;
  return obj.x + obj.y;
}
for (let i = 0; i < 110; i++) test1();
console.log('[test1] object put/get field:', test1(), 'ok:', test1() === 30);

function test2() {
  let obj = {};
  let key = 'hello';
  obj[key] = 42;
  return obj[key];
}
for (let i = 0; i < 110; i++) test2();
console.log('[test2] computed prop access:', test2(), 'ok:', test2() === 42);

function test3(a, b, c) {
  let arr = [a, b, c];
  return arr[0] + arr[1] + arr[2];
}
for (let i = 0; i < 110; i++) test3(1, 2, 3);
console.log('[test3] array literal:', test3(10, 20, 30), 'ok:', test3(10, 20, 30) === 60);

var globalVal = 0;
function test4(n) {
  globalVal = n;
  return globalVal;
}
for (let i = 0; i < 110; i++) test4(i);
console.log('[test4] put global:', test4(99), 'ok:', test4(99) === 99);

function test5(n) {
  let obj = {};
  for (let i = 0; i < n; i++) {
    obj['k' + i] = i;
  }
  return obj.k0 + obj.k1 + obj.k2;
}
for (let i = 0; i < 110; i++) test5(5);
console.log('[test5] loop object build:', test5(5), 'ok:', test5(5) === 3);

function test6(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    let pair = [i, i + 1];
    sum = sum + pair[0] + pair[1];
  }
  return sum;
}
for (let i = 0; i < 110; i++) test6(10);
let r6 = test6(100);
console.log('[test6] array in loop:', r6, 'ok:', r6 === 10000);

function test7() {
  let arr = [0, 0, 0];
  arr[0] = 10;
  arr[1] = 20;
  arr[2] = 30;
  return arr[0] + arr[1] + arr[2];
}
for (let i = 0; i < 110; i++) test7();
console.log('[test7] put_elem numeric:', test7(), 'ok:', test7() === 60);

function test8() {
  let arr = [];
  return arr.length;
}
for (let i = 0; i < 110; i++) test8();
console.log('[test8] empty array:', test8(), 'ok:', test8() === 0);

function test9() {
  let outer = {};
  outer.inner = {};
  outer.inner.val = 42;
  return outer.inner.val;
}
for (let i = 0; i < 110; i++) test9();
console.log('[test9] nested objects:', test9(), 'ok:', test9() === 42);

function reader(obj) {
  return obj.a + obj.b;
}
function test10() {
  let o = {};
  o.a = 3;
  o.b = 7;
  return reader(o);
}
for (let i = 0; i < 110; i++) test10();
console.log('[test10] obj as arg:', test10(), 'ok:', test10() === 10);
