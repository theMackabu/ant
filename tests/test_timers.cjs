// Test setTimeout
console.log('Starting timer tests...');

Ant.setTimeout(() => {
  console.log('setTimeout executed after 1000ms');
}, 1000);

Ant.setTimeout(() => {
  console.log('setTimeout executed after 500ms');
}, 500);

// Test queueMicrotask
Ant.queueMicrotask(() => {
  console.log('Microtask 1 executed');
});

Ant.queueMicrotask(() => {
  console.log('Microtask 2 executed');
});

console.log('Synchronous code finished');

// Test setInterval
let count = 0;
const intervalId = Ant.setInterval(() => {
  count++;
  console.log('Interval execution #' + count);

  if (count >= 3) {
    Ant.clearInterval(intervalId);
    console.log('Interval cleared after 3 executions');
  }
}, 200);

// Test clearTimeout
const timeoutId = Ant.setTimeout(() => {
  console.log('This should NOT be printed');
}, 300);

Ant.clearTimeout(timeoutId);
console.log('Timeout cleared before execution');
