// Test labeled break and continue in loops
console.log('=== Labeled Loop Tests ===');

// Test 1: Labeled break in nested for loops
console.log('\nTest 1: Labeled break in nested for loops');
let result1 = '';
outer1: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    if (i === 1 && j === 1) {
      break outer1;
    }
    result1 += `(${i},${j})`;
  }
}
console.log('Result: ' + result1);
console.log('Expected: (0,0)(0,1)(0,2)(1,0)');

// Test 2: Labeled continue in nested for loops
console.log('\nTest 2: Labeled continue in nested for loops');
let result2 = '';
outer2: for (let i = 0; i < 3; i++) {
  for (let j = 0; j < 3; j++) {
    if (j === 1) {
      continue outer2;
    }
    result2 += `(${i},${j})`;
  }
}
console.log('Result: ' + result2);
console.log('Expected: (0,0)(1,0)(2,0)');

// Test 3: Labeled break in for-of loop
console.log('\nTest 3: Labeled break in for-of loop');
let result3 = '';
const arr1 = [[1,2,3], [4,5,6], [7,8,9]];
outer3: for (const row of arr1) {
  for (const val of row) {
    if (val === 5) {
      break outer3;
    }
    result3 += val + ',';
  }
}
console.log('Result: ' + result3);
console.log('Expected: 1,2,3,4,');

// Test 4: Labeled continue in for-of loop
console.log('\nTest 4: Labeled continue in for-of loop');
let result4 = '';
const arr2 = [[1,2], [3,4], [5,6]];
outer4: for (const row of arr2) {
  for (const val of row) {
    if (val % 2 === 0) {
      continue outer4;
    }
    result4 += val + ',';
  }
}
console.log('Result: ' + result4);
console.log('Expected: 1,3,5,');

// Test 5: Labeled break in for-in loop
console.log('\nTest 5: Labeled break in for-in loop');
let count5 = 0;
let brokeEarly5 = false;
const obj1 = { a: 1, b: 2, c: 3 };
outer5: for (const key1 in obj1) {
  for (const key2 in obj1) {
    count5++;
    if (count5 === 4) {
      brokeEarly5 = true;
      break outer5;
    }
  }
}
console.log('Count: ' + count5 + ', Broke early: ' + brokeEarly5);
console.log('Expected: Count: 4, Broke early: true');

// Test 6: Labeled continue in for-in loop  
console.log('\nTest 6: Labeled continue in for-in loop');
let outerCount6 = 0;
let innerCount6 = 0;
const obj2 = { a: 1, b: 2, c: 3 };
outer6: for (const key1 in obj2) {
  outerCount6++;
  for (const key2 in obj2) {
    innerCount6++;
    continue outer6;  // Should skip rest of inner loop
  }
}
// Each outer iteration should only do 1 inner iteration before continuing outer
console.log('Outer: ' + outerCount6 + ', Inner: ' + innerCount6);
console.log('Expected: Outer: 3, Inner: 3');

// Test 7: Labeled break in while loop
console.log('\nTest 7: Labeled break in while loop');
let result7 = '';
let i7 = 0;
outer7: while (i7 < 3) {
  let j7 = 0;
  while (j7 < 3) {
    if (i7 === 1 && j7 === 1) {
      break outer7;
    }
    result7 += `(${i7},${j7})`;
    j7++;
  }
  i7++;
}
console.log('Result: ' + result7);
console.log('Expected: (0,0)(0,1)(0,2)(1,0)');

// Test 8: Labeled continue in while loop
console.log('\nTest 8: Labeled continue in while loop');
let result8 = '';
let i8 = 0;
outer8: while (i8 < 3) {
  let j8 = 0;
  while (j8 < 3) {
    if (j8 === 1) {
      i8++;
      continue outer8;
    }
    result8 += `(${i8},${j8})`;
    j8++;
  }
  i8++;
}
console.log('Result: ' + result8);
console.log('Expected: (0,0)(1,0)(2,0)');

// Test 9: Triple nested loops with labeled break
console.log('\nTest 9: Triple nested with labeled break');
let result9 = '';
outer9: for (let i = 0; i < 2; i++) {
  middle9: for (let j = 0; j < 2; j++) {
    for (let k = 0; k < 2; k++) {
      if (i === 1 && j === 0 && k === 1) {
        break outer9;
      }
      result9 += `(${i},${j},${k})`;
    }
  }
}
console.log('Result: ' + result9);
console.log('Expected: (0,0,0)(0,0,1)(0,1,0)(0,1,1)(1,0,0)');

// Test 10: Triple nested with middle label break
console.log('\nTest 10: Triple nested with middle label break');
let result10 = '';
outer10: for (let i = 0; i < 2; i++) {
  middle10: for (let j = 0; j < 2; j++) {
    for (let k = 0; k < 2; k++) {
      if (j === 1 && k === 0) {
        break middle10;
      }
      result10 += `(${i},${j},${k})`;
    }
  }
}
console.log('Result: ' + result10);
console.log('Expected: (0,0,0)(0,0,1)(1,0,0)(1,0,1)');

// Test 11: for-of with string iteration and labeled continue
console.log('\nTest 11: for-of string with labeled continue');
let result11 = '';
const strings = ['ab', 'cd', 'ef'];
outer11: for (const str of strings) {
  for (const ch of str) {
    if (ch === 'c' || ch === 'e') {
      continue outer11;
    }
    result11 += ch;
  }
}
console.log('Result: ' + result11);
console.log('Expected: ab');

console.log('\n=== All labeled loop tests completed ===');
