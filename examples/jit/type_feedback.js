function addNums(a, b) {
  return a + b;
}
for (let i = 0; i < 200; i++) addNums(i, i + 1);
let r1 = addNums(100, 42);
console.log('[fb_num_only ADD]', r1, 'ok:', r1 === 142);

function subNums(a, b) {
  return a - b;
}
for (let i = 0; i < 200; i++) subNums(i * 2, i);
let r2 = subNums(100, 58);
console.log('[fb_num_only SUB]', r2, 'ok:', r2 === 42);

function mulNums(a, b) {
  return a * b;
}
for (let i = 0; i < 200; i++) mulNums(i, 2);
let r3 = mulNums(21, 2);
console.log('[fb_num_only MUL]', r3, 'ok:', r3 === 42);

function divNums(a, b) {
  return a / b;
}
for (let i = 0; i < 200; i++) divNums(i * 3, 3);
let r4 = divNums(84, 2);
console.log('[fb_num_only DIV]', r4, 'ok:', r4 === 42);

function ltNums(a, b) {
  return a < b;
}
for (let i = 0; i < 200; i++) ltNums(i, 100);
console.log('[fb_num_only LT] 3<5:', ltNums(3, 5), 'ok:', ltNums(3, 5) === true);
console.log('[fb_num_only LT] 5<3:', ltNums(5, 3), 'ok:', ltNums(5, 3) === false);

function leNums(a, b) {
  return a <= b;
}
for (let i = 0; i < 200; i++) leNums(i, 100);
console.log('[fb_num_only LE] 3<=3:', leNums(3, 3), 'ok:', leNums(3, 3) === true);
console.log('[fb_num_only LE] 4<=3:', leNums(4, 3), 'ok:', leNums(4, 3) === false);

function gtNums(a, b) {
  return a > b;
}
for (let i = 0; i < 200; i++) gtNums(i, 100);
console.log('[fb_num_only GT] 5>3:', gtNums(5, 3), 'ok:', gtNums(5, 3) === true);
console.log('[fb_num_only GT] 3>3:', gtNums(3, 3), 'ok:', gtNums(3, 3) === false);

function geNums(a, b) {
  return a >= b;
}
for (let i = 0; i < 200; i++) geNums(i, 100);
console.log('[fb_num_only GE] 3>=3:', geNums(3, 3), 'ok:', geNums(3, 3) === true);
console.log('[fb_num_only GE] 2>=3:', geNums(2, 3), 'ok:', geNums(2, 3) === false);

function addStrs(a, b) {
  return a + b;
}
for (let i = 0; i < 200; i++) addStrs('x', 'y');
let r5 = addStrs('hello', ' world');
console.log('[fb_never_num ADD]', r5, 'ok:', r5 === 'hello world');

function ltStrs(a, b) {
  return a < b;
}
for (let i = 0; i < 200; i++) ltStrs('abc', 'def');
console.log("[fb_never_num LT] 'a'<'b':", ltStrs('a', 'b'), 'ok:', ltStrs('a', 'b') === true);
console.log("[fb_never_num LT] 'b'<'a':", ltStrs('b', 'a'), 'ok:', ltStrs('b', 'a') === false);
console.log("[fb_never_num LT] 'z'<'a':", ltStrs('z', 'a'), 'ok:', ltStrs('z', 'a') === false);

function addMixed(a, b) {
  return a + b;
}
for (let i = 0; i < 100; i++) addMixed(i, i);
for (let i = 0; i < 100; i++) addMixed('a', 'b');
let r6 = addMixed(20, 22);
let r7 = addMixed('foo', 'bar');
console.log('[mixed ADD] num:', r6, 'ok:', r6 === 42);
console.log('[mixed ADD] str:', r7, 'ok:', r7 === 'foobar');

function addTrainedNum(a, b) {
  return a + b;
}
for (let i = 0; i < 200; i++) addTrainedNum(i, i);

let r8 = addTrainedNum('bail', 'out');
console.log('[fb_num_only bailout]', r8, 'ok:', r8 === 'bailout');

let r9 = addTrainedNum(10, 32);
console.log('[post-bailout num]', r9, 'ok:', r9 === 42);

function numericLoop(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum = sum + i;
  }
  return sum;
}
for (let i = 0; i < 200; i++) numericLoop(10);
let r10 = numericLoop(10000);
let expected = (9999 * 10000) / 2;
console.log('[numeric loop]', r10, 'ok:', r10 === expected);

function mathChain(a, b, c) {
  let x = a + b;
  let y = x * c;
  let z = y - a;
  return z / b;
}
for (let i = 1; i < 200; i++) mathChain(i, i + 1, 2);
let r11 = mathChain(10, 5, 3);
console.log('[math chain]', r11, 'ok:', r11 === 7);

function countAbove(arr, threshold) {
  let count = 0;
  for (let i = 0; i < arr.length; i++) {
    if (arr[i] > threshold) count = count + 1;
  }
  return count;
}
let nums = [];
for (let i = 0; i < 100; i++) nums[i] = i;
for (let i = 0; i < 200; i++) countAbove(nums, 50);
let r12 = countAbove(nums, 50);
console.log('[comparison loop]', r12, 'ok:', r12 === 49);

function concatLoop(prefix, n) {
  let s = prefix;
  for (let i = 0; i < n; i++) {
    s = s + '.';
  }
  return s;
}
for (let i = 0; i < 200; i++) concatLoop('x', 3);
let r13 = concatLoop('start', 5);
console.log('[string concat loop]', r13, 'ok:', r13 === 'start.....');

function numericInitThenString() {
  let x = 0;
  x = 'safe';
  return x;
}
for (let i = 0; i < 200; i++) numericInitThenString();
let r14 = numericInitThenString();
console.log('[numeric init then string]', r14, 'ok:', r14 === 'safe');

function numericInitBranchString(flag) {
  let x = 0;
  if (flag) x = 's';
  return x + 1;
}
for (let i = 0; i < 200; i++) numericInitBranchString(false);
let r15 = numericInitBranchString(true);
let r16 = numericInitBranchString(false);
console.log('[numeric init branch string]', r15, 'ok:', r15 === 's1');
console.log('[numeric init branch number]', r16, 'ok:', r16 === 1);

function conditionalLocalAdd(flag) {
  let x = flag ? 'p' : 2;
  return x + 1;
}
for (let i = 0; i < 200; i++) conditionalLocalAdd(false);
let r17 = conditionalLocalAdd(true);
let r18 = conditionalLocalAdd(false);
console.log('[conditional local add bailout]', r17, 'ok:', r17 === 'p1');
console.log('[conditional local add warm]', r18, 'ok:', r18 === 3);

function conditionalLocalMul(flag) {
  let x = flag ? 'p' : 2;
  return x * 2;
}
for (let i = 0; i < 200; i++) conditionalLocalMul(false);
let r19 = conditionalLocalMul(true);
let r20 = conditionalLocalMul(false);
console.log('[conditional local mul bailout]', r19, 'ok:', Number.isNaN(r19));
console.log('[conditional local mul warm]', r20, 'ok:', r20 === 4);

let allOk =
  r1 === 142 &&
  r2 === 42 &&
  r3 === 42 &&
  r4 === 42 &&
  ltNums(3, 5) === true &&
  ltNums(5, 3) === false &&
  leNums(3, 3) === true &&
  leNums(4, 3) === false &&
  gtNums(5, 3) === true &&
  gtNums(3, 3) === false &&
  geNums(3, 3) === true &&
  geNums(2, 3) === false &&
  r5 === 'hello world' &&
  ltStrs('a', 'b') === true &&
  r6 === 42 &&
  r7 === 'foobar' &&
  r8 === 'bailout' &&
  r9 === 42 &&
  r10 === expected &&
  r11 === 7 &&
  r12 === 49 &&
  r13 === 'start.....' &&
  r14 === 'safe' &&
  r15 === 's1' &&
  r16 === 1 &&
  r17 === 'p1' &&
  r18 === 3 &&
  Number.isNaN(r19) &&
  r20 === 4;

console.log('');
console.log('all type feedback tests passed:', allOk);
