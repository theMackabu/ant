import assert from 'node:assert';

async function* numbers(n) {
  for (let i = 0; i < n; i++) yield i;
}

async function innerLoop(rounds) {
  let sum = 0;
  for await (const value of numbers(20)) {
    for (let i = 0; i < rounds; i++) sum = (sum + i * value) | 0;
  }
  return sum;
}

async function breakEarly() {
  let sum = 0;
  for await (const value of numbers(100)) {
    if (value === 50) break;
    for (let i = 0; i < 200; i++) sum = (sum + i) | 0;
  }
  return sum;
}

async function throwInside() {
  let sum = 0;
  try {
    for await (const value of numbers(100)) {
      for (let i = 0; i < 200; i++) sum = (sum + i) | 0;
      if (value === 10) throw new Error('stop');
    }
  } catch (err) {
    return `${sum}:${err.message}`;
  }
}

async function returnInside() {
  for await (const value of numbers(100)) {
    let local = 0;
    for (let i = 0; i < 200; i++) local += i;
    if (value === 7) return local + value;
  }
}

let closed = 0;
const closable = {
  [Symbol.asyncIterator]() {
    let i = 0;
    return {
      next: async () => ({ value: i++, done: i > 5 }),
      return: async () => { closed++; return { done: true }; },
    };
  },
};

async function closeOnBreak() {
  let sum = 0;
  for await (const value of closable) {
    for (let i = 0; i < 1000; i++) sum = (sum + i) | 0;
    if (value === 2) break;
  }
  return sum;
}

assert.strictEqual(await innerLoop(100), 940500);
assert.strictEqual(await innerLoop(20000), -656605664);
for (let k = 0; k < 3; k++) assert.strictEqual(await innerLoop(20000), -656605664);
assert.strictEqual(await breakEarly(), 995000);
assert.strictEqual(await throwInside(), '218900:stop');
assert.strictEqual(await returnInside(), 19907);
assert.strictEqual(await closeOnBreak(), 1498500);
assert.strictEqual(closed, 1);

console.log('ok');
