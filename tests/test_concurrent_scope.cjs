// Test concurrent async scope isolation
// Verifies that closure variables survive across await boundaries
// when multiple async functions run concurrently

let passed = 0;
let failed = 0;

function assert(condition, msg) {
  if (condition) {
    passed++;
  } else {
    failed++;
    console.log('FAIL:', msg);
  }
}

// Test 1: Basic concurrent closure variable preservation
async function worker(id, delayMs) {
  const myId = id;
  const myPrefix = 'worker-' + id;
  const myArray = [id, id * 10, id * 100];
  const myObj = { name: myPrefix, value: id * 42 };

  await new Promise(resolve => setTimeout(resolve, delayMs));

  // After await, all closure variables should be intact
  assert(myId === id, `worker ${id}: myId was ${myId}, expected ${id}`);
  assert(myPrefix === 'worker-' + id, `worker ${id}: myPrefix was ${myPrefix}`);
  assert(myArray.length === 3, `worker ${id}: myArray.length was ${myArray.length}`);
  assert(myArray[0] === id, `worker ${id}: myArray[0] was ${myArray[0]}`);
  assert(myArray[2] === id * 100, `worker ${id}: myArray[2] was ${myArray[2]}`);
  assert(myObj.name === myPrefix, `worker ${id}: myObj.name was ${myObj.name}`);
  assert(myObj.value === id * 42, `worker ${id}: myObj.value was ${myObj.value}`);

  return myPrefix;
}

// Test 2: Multiple sequential awaits with closure vars
async function multiAwait(id) {
  const step1 = 'step1-' + id;
  await new Promise(resolve => setTimeout(resolve, 10));

  assert(step1 === 'step1-' + id, `multiAwait ${id} after 1st await: step1 was ${step1}`);

  const step2 = 'step2-' + id;
  await new Promise(resolve => setTimeout(resolve, 10));

  assert(step1 === 'step1-' + id, `multiAwait ${id} after 2nd await: step1 was ${step1}`);
  assert(step2 === 'step2-' + id, `multiAwait ${id} after 2nd await: step2 was ${step2}`);

  const step3 = 'step3-' + id;
  await new Promise(resolve => setTimeout(resolve, 10));

  assert(step1 === 'step1-' + id, `multiAwait ${id} after 3rd await: step1 was ${step1}`);
  assert(step2 === 'step2-' + id, `multiAwait ${id} after 3rd await: step2 was ${step2}`);
  assert(step3 === 'step3-' + id, `multiAwait ${id} after 3rd await: step3 was ${step3}`);

  return [step1, step2, step3];
}

// Test 3: Arrow function closures in async context
async function closureTest(id) {
  const captured = 'captured-' + id;
  const transform = x => captured + '-' + x;

  await new Promise(resolve => setTimeout(resolve, 15));

  const result = transform('test');
  assert(result === 'captured-' + id + '-test', `closureTest ${id}: transform result was ${result}`);
  assert(captured === 'captured-' + id, `closureTest ${id}: captured was ${captured}`);

  return result;
}

// Test 4: Nested async calls with closure isolation
async function outer(id) {
  const outerVar = 'outer-' + id;

  async function inner(suffix) {
    const innerVar = outerVar + '-inner-' + suffix;
    await new Promise(resolve => setTimeout(resolve, 5));
    assert(innerVar === 'outer-' + id + '-inner-' + suffix, `inner ${id}/${suffix}: innerVar was ${innerVar}`);
    assert(outerVar === 'outer-' + id, `inner ${id}/${suffix}: outerVar was ${outerVar}`);
    return innerVar;
  }

  const r1 = await inner('a');
  assert(outerVar === 'outer-' + id, `outer ${id} after inner a: outerVar was ${outerVar}`);

  const r2 = await inner('b');
  assert(outerVar === 'outer-' + id, `outer ${id} after inner b: outerVar was ${outerVar}`);

  return [r1, r2];
}

// Test 5: Parameter preservation across await
async function paramTest(a, b, c) {
  const sum = a + b + c;
  await new Promise(resolve => setTimeout(resolve, 10));

  assert(a + b + c === sum, `paramTest: sum was ${a + b + c}, expected ${sum}`);
  assert(typeof a === 'number', `paramTest: a type was ${typeof a}`);
  assert(typeof b === 'string', `paramTest: b type was ${typeof b}`);
  assert(typeof c === 'boolean', `paramTest: c type was ${typeof c}`);

  return sum;
}

// Launch all tests concurrently
async function main() {
  const promises = [];

  // Launch 5 concurrent workers with different delays
  for (let i = 1; i <= 5; i++) {
    promises.push(worker(i, 10 + i * 5));
  }

  // Launch 3 concurrent multi-await chains
  for (let i = 1; i <= 3; i++) {
    promises.push(multiAwait(i));
  }

  // Launch 3 concurrent closure tests
  for (let i = 1; i <= 3; i++) {
    promises.push(closureTest(i));
  }

  // Launch 3 concurrent nested async tests
  for (let i = 1; i <= 3; i++) {
    promises.push(outer(i));
  }

  // Launch concurrent param tests
  promises.push(paramTest(42, 'hello', true));
  promises.push(paramTest(99, 'world', false));

  await Promise.all(promises);

  console.log(`Results: ${passed} passed, ${failed} failed`);
  if (failed > 0) {
    console.log('FAIL: concurrent scope isolation test');
    Ant.exit(1);
  } else {
    console.log('PASS: concurrent scope isolation test');
  }
}

main();
