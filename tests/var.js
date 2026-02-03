// ============================================
// COMPREHENSIVE VAR DECLARATION BUG TEST
// ============================================

console.log('=== TEST 1: Regular IIFE (baseline) ===');
(function () {
  var a = 1;
  var b = 2,
    c = 3;
  if (true) {
    var d = 4;
  }
  {
    var e = 5;
  }
  for (var f = 6; f < 7; f++) {
    var g = 7;
  }
  while (false) {
    var h = 8;
  }
  do {
    var i = 9;
  } while (false);
  try {
    var j = 10;
  } catch (err) {
    var k = 11;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' e=' + e + ' f=' + f + ' g=' + g + ' h=' + h + ' i=' + i + ' j=' + j + ' k=' + k);
})();

console.log('\n=== TEST 2: Arrow IIFE ===');
(() => {
  var a = 1;
  var b = 2,
    c = 3;
  if (true) {
    var d = 4;
  }
  {
    var e = 5;
  }
  for (var f = 6; f < 7; f++) {
    var g = 7;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' e=' + e + ' f=' + f + ' g=' + g);
})();

console.log('\n=== TEST 3: User-defined callback ===');
function callFn(cb) {
  cb(100);
}
callFn(function (x) {
  var a = 1;
  var b = 2,
    c = 3;
  if (true) {
    var d = 4;
  }
  for (var f = 6; f < 7; f++) {
    var g = 7;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' f=' + f + ' g=' + g + ' x=' + x);
});

console.log('\n=== TEST 4: User-defined callback (arrow) ===');
callFn(x => {
  var a = 1;
  var b = 2,
    c = 3;
  if (true) {
    var d = 4;
  }
  for (var f = 6; f < 7; f++) {
    var g = 7;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' f=' + f + ' g=' + g + ' x=' + x);
});

console.log('\n=== TEST 5: forEach with regular function ===');
[1].forEach(function (x, idx, arr) {
  var a = 1;
  var b = 2,
    c = 3;
  if (true) {
    var d = 4;
  }
  {
    var e = 5;
  }
  for (var f = 6; f < 7; f++) {
    var g = 7;
  }
  while (false) {
    var h = 8;
  }
  do {
    var i = 9;
  } while (false);
  try {
    var j = 10;
  } catch (err) {
    var k = 11;
  }
  switch (1) {
    case 1:
      var l = 12;
      break;
  }
  console.log(
    '  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' e=' + e + ' f=' + f + ' g=' + g + ' h=' + h + ' i=' + i + ' j=' + j + ' k=' + k + ' l=' + l
  );
});

console.log('\n=== TEST 6: forEach with arrow function ===');
[1].forEach((x, idx, arr) => {
  var a = 1;
  var b = 2,
    c = 3;
  if (true) {
    var d = 4;
  }
  {
    var e = 5;
  }
  for (var f = 6; f < 7; f++) {
    var g = 7;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' e=' + e + ' f=' + f + ' g=' + g);
});

console.log('\n=== TEST 7: map with regular function ===');
[1].map(function (x) {
  var a = 1;
  if (true) {
    var b = 2;
  }
  for (var c = 3; c < 4; c++) {
    var d = 4;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d);
  return x;
});

console.log('\n=== TEST 8: filter with regular function ===');
[1].filter(function (x) {
  var a = 1;
  if (true) {
    var b = 2;
  }
  for (var c = 3; c < 4; c++) {
    var d = 4;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d);
  return true;
});

console.log('\n=== TEST 9: reduce with regular function ===');
[1, 2].reduce(function (acc, x) {
  var a = 1;
  if (true) {
    var b = 2;
  }
  for (var c = 3; c < 4; c++) {
    var d = 4;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' acc=' + acc + ' x=' + x);
  return acc + x;
}, 0);

console.log('\n=== TEST 10: find with regular function ===');
[1].find(function (x) {
  var a = 1;
  for (var b = 2; b < 3; b++) {
    var c = 3;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c);
  return true;
});

console.log('\n=== TEST 11: some with regular function ===');
[1].some(function (x) {
  var a = 1;
  for (var b = 2; b < 3; b++) {
    var c = 3;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c);
  return true;
});

console.log('\n=== TEST 12: every with regular function ===');
[1].every(function (x) {
  var a = 1;
  for (var b = 2; b < 3; b++) {
    var c = 3;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c);
  return true;
});

console.log('\n=== TEST 13: sort with regular function ===');
[2, 1].sort(function (a, b) {
  var v = 1;
  for (var i = 0; i < 1; i++) {
    var w = 2;
  }
  console.log('  v=' + v + ' i=' + i + ' w=' + w);
  return a - b;
});

console.log('\n=== TEST 14: flatMap with regular function ===');
[1].flatMap(function (x) {
  var a = 1;
  for (var b = 2; b < 3; b++) {
    var c = 3;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c);
  return [x];
});

console.log('\n=== TEST 15: findIndex with regular function ===');
[1].findIndex(function (x) {
  var a = 1;
  for (var b = 2; b < 3; b++) {
    var c = 3;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c);
  return true;
});

console.log('\n=== TEST 16: let/const in forEach (should work) ===');
[1].forEach(function (x) {
  let a = 1;
  const b = 2;
  let c = 3,
    d = 4;
  if (true) {
    let e = 5;
    var f = 6;
  }
  for (let g = 7; g < 8; g++) {
    let h = 8;
    var i = 9;
  }
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' f=' + f + ' i=' + i);
});

console.log('\n=== TEST 17: Nested forEach ===');
[1].forEach(function (x) {
  var outer = 'outer';
  [2].forEach(function (y) {
    var inner = 'inner';
    for (var z = 0; z < 1; z++) {
      var inFor = 'inFor';
    }
    console.log('  outer=' + outer + ' inner=' + inner + ' z=' + z + ' inFor=' + inFor);
  });
});

console.log('\n=== TEST 18: setTimeout callback ===');
setTimeout(function () {
  var a = 1;
  if (true) {
    var b = 2;
  }
  for (var c = 3; c < 4; c++) {
    var d = 4;
  }
  console.log('  setTimeout: a=' + a + ' b=' + b + ' c=' + c + ' d=' + d);
}, 0);

console.log('\n=== TEST 19: Promise.then callback ===');
Promise.resolve(1).then(function (x) {
  var a = 1;
  if (true) {
    var b = 2;
  }
  for (var c = 3; c < 4; c++) {
    var d = 4;
  }
  console.log('  Promise.then: a=' + a + ' b=' + b + ' c=' + c + ' d=' + d);
});

console.log('\n=== TEST 20: Object methods ===');
Object.keys({ a: 1 }).forEach(function (k) {
  var v = 'test';
  for (var i = 0; i < 1; i++) {
    var w = 'for';
  }
  console.log('  Object.keys.forEach: v=' + v + ' i=' + i + ' w=' + w);
});

console.log('\n=== TEST 21: String methods ===');
'abc'.split('').forEach(function (c) {
  var v = 'test';
  for (var i = 0; i < 1; i++) {
    var w = 'for';
  }
  console.log('  String.split.forEach: v=' + v + ' i=' + i + ' w=' + w + ' c=' + c);
});

console.log('\n=== TEST 22: Map.forEach ===');
new Map([['a', 1]]).forEach(function (v, k) {
  var a = 1;
  for (var i = 0; i < 1; i++) {
    var b = 2;
  }
  console.log('  Map.forEach: a=' + a + ' i=' + i + ' b=' + b);
});

console.log('\n=== TEST 23: Set.forEach ===');
new Set([1]).forEach(function (v) {
  var a = 1;
  for (var i = 0; i < 1; i++) {
    var b = 2;
  }
  console.log('  Set.forEach: a=' + a + ' i=' + i + ' b=' + b);
});

console.log('\n=== TEST 24: for...of loop ===');
for (var item of [1, 2]) {
  var a = 'test';
  console.log('  for...of: item=' + item + ' a=' + a);
}

console.log('\n=== TEST 25: for...in loop ===');
for (var key in { a: 1, b: 2 }) {
  var v = 'test';
  console.log('  for...in: key=' + key + ' v=' + v);
}

console.log('\n=== TEST 26: Nested functions in forEach ===');
[1].forEach(function (x) {
  var outer = 'outer';
  function inner() {
    var innerVar = 'inner';
    console.log('  nested fn: outer=' + outer + ' innerVar=' + innerVar);
  }
  inner();
  console.log('  forEach body: outer=' + outer);
});

console.log('\n=== TEST 27: var in different for variants ===');
[1].forEach(function (x) {
  for (var a = 0; a < 1; a++) {}
  for (var b in { x: 1 }) {
  }
  for (var c of [1]) {
  }
  var d = 'plain';
  console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d);
});

console.log('\n=== TEST 28: Hoisting test ===');
[1].forEach(function (x) {
  console.log('  before: a=' + a + ' b=' + b);
  var a = 1;
  for (var b = 2; b < 3; b++) {}
  console.log('  after: a=' + a + ' b=' + b);
});

console.log('\n=== TEST 29: Function expression assignment ===');
[1].forEach(function (x) {
  var fn = function () {
    return 'test';
  };
  for (var i = 0; i < 1; i++) {
    var fn2 = function () {
      return 'for';
    };
  }
  console.log('  fn=' + fn + ' fn()=' + (fn ? fn() : 'N/A') + ' fn2()=' + (fn2 ? fn2() : 'N/A'));
});

console.log('\n=== TEST 30: Destructuring var ===');
try {
  [1].forEach(function (x) {
    var [a, b] = [1, 2];
    var { c, d } = { c: 3, d: 4 };
    for (var [e] = [5]; e < 6; e++) {}
    console.log('  a=' + a + ' b=' + b + ' c=' + c + ' d=' + d + ' e=' + e);
  });
} catch (e) {
  console.log('  destructuring error: ' + e.message);
}

console.log('\n=== ASYNC TESTS (will print after) ===');
