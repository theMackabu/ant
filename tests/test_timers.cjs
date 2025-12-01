// Test setTimeout
console.log('Starting timer tests...');

setTimeout(() => {
  console.log('setTimeout executed after 1000ms');
}, 1000);

setTimeout(() => {
  console.log('setTimeout executed after 500ms');
}, 500);

// Test queueMicrotask
queueMicrotask(() => {
  console.log('Microtask 1 executed');
});

queueMicrotask(() => {
  console.log('Microtask 2 executed');
});

console.log('Synchronous code finished');

// Test setInterval
let count = 0;
const intervalId = setInterval(() => {
  count++;
  console.log('Interval execution #' + count);

  if (count >= 3) {
    clearInterval(intervalId);
    console.log('Interval cleared after 3 executions');
  }
}, 200);

// Test clearTimeout
const timeoutId = setTimeout(() => {
  console.log('This should NOT be printed');
}, 300);

clearTimeout(timeoutId);
console.log('Timeout cleared before execution');
