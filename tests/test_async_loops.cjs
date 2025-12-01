// Test async/await with loops
Ant.println('=== Async/Await with Loops Tests ===');

// Test 1: Await in for loop
Ant.println('\nTest 1: Await in for loop');
async function test1() {
  let sum = 0;
  for (let i = 0; i < 3; i = i + 1) {
    const val = await Promise.resolve(i);
    sum = sum + val;
  }
  return sum;
}
test1().then(v => Ant.println('Test 1: ' + v));

// Test 2: Await in while loop
Ant.println('\nTest 2: Await in while loop');
async function test2() {
  let i = 0;
  let sum = 0;
  while (i < 3) {
    const val = await Promise.resolve(i);
    sum = sum + val;
    i = i + 1;
  }
  return sum;
}
test2().then(v => Ant.println('Test 2: ' + v));

// Test 3: Await in do-while loop
Ant.println('\nTest 3: Await in do-while loop');
async function test3() {
  let i = 0;
  let sum = 0;
  do {
    const val = await Promise.resolve(i);
    sum = sum + val;
    i = i + 1;
  } while (i < 3);
  return sum;
}
test3().then(v => Ant.println('Test 3: ' + v));

// Test 4: Multiple awaits in loop
Ant.println('\nTest 4: Multiple awaits in loop');
async function test4() {
  let result = 0;
  for (let i = 0; i < 2; i = i + 1) {
    const a = await Promise.resolve(i);
    const b = await Promise.resolve(i + 10);
    result = result + a + b;
  }
  return result;
}
test4().then(v => Ant.println('Test 4: ' + v));

// Test 5: Await with break
Ant.println('\nTest 5: Await with break');
async function test5() {
  let i = 0;
  while (i < 10) {
    const val = await Promise.resolve(i);
    if (val === 3) {
      break;
    }
    i = i + 1;
  }
  return i;
}
test5().then(v => Ant.println('Test 5: ' + v));

// Test 6: Await with continue
Ant.println('\nTest 6: Await with continue');
async function test6() {
  let sum = 0;
  for (let i = 0; i < 5; i = i + 1) {
    const val = await Promise.resolve(i);
    if (val % 2 === 0) {
      continue;
    }
    sum = sum + val;
  }
  return sum;
}
test6().then(v => Ant.println('Test 6: ' + v));

// Test 7: Nested loops with await
Ant.println('\nTest 7: Nested loops with await');
async function test7() {
  let count = 0;
  for (let i = 0; i < 2; i = i + 1) {
    for (let j = 0; j < 2; j = j + 1) {
      const val = await Promise.resolve(1);
      count = count + val;
    }
  }
  return count;
}
test7().then(v => Ant.println('Test 7: ' + v));

// Test 8: While loop with async condition check
Ant.println('\nTest 8: While loop awaiting values');
async function test8() {
  let i = 0;
  let product = 1;
  while (i < 3) {
    const multiplier = await Promise.resolve(2);
    product = product * multiplier;
    i = i + 1;
  }
  return product;
}
test8().then(v => Ant.println('Test 8: ' + v));

// Test 9: Do-while with await and condition
Ant.println('\nTest 9: Do-while with await');
async function test9() {
  let count = 0;
  let val;
  do {
    val = await Promise.resolve(count);
    count = count + 1;
  } while (val < 2);
  return count;
}
test9().then(v => Ant.println('Test 9: ' + v));

// Test 10: For loop building array with await
Ant.println('\nTest 10: Building array with await in loop');
async function test10() {
  const arr = [];
  for (let i = 0; i < 3; i = i + 1) {
    const val = await Promise.resolve(i * 2);
    arr[i] = val;
  }
  return arr[0] + arr[1] + arr[2];
}
test10().then(v => Ant.println('Test 10: ' + v));

Ant.println('\n=== All async loop tests initiated ===');
