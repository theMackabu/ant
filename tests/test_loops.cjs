// Test all loop types: for, while, do-while
console.log('=== Loop Tests ===');

// Test 1: Basic while loop
console.log('\nTest 1: Basic while loop');
let i = 0;
let sum = 0;
while (i < 5) {
  sum = sum + i;
  i = i + 1;
}
console.log('While loop sum (0-4): ' + sum);

// Test 2: While loop with break
console.log('\nTest 2: While loop with break');
let count = 0;
while (count < 10) {
  if (count === 5) {
    break;
  }
  count = count + 1;
}
console.log('While with break, count: ' + count);

// Test 3: While loop with continue
console.log('\nTest 3: While loop with continue');
let j = 0;
let evenSum = 0;
while (j < 10) {
  j = j + 1;
  if (j % 2 === 1) {
    continue;
  }
  evenSum = evenSum + j;
}
console.log('Even sum (2,4,6,8,10): ' + evenSum);

// Test 4: Basic do-while loop
console.log('\nTest 4: Basic do-while loop');
let k = 0;
let result = 0;
do {
  result = result + k;
  k = k + 1;
} while (k < 5);
console.log('Do-while sum (0-4): ' + result);

// Test 5: Do-while executes at least once
console.log('\nTest 5: Do-while executes at least once');
let executed = false;
do {
  executed = true;
  console.log('Do-while executed even when condition is false');
} while (false);

// Test 6: Do-while with break
console.log('\nTest 6: Do-while with break');
let n = 0;
do {
  n = n + 1;
  if (n === 3) {
    break;
  }
} while (n < 10);
console.log('Do-while with break, n: ' + n);

// Test 7: Do-while with continue
console.log('\nTest 7: Do-while with continue');
let m = 0;
let oddSum = 0;
do {
  m = m + 1;
  if (m % 2 === 0) {
    continue;
  }
  oddSum = oddSum + m;
} while (m < 9);
console.log('Odd sum (1,3,5,7,9): ' + oddSum);

// Test 8: For loop basic
console.log('\nTest 8: For loop basic');
let forSum = 0;
for (let x = 0; x < 5; x = x + 1) {
  forSum = forSum + x;
}
console.log('For loop sum (0-4): ' + forSum);

// Test 9: For loop with break
console.log('\nTest 9: For loop with break');
let breakCount = 0;
for (let y = 0; y < 10; y = y + 1) {
  if (y === 5) {
    break;
  }
  breakCount = breakCount + 1;
}
console.log('For loop with break, count: ' + breakCount);

// Test 10: For loop with continue
console.log('\nTest 10: For loop with continue');
let skipSum = 0;
for (let z = 0; z < 10; z = z + 1) {
  if (z % 2 === 0) {
    continue;
  }
  skipSum = skipSum + z;
}
console.log('For loop skip evens (1+3+5+7+9): ' + skipSum);

// Test 11: Nested while loops
console.log('\nTest 11: Nested while loops');
let outer = 0;
let nestedSum = 0;
while (outer < 3) {
  let inner = 0;
  while (inner < 3) {
    nestedSum = nestedSum + 1;
    inner = inner + 1;
  }
  outer = outer + 1;
}
console.log('Nested while loops, count: ' + nestedSum);

// Test 12: Nested for loops
console.log('\nTest 12: Nested for loops');
let product = 0;
for (let a = 1; a <= 3; a = a + 1) {
  for (let b = 1; b <= 3; b = b + 1) {
    product = a * b;
  }
}
console.log('Last nested for product: ' + product);

// Test 13: While with complex condition
console.log('\nTest 13: While with complex condition');
let p = 0;
let q = 10;
while (p < 5 && q > 5) {
  p = p + 1;
  q = q - 1;
}
console.log('Complex while, p: ' + p + ', q: ' + q);

// Test 14: For loop with let declaration
console.log('\nTest 14: For loop with let');
for (let temp = 0; temp < 3; temp = temp + 1) {
  console.log('For loop iteration: ' + temp);
}

// Test 15: While loop with single statement
console.log('\nTest 15: While with single statement');
let single = 0;
while (single < 3) single = single + 1;
console.log('Single statement while: ' + single);

// Test 16: Do-while with single statement (in block)
console.log('\nTest 16: Do-while with single statement');
let singleDo = 0;
do { singleDo = singleDo + 1; } while (singleDo < 3);
console.log('Single statement do-while: ' + singleDo);

// Test 17: Empty while loop
console.log('\nTest 17: Empty while loop');
let empty = 5;
while (empty < 5) {
  console.log('Should not print');
}
console.log('Empty while executed: no output above');

// Test 18: For loop with const
console.log('\nTest 18: For loop counting down');
let countdown = 0;
for (let val = 5; val > 0; val = val - 1) {
  countdown = countdown + val;
}
console.log('Countdown sum (5+4+3+2+1): ' + countdown);

// Test 19: While loop with array
console.log('\nTest 19: While loop with array');
const arr = [10, 20, 30];
let idx = 0;
let arrSum = 0;
while (idx < arr.length) {
  arrSum = arrSum + arr[idx];
  idx = idx + 1;
}
console.log('Array sum via while: ' + arrSum);

// Test 20: Do-while minimal
console.log('\nTest 20: Do-while minimal');
let minimal = 0;
do {
  minimal = minimal + 1;
} while (minimal < 1);
console.log('Minimal do-while: ' + minimal);

console.log('\n=== All loop tests completed ===');
