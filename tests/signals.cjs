let counter = 0;

Ant.signal('sigint', function (signum) {
  console.log('\nReceived SIGINT (signal', signum, ')');
  console.log('Counter reached:', counter);
  console.log('Shutting down gracefully...');
});

Ant.signal('sigterm', function (signum) {
  console.log('\nReceived SIGTERM (signal', signum, ')');
  console.log('Counter reached:', counter);
  console.log('Terminating...');
});

console.log('Starting counter...');
console.log('Press Ctrl+C to trigger graceful shutdown');

for (;;) {
  counter++;
  if (counter % 1000000 === 0) {
    console.log('Counter:', counter);
  }
}
