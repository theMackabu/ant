let counter = 0;

Ant.signal('sigint', function (signum) {
  Ant.println('\nReceived SIGINT (signal', signum, ')');
  Ant.println('Counter reached:', counter);
  Ant.println('Shutting down gracefully...');
});

Ant.signal('sigterm', function (signum) {
  Ant.println('\nReceived SIGTERM (signal', signum, ')');
  Ant.println('Counter reached:', counter);
  Ant.println('Terminating...');
});

Ant.println('Starting counter...');
Ant.println('Press Ctrl+C to trigger graceful shutdown');

for (;;) {
  counter++;
  if (counter % 1000000 === 0) {
    Ant.println('Counter:', counter);
  }
}
