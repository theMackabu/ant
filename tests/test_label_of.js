// Regression test: contextual keywords (of, as, from, async) used as labels
console.log('=== Label with contextual keywords ===');

// Test 1: "of" as a label in a method body
let result1 = 0;
const obj1 = {
  b() {
    of: {
      result1 = 1;
      break of;
      result1 = 99;
    }
  }
};
obj1.b();
console.log('Test 1 (of label in method): ' + result1);
console.log('Expected: 1');

// Test 2: "of" as a label in top-level code
let result2 = 0;
of: {
  result2 = 2;
  break of;
  result2 = 99;
}
console.log('Test 2 (of label top-level): ' + result2);
console.log('Expected: 2');

// Test 3: "as" as a label
let result3 = 0;
as: {
  result3 = 3;
  break as;
  result3 = 99;
}
console.log('Test 3 (as label): ' + result3);
console.log('Expected: 3');

// Test 4: "from" as a label
let result4 = 0;
from: {
  result4 = 4;
  break from;
  result4 = 99;
}
console.log('Test 4 (from label): ' + result4);
console.log('Expected: 4');

// Test 5: "async" as a label
let result5 = 0;
async: {
  result5 = 5;
  break async;
  result5 = 99;
}
console.log('Test 5 (async label): ' + result5);
console.log('Expected: 5');

// Test 6: "of" as a label on a loop
let result6 = '';
of: for (let i = 0; i < 5; i++) {
  if (i === 3) break of;
  result6 += i;
}
console.log('Test 6 (of label on loop): ' + result6);
console.log('Expected: 012');

console.log('\n=== All contextual keyword label tests completed ===');
