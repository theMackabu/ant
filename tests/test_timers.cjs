// Test setTimeout
Ant.println('Starting timer tests...');

Ant.setTimeout(() => {
  Ant.println('setTimeout executed after 1000ms');
}, 1000);

Ant.setTimeout(() => {
  Ant.println('setTimeout executed after 500ms');
}, 500);

// Test queueMicrotask
Ant.queueMicrotask(() => {
  Ant.println('Microtask 1 executed');
});

Ant.queueMicrotask(() => {
  Ant.println('Microtask 2 executed');
});

Ant.println('Synchronous code finished');

// Test setInterval
let count = 0;
const intervalId = Ant.setInterval(() => {
  count++;
  Ant.println('Interval execution #' + count);

  if (count >= 3) {
    Ant.clearInterval(intervalId);
    Ant.println('Interval cleared after 3 executions');
  }
}, 200);

// Test clearTimeout
const timeoutId = Ant.setTimeout(() => {
  Ant.println('This should NOT be printed');
}, 300);

Ant.clearTimeout(timeoutId);
Ant.println('Timeout cleared before execution');
