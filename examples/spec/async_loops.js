import { test, summary } from './helpers.js';

console.log('Async Loop Closure Tests\n');

let results = {};

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function testForLetClosure() {
  const fns = [];
  for (let i = 0; i < 3; i++) {
    await delay(5);
    fns.push(() => i);
  }
  return fns.map(f => f()).join(',');
}
testForLetClosure().then(v => {
  results.forLetClosure = v;
});

async function testBlockScopedCapture() {
  const fns = [];
  for (let i = 0; i < 3; i++) {
    const captured = i * 10;
    await delay(5);
    fns.push(() => captured);
  }
  return fns.map(f => f()).join(',');
}
testBlockScopedCapture().then(v => {
  results.blockScopedCapture = v;
});

async function testNestedLoops() {
  const fns = [];
  for (let i = 0; i < 2; i++) {
    for (let j = 0; j < 2; j++) {
      const captured = `${i}-${j}`;
      await delay(5);
      fns.push(() => captured);
    }
  }
  return fns.map(f => f()).join(',');
}
testNestedLoops().then(v => {
  results.nestedLoops = v;
});

async function testWhileLoop() {
  let i = 0;
  const values = [];
  while (i < 3) {
    const captured = i;
    await delay(5);
    values.push(captured);
    i++;
  }
  return values.join(',');
}
testWhileLoop().then(v => {
  results.whileLoop = v;
});

async function testMultipleAwaits() {
  const fns = [];
  for (let i = 0; i < 3; i++) {
    const captured = i;
    await delay(5);
    await delay(5);
    fns.push(() => captured);
  }
  return fns.map(f => f()).join(',');
}
testMultipleAwaits().then(v => {
  results.multipleAwaits = v;
});

async function loopTask(id) {
  const taskResults = [];
  for (let i = 0; i < 3; i++) {
    const captured = `${id}:${i}`;
    await delay(5);
    taskResults.push(() => captured);
  }
  return taskResults.map(fn => fn());
}

async function testSequentialTasks() {
  const a = await loopTask('A');
  const b = await loopTask('B');
  return { a: a.join(','), b: b.join(',') };
}
testSequentialTasks().then(v => {
  results.sequentialTasks = v;
});

async function testAsyncArrowInLoop() {
  const fns = [];
  for (let i = 0; i < 3; i++) {
    const captured = i;
    fns.push(async () => {
      await delay(5);
      return captured;
    });
  }
  const values = await Promise.all(fns.map(f => f()));
  return values.join(',');
}
testAsyncArrowInLoop().then(v => {
  results.asyncArrowInLoop = v;
});

async function testTryCatchInLoop() {
  const values = [];
  for (let i = 0; i < 3; i++) {
    const captured = i;
    try {
      await delay(5);
      values.push(captured);
    } catch (e) {
      values.push('error');
    }
  }
  return values.join(',');
}
testTryCatchInLoop().then(v => {
  results.tryCatchInLoop = v;
});

async function testDoWhile() {
  let i = 0;
  const values = [];
  do {
    const captured = i;
    await delay(5);
    values.push(captured);
    i++;
  } while (i < 3);
  return values.join(',');
}
testDoWhile().then(v => {
  results.doWhile = v;
});

async function testConditionalAwait() {
  const fns = [];
  for (let i = 0; i < 3; i++) {
    const captured = i;
    if (i % 2 === 0) {
      await delay(5);
    }
    fns.push(() => captured);
  }
  return fns.map(f => f()).join(',');
}
testConditionalAwait().then(v => {
  results.conditionalAwait = v;
});

setTimeout(() => {
  test('for-let with async closure', results.forLetClosure, '0,1,2');
  test('block-scoped capture', results.blockScopedCapture, '0,10,20');
  test('nested loops', results.nestedLoops, '0-0,0-1,1-0,1-1');
  test('while loop const', results.whileLoop, '0,1,2');
  test('multiple awaits per iteration', results.multipleAwaits, '0,1,2');
  test('sequential tasks A', results.sequentialTasks.a, 'A:0,A:1,A:2');
  test('sequential tasks B', results.sequentialTasks.b, 'B:0,B:1,B:2');
  test('async arrow in loop', results.asyncArrowInLoop, '0,1,2');
  test('try-catch in loop', results.tryCatchInLoop, '0,1,2');
  test('do-while with const', results.doWhile, '0,1,2');
  test('conditional await', results.conditionalAwait, '0,1,2');
  summary();
}, 200);
